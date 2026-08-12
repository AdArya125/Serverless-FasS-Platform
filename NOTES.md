# Project Notes: Serverless / FaaS Platform

A self-contained study guide to this project. Read it top to bottom the
first time; use the headers to jump back in for revision later. It
assumes no prior context beyond general programming knowledge - every
term used is defined here or in the glossary (section 17).

---

## 1. What this project is

A miniature **Function-as-a-Service (FaaS)** platform: something in the
spirit of AWS Lambda, Google Cloud Functions, or Azure Functions, but
built small enough that a single person can understand every line of
it. A user registers a function (a container image plus some limits),
invokes it through a CLI or HTTP API, and gets a result back - without
ever running `docker run` themselves or managing the container's
lifecycle.

**The central engineering question this project answers:**

> How can a cloud platform execute short-lived functions efficiently
> while providing isolation, lifecycle management, reliability, and low
> invocation latency?

That question has several parts, and the project is organized around
answering each one concretely, with working code and measured numbers,
rather than just asserting an answer:

- **Isolation** - each function runs inside its own container (or pod).
- **Lifecycle management** - a runtime is created, reused while warm,
  and eventually torn down; the system tracks which state it's in.
- **Reliability** - failures (timeouts, crashed containers, a
  restarted control plane) are detected and recovered from, not just
  hoped against.
- **Low latency** - warm reuse exists specifically to avoid paying
  container-startup cost on every single call.

### What this project deliberately does not try to be

- Not a Kubernetes replacement, and not primarily a scheduling project.
- Not a general microservices framework.
- No fancy web dashboard.
- No multi-region deployment, billing system, or production-grade
  identity/access management.

Keeping the scope this narrow is itself a design decision, not a
limitation to apologize for: a small system with a few mechanisms
implemented *correctly* and *measured* is worth more than a large
system with many mechanisms implemented shallowly. That principle shows
up constantly in the choices below.

---

## 2. Big picture: how a request flows through the system

```
Client / CLI (cloudfn)
        |
        v
API / HTTP Server  (control-plane, listens on :8080)
        |
        +--> Function Registry   (what a function is: image, limits)   -- SQLite
        |
        +--> Runtime Manager     (where a function is running right now)
        |         |
        |         v
        |    ContainerBackend  (Docker or Kubernetes - pluggable)
        |         |
        |         v
        |    Container / Pod running the function image
        |
        +--> Invocation Store    (what happened on each past call)     -- SQLite
        |
        +--> Metrics             (counters/histograms for /metrics)    -- in-memory
```

Two data stores matter here, and they track fundamentally different
things:

- The **function registry** answers "what *is* function `hello`?" -
  its image, its timeout, its memory limit. This is metadata a human
  configured, and it changes rarely.
- The **runtime manager** answers "is function `hello` *currently
  running* somewhere, and where?" This is live, fast-changing state -
  it might be true this second and false five seconds from now (scale
  to zero), or true again a moment after that (recreated on demand).

Confusing these two is the single most common conceptual mistake when
approaching a FaaS system for the first time. Keep them separate in
your head: **registry = declared intent, runtime manager = observed
reality.**

### A concrete walk-through

1. `cloudfn deploy hello --image hello:v1` → `POST /functions` → stored
   in the function registry (SQLite). No container exists yet.
2. `cloudfn invoke hello --data '{"name":"Adi"}'` → `POST
   /functions/hello/invoke`. The runtime manager checks: is there a
   runtime for `hello`? No. So it creates one (`docker run`, wait for
   it to answer `GET /health`), then forwards the request to it over
   HTTP. This whole thing - container creation plus the first real
   call - is a **cold start**, and it's the expensive path (roughly
   1 second in this project's own measurements, see section 15).
3. Invoke `hello` again immediately. The runtime manager finds the
   existing container, still healthy, and just forwards the request
   directly - no container creation. This is a **warm start**, and
   it's roughly 1,000-2,000x cheaper than the cold start above,
   because the only work left is a single HTTP round trip.
4. Stop invoking `hello`. After its configured idle timeout (default
   60 seconds, but configurable per function), a background thread
   notices the runtime has been idle too long and removes the
   container. The function is now **scaled to zero** - no container is
   running for it, though it is still registered.
5. Invoke `hello` again. There's no runtime, so this repeats step 2: a
   fresh cold start, completely automatically. The caller did not have
   to know or care that scale-to-zero had happened.

That five-step cycle - cold start, warm reuse, scale to zero, recreate
on demand - is the single most important behavior in the whole project.
Everything else (persistence, metrics, Kubernetes support) supports or
measures that cycle; none of it replaces it.

---

## 3. Technology choices, and why

| Choice | What was picked | Why |
|---|---|---|
| Control-plane language | C++17 | Matches the project's systems-programming goal; forces you to actually understand sockets, threads, and memory rather than getting them for free from a framework. |
| HTTP server/client | Hand-written, on raw POSIX sockets (`common/`) | Request parsing *is* one of the mechanics this project is meant to make visible. Also removes an external dependency, which matters for "reproduces cleanly on another machine." |
| JSON | `nlohmann/json` (one real dependency) | Writing a correct JSON parser from scratch is a lot of unrelated work for very little learning value once you've already written an HTTP parser. This is the one place effort was spent on the platform logic instead of the plumbing. |
| Persistence | SQLite via a small hand-written RAII wrapper (`database.cpp`), not an ORM | Same reasoning as the HTTP layer: this project explicitly favors "write the plumbing, understand it" over "import a library that hides the mechanism." |
| Container orchestration | Shell out to the `docker` / `kubectl` CLI, not a client library | Both CLIs are already well-documented, stable interfaces. Shelling out to them is simpler and more transparent than linking against the Docker Engine API or the Kubernetes client library, and it makes the two backends symmetrical (see section 11). |
| Testing | A ~40-line hand-written test framework (`tests/test_framework.hpp`), no GoogleTest/Catch2 | The same dependency-minimalism reasoning again, and the project's actual testing needs (register a test function, assert on it) don't need a heavyweight framework. |

The theme across every one of these choices: **prefer writing the small
amount of plumbing yourself over importing a library that would hide
the mechanism you're trying to learn**, except where the plumbing is
genuinely large and unrelated to the platform's own logic (JSON
parsing). That's a judgment call, and it's worth being able to explain
*why* each dependency was or wasn't taken - that's exactly the kind of
question a reviewer of this project would ask.

---

## 4. The function model

A function is described by a `FunctionSpec` (`control-plane/include/faas/function.hpp`):

```cpp
struct FunctionSpec {
    std::string name;
    int version = 1;
    std::string image;
    int timeout_ms = 5000;
    int memory_mb = 256;
    double cpu = 0.5;
    int max_concurrency = 1;
    long idle_timeout_ms = 60000;
};
```

Only `name` and `image` are required at deploy time; everything else
has a sensible default. `FunctionRegistry` stores these in a SQLite
table (`functions`), keyed by `name` - deploying a name that already
exists overwrites it, which is how re-deploys work.

Note: `memory_mb`, `cpu`, and `max_concurrency` are captured and stored,
but not yet *enforced* by this project (no cgroup limits are actually
applied, and no concurrency queueing exists). That's an honest,
documented gap, not an oversight - the spec this project follows marks
concurrency controls as an optional later extension, not core scope.
Knowing what your own system does *not* do is as important as knowing
what it does.

---

## 5. The function contract

A function is an ordinary container image that, once started, exposes
two HTTP endpoints on port 8080:

```
GET  /health   -> 200 once ready to serve traffic
POST /invoke   -> Content-Type: application/json, arbitrary JSON body
                  responds with a JSON value (any type - string, object,
                  array...)
```

The platform passes whatever the function returns straight back to the
caller as the `result` field of the invoke response, unmodified. It
does not assume a particular shape - this keeps the platform generic
rather than coupled to one function's conventions.

Two reference functions exist in `functions/`:

- **`hello`** - the trivial workload. Given `{"name": "Adi"}`, returns
  `"Hello, Adi!"`. Used to measure pure platform overhead, since the
  function itself does no real work.
- **`sleep`** - given `{"sleep_ms": N}`, sleeps for `N` milliseconds
  before responding. Used to deliberately trigger timeouts and to
  create controllable load for the concurrency experiment.

Both are written in plain Python using only the standard library's
`http.server` - no framework, again for minimal setup burden on anyone
reproducing the project.

---

## 6. Invocation lifecycle (the heart of the project)

This is implemented in `RuntimeManager` (`control-plane/src/runtime_manager.cpp`).
It owns a map from function name to (at most) one `Runtime`:

```cpp
struct Runtime {
    std::string container_id;
    int host_port = 0;
    RuntimeState state = RuntimeState::STARTING;
    std::chrono::steady_clock::time_point last_used_at;
    long idle_timeout_ms = 0;
};
```

### Cold start, step by step

1. `invoke()` is called. The manager locks its mutex and checks: is
   there an entry for this function name? No.
2. It calls `backend_->run_container(image, 8080)` - for Docker, this
   is `docker run -d -p 8080 <image>`, letting Docker auto-assign a
   host port; for Kubernetes, see section 11.
3. It calls `backend_->get_host_port(...)` to learn which host port to
   talk to.
4. It polls `GET /health` on that port every 100ms, up to 50 times
   (5 seconds), until the container answers. This is a genuine
   readiness check, not just "the process started."
5. Once healthy, the runtime is inserted into the map, marked `BUSY`,
   and the lock is released.
6. *Only now* does the manager make the actual `POST /invoke` call to
   the container, using the function's configured `timeout_ms` as a
   socket-level read timeout (see "timeout enforcement" below).
7. The result comes back; the runtime is marked `IDLE`; the response
   (including `cold_start: true` and the total elapsed time, which
   includes all of steps 2-6) goes back to the caller.

### Warm reuse - and why it skips the health check

On a later call, step 1 finds an existing entry. The obvious thing to
do would be to health-check it again before reusing it - "make sure
it's still alive." This project deliberately does **not** do that,
because it would cost a full extra network round trip on *every single
warm call*, just to confirm something that was true a moment ago.

Instead, the manager trusts a warm runtime and only reacts if the real
invocation itself fails to reach it:

- If the call **cannot connect at all** (the container crashed, was
  OOM-killed, or was manually removed) - that's "the warm runtime
  turned out to be dead." The manager tears it down and **retries the
  invocation exactly once** against a freshly created runtime. The
  caller sees a normal successful response, just with `cold_start:
  true` - not an error caused by the platform's own bookkeeping being
  stale.
- If the call **times out** - the container did answer the connection,
  so it's presumably alive and just slow or stuck. This is *not*
  retried: retrying a call that's guaranteed to time out again would
  only double the caller's wait for nothing.
- If the *retry itself* also fails to connect - that's a genuinely
  broken image, and it is not retried a second time, to avoid looping
  forever on something that cannot work.

This is implemented as two functions: `try_invoke()` does one attempt
(create-or-reuse, then call), and `invoke()` wraps it, calling it twice
only in the specific "warm and dead" case described above.

### Timeout enforcement

Each function has a `timeout_ms`. The manager applies this as a
socket-level receive timeout (`SO_RCVTIMEO`) on the HTTP call to the
container - implemented in the shared HTTP client
(`common/src/http_client.cpp`), not something Docker or Kubernetes
provides for you. If the timeout fires, the result is reported as
`"timeout"`, and - importantly - **the runtime is torn down, not
reused**, because a container that's still running something when its
caller gave up is not something you can safely hand to the next
request.

---

## 7. Runtime state machine

```
STARTING -> READY -> BUSY -> IDLE -> TERMINATED
               |        |       |
               v        v       v
             FAILED   FAILED  FAILED
```

- **STARTING** - container requested, not yet accepting traffic. Not
  observable through the API in this implementation: creation happens
  while the manager holds its lock, and any status query blocks on
  that same lock, so by the time a query can run, creation has already
  finished one way or another.
- **READY** - passed its readiness check, not yet handling a request.
- **BUSY** - actively executing an invocation.
- **IDLE** - warm, available for reuse.
- **FAILED** - timed out, or turned out to be unreachable when
  actually invoked. Never reused - removed immediately.
- **TERMINATED** - stopped, either by the idle-timeout reaper
  (scale-to-zero) or because it failed. There's no lingering
  "terminated" record; a function with no runtime just has no entry in
  the manager's map. That absence *is* the terminated state.

---

## 8. Scale-to-zero

A background thread inside `RuntimeManager` wakes up every 500ms and
checks every tracked runtime: if it's `IDLE` and has been idle longer
than *its own function's* `idle_timeout_ms`, it's removed.

The word "own" matters: each runtime remembers the idle timeout that
was configured for its function at creation time, so two functions can
scale down on completely independent schedules - deploying `hello` with
`--idle-timeout 5s` and `long-running-job` with `--idle-timeout 5m`
does not make them interfere with each other.

Because "no runtime for this function" is simply the absence of a map
entry (see section 7), scale-to-zero requires no special-case code on
the invoke path - `invoke()` already handles "no runtime exists yet" as
its normal cold-start case. Scaling to zero and a function's very first
invocation are, from the code's point of view, the *same situation*.
That's a nice example of a good abstraction: you didn't need to write
extra logic for "handle the recreate-after-scale-to-zero case" because
the existing "handle the doesn't-exist-yet case" already covers it.

---

## 9. Persistence

Two things are persisted to a single SQLite file (`faas.db` by default,
`FAAS_DB_PATH` to override): the function registry, and a history of
every invocation (`InvocationStore`). Both go through a small shared
`Database`/`Statement` wrapper (`control-plane/src/database.cpp`) built
directly on the SQLite C API.

The one subtlety worth understanding: `Statement` holds the database's
lock for its **entire lifetime**, from `prepare` through the last
`step`, not just for one call. This means two threads can never
interleave binds/steps on the same connection - simple and safe, at the
cost of not being maximally concurrent. For a control plane whose
registry/history operations are infrequent compared to invocations
themselves, that's a reasonable trade, not a shortcut.

### What's deliberately *not* persisted

Live runtime state (which containers are currently running) is **not**
saved anywhere. After a restart, the control plane has no way to verify
whether a container it no longer remembers about is still healthy - so
rather than guess, it starts with zero known runtimes and lets the next
invocation for each function cold-start normally, exactly as if that
function had simply been idle. This is a deliberate, documented
decision: reconciling actual Docker/Kubernetes state with the control
plane's view of the world on startup is a real feature (and future
work), not something to fake by assuming stale data is still correct.

---

## 10. Observability

`GET /metrics` exposes a small, hand-rolled Prometheus-format registry
(`control-plane/src/metrics.cpp`) - not a general metrics library, just
the specific series this project's own experiments need:

- `faas_active_runtimes` (gauge) - read fresh from the runtime manager
  at scrape time.
- `faas_invocations_total{status, cold_start}` (counter) - raw material
  for success/error/timeout rates and the cold-vs-warm mix.
- `faas_invocation_duration_ms{cold_start}` (histogram, fixed buckets
  from 1ms to 10s) - deliberately split by cold/warm, so a tool like
  Prometheus/Grafana's `histogram_quantile()` can report cold and warm
  p95/p99 as two separate numbers, which is exactly what the
  evaluation experiments need to report.
- `faas_runtimes_created_total` / `faas_runtimes_terminated_total{reason}`
  (counters) - runtime churn, with `reason` distinguishing
  `idle_timeout` (ordinary scale-to-zero), `timeout`, `dead`
  (self-healing recovery), and `deleted`.

If you haven't seen Prometheus's text format before: it's just
whitespace-separated `metric_name{label="value"} number` lines, with
`# HELP`/`# TYPE` comment lines above each metric family. A histogram
in this format is several `_bucket` lines (one per boundary, each
counting *all* observations less than or equal to that boundary -
"cumulative"), plus a `_sum` and a `_count` line.

---

## 11. Kubernetes backend

This is the project's clearest demonstration of a real architecture
principle: the runtime manager does not talk to Docker directly. It
talks to an abstract interface:

```cpp
class ContainerBackend {
public:
    struct RunResult { bool ok; std::string id; std::string error; };
    virtual RunResult run_container(const std::string& image, int port) = 0;
    virtual int get_host_port(const std::string& id, int port) = 0;
    virtual bool remove_container(const std::string& id) = 0;
    virtual ~ContainerBackend() = default;
};
```

`DockerClient` and `KubernetesClient` both implement this. `RuntimeManager`
holds a `std::unique_ptr<ContainerBackend>`, chosen once at startup
(`FAAS_RUNTIME_BACKEND=docker` or `kubernetes`/`k8s`), and from that
point on it has **no idea** which one it's talking to. Every mechanism
described above - the state machine, warm reuse, timeout enforcement,
scale-to-zero - works identically regardless.

This is a direct, provable illustration of the project specification's
own framing: *Kubernetes is the infrastructure layer, and this platform
is the serverless abstraction sitting above it.* Swapping the
infrastructure layer required zero changes to any lifecycle logic -
that's what "abstraction" is supposed to buy you, made concrete instead
of just asserted.

If this general pattern has a name you'll see in software design
material, it's the **Strategy pattern** (or, more generally,
**dependency inversion**: the high-level policy - RuntimeManager -
depends on an interface, not on a concrete low-level implementation).

### How `KubernetesClient` actually works

A bare Kubernetes pod has no Docker-style "auto-assigned host port," so
each runtime here is a **pod + NodePort Service pair**, both sharing one
generated name that doubles as the `ContainerBackend` id:

```
create : kubectl run <name> --image=<image> --port=<port> \
             --image-pull-policy=IfNotPresent --restart=Never
         kubectl wait --for=condition=Ready pod/<name>
         kubectl expose pod <name> --type=NodePort

port   : kubectl get svc <name> -o jsonpath={.spec.ports[0].nodePort}

remove : kubectl delete svc <name>
         kubectl delete pod <name>
```

Two assumptions worth knowing:

- **The cluster is reachable at `127.0.0.1`.** A NodePort is exposed on
  every node's IP; the runtime manager always connects to
  `127.0.0.1:<port>`, correct for a single-node k3s cluster on the same
  machine as the control plane (this project's target setup), but
  wrong for a remote or multi-node cluster without adjustment.
- **Images must exist in the cluster's own image store**, not
  Docker's - k3s runs its own embedded `containerd`, entirely separate
  from the Docker daemon. An image built with `docker build` has to be
  imported (`docker save hello:v1 | sudo k3s ctr images import -`)
  before a pod can use it, and `--image-pull-policy=IfNotPresent` is
  what stops Kubernetes from trying (and failing) to pull it from a
  registry instead.

Measured once: a Kubernetes cold start took about 2.5 seconds versus
Docker's about 1 second - the extra `kubectl` round trips (create,
wait, expose, then a separate query for the port) add up. Warm reuse
was identical either way, about 1ms, because a warm call is just an
HTTP request to an already-known port regardless of backend.

---

## 12. API reference

Full detail in `docs/api.md`; summary here.

| Method | Path | What it does |
|---|---|---|
| GET | `/health` | Liveness check |
| POST | `/functions` | Register/redeploy a function |
| GET | `/functions/{name}` | Fetch function metadata |
| DELETE | `/functions/{name}` | Remove a function, and immediately tear down any running container/pod for it |
| POST | `/functions/{name}/invoke` | Invoke - cold-starts if needed, reuses if warm |
| GET | `/functions/{name}/runtime` | Inspect the one runtime currently backing a function, if any |
| GET | `/runtimes` | List every active runtime across all functions |
| GET | `/invocations/{id}` | Look up a past invocation by id |
| GET | `/metrics` | Prometheus text-format metrics |

Every invoke response includes `status`, `duration_ms`, `cold_start`,
and an `invocation_id` you can look up later. `status` is one of:
`"success"` (2xx from the function), `"error"` (non-2xx - a *user
function* failure, the runtime stays warm), `"timeout"` (runtime torn
down), or, at the HTTP level, a `502` for `"infra_error"` (the platform
itself couldn't run the function at all).

---

## 13. CLI reference

| Command | Does |
|---|---|
| `cloudfn deploy <name> --image <img> [--timeout 5s] [--memory 256Mi] [--cpu 0.5] [--concurrency 1] [--idle-timeout 60s]` | Register a function |
| `cloudfn describe <name>` | Show its metadata |
| `cloudfn invoke <name> --data '<json>'` | Invoke it |
| `cloudfn invocation <id>` | Look up a past invocation |
| `cloudfn runtimes` | List every active runtime |
| `cloudfn delete <name>` | Remove it (and its runtime) |
| `cloudfn logs <name>` | Not implemented (documented gap, not silently missing) |

The CLI talks to the control plane's HTTP API (`FAAS_API`, default
`127.0.0.1:8080`) - it is not a special client with privileged access;
anything it can do, a `curl` command could do too.

---

## 14. Testing strategy

Three tiers, each with a different job:

1. **Unit tests** (`tests/control_plane/*.cpp`, run via `make test`) -
   fast, no Docker required. Test `FunctionRegistry`, `InvocationStore`,
   and `Metrics` in isolation, using an in-memory (`:memory:`) SQLite
   database so they don't touch disk.
2. **Integration tests** (`tests/integration/*.sh`, `make
   test-integration` etc.) - exercise the real thing: build a real
   image, start a real control plane, hit the real HTTP API, and
   assert on real Docker/Kubernetes state (`docker ps`, `kubectl get
   pods`). Slower, but these are the tests that actually catch "does
   this work end to end," which unit tests alone cannot.
3. **Benchmarks** (`benchmarks/`, see section 15) - not pass/fail tests
   at all; they exist to produce numbers, not verdicts.

This three-way split mirrors a real engineering practice: fast tests
you run constantly while developing, slower tests that prove the whole
system actually works together, and measurement scripts that answer
"how well" rather than "does it work."

---

## 15. Evaluation results (summary)

Full methodology and tables in `benchmarks/README.md`. Four experiments,
matching the project specification's required set:

- **Cold vs warm start**: warm reuse measured roughly **1,000-2,000x
  faster** than a cold start for a trivial function (cold start
  ≈1.0-1.1s median; warm ≈0.5-1ms median).
- **Concurrency**: throughput plateaus around 12-17 req/s regardless of
  concurrency level (1 through 50), and latency grows roughly linearly
  with it - the direct, measured cost of keeping exactly one runtime
  per function rather than a pool.
- **Scale-to-zero**: time-to-reach-zero tracks the configured idle
  timeout closely; recovery latency after scale-to-zero matches
  ordinary cold-start latency, because recreating a scaled-to-zero
  runtime *is* a cold start.
- **Failure injection**: a killed warm container and a forced timeout
  both recover in ≈1.07s (both are cold starts under the hood); a
  control-plane process restart is back to healthy in ≈14ms, with
  function metadata and invocation history intact.

The single biggest lesson these numbers demonstrate: **warm reuse is
the dominant lever for latency**, which is exactly why the
idle-timeout choice behind scale-to-zero is a genuine trade-off (faster
resource reclamation vs. more frequent cold starts) and not a free
optimization.

---

## 16. Key design decisions and trade-offs (a "why" glossary)

Interviewers and reviewers tend to ask "why did you do X" questions.
Here are the ones this project's own design invites, with the honest
answer to each:

- **Why one runtime per function instead of a pool?** Simplicity and
  correctness first. A pool is explicitly called out in the project
  specification as an "advanced extension," not core scope. The
  concurrency experiment (section 15) exists specifically to show,
  with numbers, what this choice costs - rather than leaving it as an
  unmeasured assumption.
- **Why no proactive health check on the warm path?** It would double
  network round trips on every single warm call to defend against a
  failure mode (a dead container) that's actually rare. Instead, the
  system detects that failure reactively (the real call itself fails)
  and recovers with a single retry - cheaper in the common case,
  without giving up correctness in the rare case.
- **Why hold the manager's lock across the entire cold-start path?**
  Correctness before performance. It does mean invocations across
  *different* functions can momentarily block on each other while one
  of them is cold-starting - a known, documented limitation to revisit
  if concurrency handling becomes a priority, not something hidden.
- **Why is runtime state not persisted, but function/invocation
  metadata is?** Because "trustworthy" and "persisted" aren't the same
  thing. Function metadata a human configured is still true after a
  restart. Whether a specific container is still alive is not
  something you can know without checking - so rather than persist a
  potentially-false belief, the system just doesn't claim to know, and
  re-derives the truth (via a fresh cold start) on demand.
- **Why shell out to `docker`/`kubectl` instead of using client
  libraries?** Both CLIs are stable, well-documented interfaces that
  already exist on any machine with Docker/Kubernetes installed;
  linking against their respective API client libraries would add
  dependency weight for a project that otherwise deliberately keeps
  dependencies minimal, without adding much learning value over "run
  the command and parse the output."

---

## 17. Glossary

- **Cold start** - the latency path when a function has no running
  runtime: create a container/pod, wait for it to become healthy, then
  serve the request. Expensive relative to a warm call.
- **Warm start** - the latency path when a healthy runtime already
  exists: skip creation, just forward the request. Cheap.
- **Scale-to-zero** - a serverless platform's ability to run zero
  containers for a function with no traffic, rather than keeping one
  running (and consuming resources) permanently "just in case."
- **Idle timeout** - how long a runtime is allowed to sit unused before
  it's stopped as part of scale-to-zero.
- **Runtime** - this project's term for "a specific running
  container/pod currently serving a specific function." Distinct from
  the function itself, which can exist (be registered) with zero
  runtimes.
- **NodePort (Kubernetes)** - a type of Kubernetes Service that opens a
  specific port on every cluster node, forwarding traffic on that port
  to the pod(s) behind the Service. Used here because it gives a
  Docker-style "host-reachable port" without needing an Ingress
  controller.
- **containerd** - the low-level container runtime that both Docker and
  Kubernetes/k3s use under the hood - but as *separate instances* with
  separate image stores, which is why an image built with `docker
  build` isn't automatically visible to a k3s cluster.
- **Prometheus text exposition format** - the plain-text format
  Prometheus scrapes metrics in: `metric_name{labels} value` lines,
  optionally preceded by `# HELP`/`# TYPE` comments.
- **RAII** (Resource Acquisition Is Initialization) - a C++ idiom where
  a resource (a lock, a database statement, a file handle) is tied to
  an object's lifetime: acquired in the constructor, released in the
  destructor, so it can't be forgotten even if an exception is thrown.
  Used throughout this project's `Database`/`Statement` and lock
  handling.
- **Strategy pattern / dependency inversion** - a design where
  high-level logic (here, `RuntimeManager`) depends on an abstract
  interface (`ContainerBackend`) rather than a concrete implementation,
  so the implementation can be swapped without touching the logic that
  uses it.

---

## 18. Study questions (self-check)

Try answering these without looking back at the sections above, then
check yourself.

1. What's the difference between the function registry and the runtime
   manager, and why are they separate?
2. Walk through exactly what happens, in order, on the very first
   invocation of a newly deployed function.
3. Why doesn't the platform health-check a warm runtime before reusing
   it? What does it do instead when that runtime turns out to be dead?
4. What's the difference in how a timeout is handled versus how a dead
   connection is handled, and why are they treated differently?
5. What state is a function's runtime in immediately after five minutes
   of no traffic (assuming a 60-second idle timeout)? What does the
   API return if you query its runtime status at that point?
6. Why is runtime state not persisted across a control-plane restart,
   while function metadata is?
7. Name the three metric types exposed via `/metrics` and what each one
   answers.
8. What interface does `RuntimeManager` depend on, and why does that
   let the same code run against both Docker and Kubernetes?
9. Why is a Kubernetes cold start slower than a Docker cold start in
   this implementation specifically (not "Kubernetes in general")?
10. What did the concurrency experiment actually measure, and what
    architectural decision does its result illustrate?

---

*This document describes the project as implemented; if you change the
code, keep this in sync with it rather than letting it drift into
describing a version of the system that no longer exists.*
