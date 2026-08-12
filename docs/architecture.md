# Architecture

## Layers

```
Client / CLI (cloudfn)
        |
        v
API / HTTP Server (control-plane)
        |
        v
Function Registry  --  Runtime Manager  --  Invocation Store
        |                    |                     |
        v                    v                     v
      SQLite            docker containers        SQLite
```

The **function registry** tracks what a function is (image, limits,
version) and the **invocation store** tracks what happened on each past
call; both are SQLite-backed and survive a control-plane restart. The
**runtime manager** tracks where a function is currently running: which
container, on which port, in which state - this is deliberately *not*
persisted (see "Persistence" below). A function can be registered with
zero runtimes running, and usually is, most of the time.

## Why the control plane is C++ with no third-party frameworks

- The HTTP server and client (`common/`) are built directly on POSIX
  sockets instead of a vendored library. This project is meant to make
  serverless *mechanics* visible, and request parsing is one of them; it
  also keeps the only build dependencies to `g++`, `make`,
  `libsqlite3-dev`, and `nlohmann-json3-dev`, all standard distro
  packages, so the project reproduces cleanly on another machine.
- JSON parsing uses `nlohmann/json` (industry standard, header-only) so
  effort goes into the platform logic rather than reinventing a parser.

## Function contract (what a function image must do)

A function image is an ordinary Docker container that, once started,
listens on port 8080 and accepts:

```
GET  /health          -> 200 once the function is ready to serve traffic
POST /invoke           Content-Type: application/json, arbitrary JSON body
```

`/invoke` should respond with a JSON value - the platform passes it back
to the caller unchanged as the `result` field of the invoke response.
The value can be any JSON type (string, object, array, ...); the
platform does not assume a particular shape. A 2xx response is a
success; anything else is treated as a user function failure rather
than a platform failure.

The runtime manager talks to whichever runtime is backing a function -
Docker container or Kubernetes pod - exactly like any other HTTP client
on a host-reachable port, polling `/health` until it responds before
forwarding the first request. `functions/hello` is the reference
implementation of this contract.

## Runtime state machine

```
STARTING -> READY -> BUSY -> IDLE -> TERMINATED
               |        |       |
               v        v       v
             FAILED   FAILED  FAILED
```

- **STARTING**: container requested from Docker, not yet accepting
  requests. Not observable through the API today: runtime creation
  happens while the runtime manager holds its single lock, and a status
  query blocks on that same lock, so by the time it can run, creation
  has already finished one way or the other.
- **READY**: container is up and passed a readiness check.
- **BUSY**: currently executing an invocation.
- **IDLE**: warm and available for reuse.
- **FAILED**: timed out, or its container turned out to be unreachable
  when actually invoked (a health check is only used while a fresh
  container is starting up, not on every reuse - see below). A failed
  runtime is never reused; the runtime manager removes its container and
  drops it immediately rather than leaving it around in a broken state.
- **TERMINATED**: stopped and removed, either due to the idle timeout
  (scale-to-zero) or because it failed. There is no lingering
  "terminated" record - a function with no runtime simply has no entry
  in the runtime manager's table, which is also its state right after
  registration, before the first invocation.

## Warm reuse

An existing runtime is reused directly, with no proactive health check
before the call - that check would cost a full extra round trip on
every single warm invocation just to confirm something that was already
true moments ago. Instead, the runtime manager trusts a warm runtime
until an actual invocation proves it wrong:

- If the call cannot reach the container at all (it crashed, was OOM
  killed, or was removed out from under the platform), that specific
  failure is treated as "the warm runtime turned out to be dead," not as
  a caller-visible error. The dead runtime is torn down and the
  invocation is retried exactly once against a freshly created one - the
  caller sees a normal successful response with `cold_start: true`,
  not a failure that happened to be the platform's own bookkeeping
  falling behind reality.
- A timeout is not treated this way and is not retried: the container
  answered the connection, so it is presumably still alive and just slow
  or stuck, and retrying a guaranteed-timeout call would only double the
  caller's wait for no benefit.
- A genuinely broken image (one where even a fresh container cannot be
  reached) is not retried a second time either - retrying indefinitely
  against something that cannot work would just hide the failure behind
  extra latency instead of reporting it.

## Timeout enforcement and idle expiry

Each invocation applies the function's `timeout_ms` as a socket-level
read timeout on the call to the container. A function that does not
respond in time is reported as `"timeout"` (see `docs/api.md`), and its
runtime is stopped and removed - a runtime that may still be stuck
running something is not a safe thing to hand to the next invocation.

Separately, a background thread in the runtime manager wakes up every
500ms and stops any runtime that has been `IDLE` for longer than *its
function's own* idle timeout - each function carries its own
`idle_timeout_ms` (set at deploy time, defaulting to
`FAAS_IDLE_TIMEOUT_MS`, itself defaulting to 60000ms), copied onto the
runtime when it is created, so two functions can scale down on
completely different schedules without affecting each other. This is
the scale-to-zero mechanism: a function with no traffic naturally drops
to zero running containers, and the next invocation recreates one on
demand exactly like a first-ever cold start.

`GET /functions/{name}/runtime` is a direct window into this for one
function - invoke it, watch it go `IDLE`, then watch it disappear once
its idle timeout elapses. `GET /runtimes` gives the same view across
every function at once, which is what a benchmark script would poll to
plot active-runtime count over time (see `benchmarks/`).

## Persistence

Function metadata (`FunctionRegistry`) and invocation history
(`InvocationStore`) are both backed by a single SQLite database
(`faas.db` by default, overridable with `FAAS_DB_PATH`), through a small
shared `Database`/`Statement` wrapper (`control-plane/src/database.cpp`)
built directly on the SQLite C API rather than an ORM - consistent with
the rest of the project's "write the plumbing, not the business logic"
approach to dependencies. A `Statement` holds the database's lock for
its entire lifetime, from prepare through the last step, so two threads
can never interleave binds/steps on the same connection; this is simple
rather than maximally concurrent, which is an acceptable trade for a
single-process control plane whose registry/history operations are
infrequent compared to invocations.

What is deliberately *not* persisted is runtime state: which containers
are currently running. After a restart, the control plane has no way to
verify whether a container it no longer remembers is still healthy, so
it does not try to adopt orphaned containers - it starts with zero known
runtimes and lets the next invocation for each function cold-start
normally, exactly as if that function had simply been idle. Any
containers left running from before the restart become orphaned and are
not cleaned up automatically; reconciling actual Docker state with the
control plane's view of the world on startup is future work.

## Observability

`GET /metrics` exposes a small, purpose-built Prometheus registry
(`control-plane/src/metrics.cpp`) rather than a general metrics library -
there is no dynamic metric registration; it tracks exactly the handful
of series this project's own evaluation experiments need (see
`docs/api.md` for the full list):

- `faas_active_runtimes` (gauge) - read fresh from the runtime manager at
  scrape time, not tracked incrementally.
- `faas_invocations_total{status, cold_start}` (counter) - the raw
  material for success/error/timeout rates and cold-vs-warm mix.
- `faas_invocation_duration_ms{cold_start}` (histogram, fixed 1ms-10s
  buckets) - split by cold/warm specifically so
  `histogram_quantile(0.95, ...)` in Prometheus/Grafana can report cold
  and warm p95/p99 as separate numbers, which is what the cold-start
  experiment (`benchmarks/`) actually needs to report.
- `faas_runtimes_created_total` / `faas_runtimes_terminated_total{reason}`
  (counters) - runtime creation/destruction rate, with `reason` in
  `idle_timeout` (scale-to-zero), `timeout`, or `dead` (self-healing
  recovery), so a spike in `dead` terminations is visible as a distinct
  signal from ordinary scale-to-zero.

Metrics live in-process and reset on restart; only function metadata and
invocation history are persisted (see "Persistence" above). Recording
happens directly inside `RuntimeManager` (it already computes status,
cold/warm, and duration for every call and creation/termination
decision) rather than being reconstructed from the outside in
`main.cpp`.

`observability/prometheus.yml` is a ready-made scrape config, and
`deploy/docker/docker-compose.yml` runs Prometheus against it
(`network_mode: host`, so it can reach the control plane on
`localhost:8080` without extra networking setup):

```bash
docker compose -f deploy/docker/docker-compose.yml up -d
# Prometheus UI: http://localhost:9090
```

Grafana is not wired up - the spec marks it optional, and a dashboard is
more useful once there is real experimental data to point it at
(`benchmarks/`, still to come) rather than as an empty shell now.

## Container backends: Docker and Kubernetes

The runtime manager does not create containers directly - it goes
through a `ContainerBackend` interface (`create`/`get_host_port`/
`remove`), and everything documented above (state machine, warm reuse,
timeout enforcement, scale-to-zero) works identically no matter which
implementation it was given. This is a direct expression of the project
specification's own framing: Kubernetes is the infrastructure layer,
and the FaaS platform is the serverless abstraction sitting above it -
provable by the fact that swapping the infrastructure layer requires
zero changes to the lifecycle logic.

Two backends exist, selected at startup via `FAAS_RUNTIME_BACKEND`
(`docker`, the default, or `kubernetes`/`k8s`):

- **`DockerClient`** shells out to `docker run` / `docker port` /
  `docker rm -f`, unchanged from earlier.
- **`KubernetesClient`** shells out to `kubectl` instead. Since there is
  no Docker-style "auto-assigned host port" concept for a bare pod, each
  runtime is actually a **pod + NodePort Service pair**, both named the
  same generated name, which doubles as the `ContainerBackend` id:
  - create: `kubectl run <name> --image=<image> --port=<port> --image-pull-policy=IfNotPresent --restart=Never`, then `kubectl wait --for=condition=Ready pod/<name>`, then `kubectl expose pod <name> --type=NodePort`
  - get port: `kubectl get svc <name> -o jsonpath={.spec.ports[0].nodePort}`
  - remove: `kubectl delete svc <name>` and `kubectl delete pod <name>`

  Both backends share the same small shell-exec helpers
  (`control-plane/src/shell_exec.cpp`: run a command, capture output,
  reject unsafe tokens before they reach a shell) rather than duplicating
  that plumbing - the same "shell out to a well-known CLI instead of
  vendoring an API client" choice made for Docker applies equally well
  to `kubectl`.

Two things this assumes, worth knowing before relying on it:

- **The cluster is reachable at `127.0.0.1`.** A NodePort is exposed on
  every cluster node's IP; the runtime manager always connects to
  `127.0.0.1:<port>`, which is correct for a single-node k3s cluster
  running on the same machine as the control plane (this project's
  target setup) but would need the node's real IP for a remote or
  multi-node cluster.
- **Images must already exist in the cluster's image store**, not
  Docker's - k3s runs its own embedded `containerd`, entirely separate
  from the Docker daemon. An image built with `docker build` (as
  `functions/*/Dockerfile` are) needs to be imported before a pod can
  use it:
  ```bash
  docker save hello:v1 | sudo k3s ctr images import -
  ```
  `--image-pull-policy=IfNotPresent` on pod creation is what makes
  `kubectl run` use that imported image instead of trying to pull
  `hello:v1` from a registry that does not have it.

Measured once against a real k3s cluster: a Kubernetes cold start took
~2.5s versus Docker's ~1s (the extra `kubectl` round trips - create,
wait, expose, then query the NodePort - add up), while warm reuse was
identical at ~1ms in both, since a warm call is just an HTTP request to
an already-known port either way. A real comparison across many runs
belongs in `benchmarks/`, not as a one-off number here, but the
direction is exactly what you'd expect from adding a scheduler and a
Service in front of the same underlying container start.

## Repository layout

```
serverless-faas-platform/
  common/          shared HTTP server/client (POSIX sockets)
  control-plane/   API, function registry, runtime manager, state
  cli/             cloudfn command-line client
  functions/       sample function images (hello, sleep, ...)
  tests/           unit tests, shared test framework
  benchmarks/      experiment scripts
  deploy/          docker-compose / kubernetes manifests
  observability/   prometheus/grafana config
  docs/            this file, api.md
  scripts/         build.sh, run.sh, test.sh
```
