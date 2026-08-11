# Serverless / FaaS Platform

A miniature Function-as-a-Service platform: register a function, invoke
it through a CLI or HTTP API, and get a result back without managing the
underlying container yourself. Built incrementally as a systems project
to make serverless mechanics (cold/warm starts, runtime lifecycle,
scale-to-zero, failure recovery) visible and measurable, rather than to
reproduce AWS Lambda's feature set.

See `docs/architecture.md` for the design and `docs/api.md` for the HTTP
API contract.

## Status

Function registration, deployment, and invocation all work end to end:
`cloudfn invoke` creates a Docker container on first use, reuses it on
later calls, and reports timing and whether the call was a cold or warm
start. Runtimes carry a real state machine (see `docs/architecture.md`):
invocations that exceed a function's `timeout_ms` are cut off and their
runtime torn down rather than reused, and a background reaper stops any
runtime that has sat idle past its function's own `idle_timeout_ms` - a
function with no traffic drops to zero running containers and a later
invocation recreates one on demand, independently of every other
function's schedule. The warm path skips any proactive health check
(one less round trip per call); if a warm runtime turns out to be dead,
the platform retries once against a fresh one instead of surfacing an
error. `GET /runtimes` (`cloudfn runtimes`) lists every active runtime
platform-wide, for watching scale-to-zero happen in real time.

- [x] Skeleton (repo, API contract, CLI, function model)
- [x] Single runtime (create container, invoke, return result)
- [x] Lifecycle (state machine, timeout, idle expiry)
- [x] Warm reuse (no pre-flight health check, self-healing retry on a dead runtime)
- [x] Scale-to-zero (per-function idle timeout, active-runtime listing)
- [ ] Persistence (SQLite)
- [ ] Observability (Prometheus)
- [ ] Kubernetes/k3s
- [ ] Evaluation (benchmarks)

## Requirements

- `g++` with C++17 support, `make`
- `libsqlite3-dev` (needed once persistence is implemented)
- `nlohmann-json3-dev`
- Docker (needed once the runtime manager creates containers)

On Debian/Ubuntu:

```bash
sudo apt install g++ make libsqlite3-dev nlohmann-json3-dev docker.io
```

## Build and run

```bash
make build          # builds control-plane and cli
make test           # builds and runs the unit tests
make run            # builds and starts the control plane on :8080
```

Or with the equivalent scripts:

```bash
./scripts/build.sh
./scripts/test.sh
./scripts/run.sh
```

The control plane listens on `0.0.0.0:8080` by default; override with
`FAAS_PORT`. The CLI reads the control plane address from `FAAS_API`
(default `127.0.0.1:8080`). `FAAS_IDLE_TIMEOUT_MS` sets the default idle
timeout (default 60000ms) for functions deployed without an explicit
`--idle-timeout`.

## CLI usage

Function images run as ordinary Docker containers, so build them first:

```bash
make functions   # builds hello:v1 and sleep:v1
```

Then, with the control plane running (`make run`, in another terminal):

```bash
$ cloudfn deploy hello --image hello:v1
deployed hello

$ cloudfn describe hello
Name:        hello
Version:     1
Image:       hello:v1
Timeout:     5000ms
Memory:      256Mi
CPU:         0.5
Concurrency: 1
Idle timeout: 60000ms
Status:      READY

$ cloudfn invoke hello --data '{"name":"Adi"}'
status: success
result: "Hello, Adi!"
duration: 930 ms
cold_start: true

$ cloudfn invoke hello --data '{"name":"Adi"}'
status: success
result: "Hello, Adi!"
duration: 0 ms
cold_start: false

$ cloudfn delete hello
deleted hello
```

The first invocation starts a container (cold start); as long as it
stays healthy, later invocations reuse it directly (warm start). Use
`--idle-timeout` at deploy time to set a per-function scale-to-zero
window (e.g. `--idle-timeout 30s`), and `cloudfn runtimes` to see every
currently active runtime:

```bash
$ cloudfn runtimes
hello	IDLE	port=32772	idle_ms=1832
```

The `cloudfn` binary is built at `cli/build/cloudfn`; add it to your
`PATH` or invoke it by full path.

## Testing

Unit tests live in `tests/` and use a small header-only test framework
(`tests/test_framework.hpp`) rather than a third-party dependency. Run
them with `make test` - they do not need Docker.

Four integration tests exercise Docker directly and are not part of
`make test`:

- `make test-integration` builds the hello image, deploys it, invokes it
  twice, and checks that the first call is a cold start and the second
  is a warm start.
- `make test-lifecycle` builds the sleep image and checks timeout
  enforcement (a slow invocation is cut off and its runtime removed),
  the `IDLE` state after a successful call, idle-expiry scale-to-zero,
  and that a later invocation recreates the runtime.
- `make test-warm-reuse` kills a warm runtime's container out from under
  the platform and checks that the next invocation recovers
  transparently (a fresh cold start reported as a normal success)
  instead of surfacing an error.
- `make test-scale-to-zero` deploys the same image under two names with
  different idle timeouts and checks that each scales to zero on its own
  schedule, independently, as seen through `GET /runtimes`.

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
  docs/            architecture and API documentation
  scripts/         build.sh, run.sh, test.sh
```

## Why this project

Central engineering question: how can a platform execute short-lived
functions efficiently while providing isolation, lifecycle management,
reliability, and low invocation latency? The project prioritizes getting
a small set of mechanisms right - cold/warm starts, scale-to-zero,
failure isolation - over feature breadth, and backs them with measured
experiments rather than claims.
