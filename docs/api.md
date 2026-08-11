# API Contract

The control plane exposes a plain HTTP/JSON API on port 8080 by default
(override with the `FAAS_PORT` environment variable).

## Status legend

- `done` - implemented and tested
- `stub` - route exists and returns `501 Not Implemented` with an
  explanatory message; implemented later
- `planned` - not yet added

| Method | Path                        | Status  | Description                          |
|--------|-----------------------------|---------|---------------------------------------|
| GET    | `/health`                   | done    | Liveness check                        |
| POST   | `/functions`                | done    | Register/redeploy a function          |
| GET    | `/functions/{name}`         | done    | Fetch function metadata               |
| DELETE | `/functions/{name}`         | done    | Remove a function                     |
| POST   | `/functions/{name}/invoke`  | done    | Invoke a function                     |
| GET    | `/functions/{name}/runtime` | done    | Inspect the current runtime, if any   |
| GET    | `/runtimes`                 | done    | List all currently active runtimes    |
| GET    | `/invocations/{id}`         | stub    | Fetch a past invocation record        |
| GET    | `/metrics`                  | planned | Prometheus text-format metrics        |

## POST /functions

Registers a function. Registering a name that already exists replaces it
(this is how re-deploys work).

Request body:

```json
{
  "name": "hello",
  "image": "hello:v1",
  "version": 1,
  "timeout_ms": 5000,
  "memory_mb": 256,
  "cpu": 0.5,
  "max_concurrency": 1,
  "idle_timeout_ms": 60000
}
```

Only `name` and `image` are required; the rest default to the values
shown above. `idle_timeout_ms`'s default can be changed platform-wide
with the `FAAS_IDLE_TIMEOUT_MS` environment variable, without redeploying
every function; a value in the request body always overrides it for that
one function.

Response: `201 Created` with the stored function record, or `400 Bad
Request` if `name`/`image` are missing or the body is not valid JSON.

## GET /functions/{name}

Response: `200 OK` with the function record, or `404 Not Found`.

```json
{
  "name": "hello",
  "version": 1,
  "image": "hello:v1",
  "timeout_ms": 5000,
  "memory_mb": 256,
  "cpu": 0.5,
  "max_concurrency": 1,
  "idle_timeout_ms": 60000,
  "status": "READY"
}
```

## DELETE /functions/{name}

Response: `204 No Content`, or `404 Not Found`.

## POST /functions/{name}/invoke

Request body: arbitrary JSON, passed through to the function unchanged.
An empty body is treated as `{}`.

If no runtime is currently running for the function, one is created
(`docker run`) and polled for readiness before the request is
forwarded - this is a cold start. If a runtime already exists, it is
reused directly with no extra health check - a warm start. If that
warm runtime turns out to be dead (crashed or removed since the last
call), the platform transparently retries once against a freshly
created runtime rather than surfacing an error - the response still
reports `"success"`, just with `cold_start: true`. A runtime that times
out is torn down and not retried (see `docs/architecture.md` for why).

Response on `200 OK` (the function ran, whether or not it succeeded):

```json
{
  "status": "success",
  "result": "Hello, Adi!",
  "duration_ms": 18,
  "cold_start": false
}
```

`status` is one of:

- `"success"` - the function returned a 2xx response. `result` holds
  whatever JSON value it returned, unmodified; if the function did not
  return valid JSON, `result` holds the raw response text instead.
- `"error"` - the function returned a non-2xx response (a user function
  failure; the runtime itself is healthy and stays warm). `result` holds
  its response body the same way as above.
- `"timeout"` - the function did not respond within the function's
  configured `timeout_ms`. `error` holds a message instead of `result`.
  The runtime is stopped and removed, not reused.

`duration_ms` covers the whole call, so a cold start's duration includes
container creation and the readiness wait.

Response on `502 Bad Gateway` (the platform could not run the function
at all - an infrastructure failure, e.g. the image does not exist or
the container never became healthy):

```json
{
  "status": "error",
  "error": "failed to start container: ...",
  "duration_ms": 4108,
  "cold_start": true
}
```

Response on `404 Not Found` if the function is not registered.

## GET /functions/{name}/runtime

Inspects the runtime currently backing a function, if any. Useful for
observing lifecycle transitions directly (e.g. watching a runtime go
`IDLE` after a call, then disappear once the idle timeout reaps it).

Response on `200 OK`:

```json
{
  "state": "IDLE",
  "container_id": "2d629e7ca249...",
  "host_port": 32772,
  "idle_ms": 1832
}
```

`state` is one of `STARTING`, `READY`, `BUSY`, `IDLE`, `FAILED`,
`TERMINATED` (see `docs/architecture.md`); in practice only `BUSY` and
`IDLE` are observable through this endpoint today, since runtime
creation happens under a lock that a status query also waits on, and
failed/terminated runtimes are removed immediately rather than kept
around in either state.

Response on `404 Not Found` if the function is not registered, or if no
runtime is currently running for it (never invoked yet, or scaled to
zero after being idle).

## GET /runtimes

Lists every currently active runtime across all functions - a
platform-wide view of scale-to-zero: a busy platform has many entries,
an idle one has few or none. Useful for watching the total active count
change over time (see `benchmarks/`).

Response on `200 OK`:

```json
[
  {
    "function": "hello",
    "state": "IDLE",
    "container_id": "2d629e7ca249...",
    "host_port": 32772,
    "idle_ms": 1832
  }
]
```

An empty array means no runtimes are currently active anywhere on the
platform.

## GET /invocations/{id}

Planned response, once invocation history is implemented: the stored
invocation record (function name, input, output, status, timing,
cold/warm flag).

## Errors

All error responses share the same shape:

```json
{ "error": "human readable message" }
```
