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
version). The **runtime manager** (added next) tracks where a function is
currently running: which container, on which port, in which state. These
are deliberately separate concerns: a function can be registered with
zero runtimes running.

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
  requests.
- **READY**: container is up and passed a readiness check.
- **BUSY**: currently executing an invocation.
- **IDLE**: warm and available for reuse.
- **FAILED**: crashed, timed out, or failed a health check. A failed
  runtime is never reused; the runtime manager replaces it.
- **TERMINATED**: stopped and removed, either due to the idle timeout
  (scale-to-zero) or because it failed.

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
  functions/       sample function images (hello, cpu-test, ...)
  tests/           unit tests, shared test framework
  benchmarks/      experiment scripts
  deploy/          docker-compose / kubernetes manifests
  observability/   prometheus/grafana config
  docs/            this file, api.md
  scripts/         build.sh, run.sh, test.sh
```
