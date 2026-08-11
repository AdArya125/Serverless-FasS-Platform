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
  "max_concurrency": 1
}
```

Only `name` and `image` are required; the rest default to the values
shown above.

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
forwarded - this is a cold start. If a healthy runtime already exists,
it is reused directly - a warm start. A runtime that fails a health
check is discarded and replaced rather than reused.

Response on `200 OK` (the function ran, whether or not it succeeded):

```json
{
  "status": "success",
  "result": "Hello, Adi!",
  "duration_ms": 18,
  "cold_start": false
}
```

`status` is `"success"` if the function returned a 2xx response, or
`"error"` if it returned a non-2xx response (a user function failure).
`result` is whatever JSON value the function returned, unmodified; if
the function did not return valid JSON, `result` holds the raw response
text instead. `duration_ms` covers the whole call, so a cold start's
duration includes container creation and the readiness wait.

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

## GET /invocations/{id}

Planned response, once invocation history is implemented: the stored
invocation record (function name, input, output, status, timing,
cold/warm flag).

## Errors

All error responses share the same shape:

```json
{ "error": "human readable message" }
```
