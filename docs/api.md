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
| POST   | `/functions/{name}/invoke`  | stub    | Invoke a function                     |
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

Planned response, once invocation is implemented:

```json
{
  "status": "success",
  "result": "Hello, Adi!",
  "duration_ms": 18,
  "cold_start": false
}
```

## GET /invocations/{id}

Planned response, once invocation history is implemented: the stored
invocation record (function name, input, output, status, timing,
cold/warm flag).

## Errors

All error responses share the same shape:

```json
{ "error": "human readable message" }
```
