# Architecture

## Layers

```
Client / CLI (cloudfn)
        |
        v
API / HTTP Server (control-plane)
        |
        v
Function Registry  --  Runtime Manager  --  Invocation Records
        |                    |                     |
        v                    v                     v
  function metadata     docker containers      SQLite (added later)
```

The **function registry** tracks what a function is (image, limits,
version). The **runtime manager** tracks where a function is currently
running: which container, on which port, in which state. These are
deliberately separate concerns: a function can be registered with zero
runtimes running, and usually is, most of the time.

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

The runtime manager lets Docker auto-assign a host port for the
container's internal 8080 and talks to it exactly like any other HTTP
client, polling `/health` until it responds before forwarding the first
request. `functions/hello` is the reference implementation of this
contract.

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
500ms and stops any runtime that has been `IDLE` for longer than the
idle timeout (`FAAS_IDLE_TIMEOUT_MS`, default 60000ms). This is the
scale-to-zero mechanism: a function with no traffic naturally drops to
zero running containers, and the next invocation recreates one on
demand exactly like a first-ever cold start. `GET
/functions/{name}/runtime` is a direct window into this - invoke a
function, watch it go `IDLE`, then watch it disappear once the idle
timeout elapses.

## Docker now, Kubernetes later

The roadmap (see `README.md`) builds and fully tests the platform
against the local Docker daemon first, then ports the runtime manager to
run pods on k3s/Kubernetes instead. This matches the project
specification's own priority ordering: Kubernetes is the "Final" phase,
not a prerequisite for the core mechanics.

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
