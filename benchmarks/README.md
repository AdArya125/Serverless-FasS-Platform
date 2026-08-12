# Evaluation

Four experiments, matching the project specification's required set
(section 4): cold/warm start latency, concurrency, the scale-to-zero
trade-off, and failure injection. Each script talks to a running
control plane over its HTTP API and writes raw JSON results into its
own `results/` directory, committed as evidence alongside this summary -
the numbers below are one real run (see the matching timestamped file in
each `results/` directory), reproducible by rerunning the scripts, not
hand-picked or invented.

## Running them

```bash
make functions
./scripts/run.sh   # in another terminal

python3 benchmarks/cold-start/run.py
python3 benchmarks/concurrency/run.py
python3 benchmarks/scale-to-zero/run.py
./benchmarks/failures/run.sh   # manages its own control-plane process
```

`benchmarks/common.py` holds the shared HTTP client (standard library
only) and percentile calculation used by the three Python scripts.
`failures/run.sh` is a shell script instead, since it needs to start and
kill the control-plane process itself (for the restart scenario) rather
than assuming one is already running.

## Results (one run, 2026-08-12)

### Cold start vs warm start

Ten trials per workload; each cold trial forces a fresh runtime by
deleting and redeploying the function first (`DELETE` tears down the
runtime immediately - see `docs/architecture.md`).

| Workload | Cold median | Cold p95 | Cold p99 | Warm median | Warm p95 | Warm p99 |
|----------|------------:|---------:|---------:|------------:|---------:|---------:|
| hello (trivial)   | 1140.5ms | 1352.3ms | 1401.7ms | 0.5ms | 2.5ms | 2.9ms |
| sleep (sleep_ms=0) | 1013.0ms | 1115.7ms | 1120.7ms | 1.0ms  | 2.0ms | 2.0ms |

Warm reuse is roughly **1,000-2,000x faster** than a cold start for a
trivial workload - the entire measurable cost of a cold start here is
Docker container creation plus the readiness poll, not the function
itself (both workloads are near-instant once actually invoked).

### Concurrency

`sleep:v1` invoked with `sleep_ms=50`, timeout 10s, against a single
warmed-up runtime.

| Concurrency | Throughput | Median latency | p95 latency | Errors | Active runtimes |
|------------:|-----------:|----------------:|------------:|-------:|-----------------:|
| 1  | 15.0 req/s | 53ms    | 53ms    | 0/1  | 1 |
| 5  | 17.6 req/s | 156ms   | 251ms   | 0/5  | 1 |
| 10 | 8.2 req/s  | 281ms   | 1170ms  | 0/10 | 1 |
| 25 | 12.3 req/s | 1334ms  | 1944ms  | 0/25 | 1 |
| 50 | 12.7 req/s | 1786ms  | 3168ms  | 0/50 | 1 |

Throughput plateaus around 12-17 req/s regardless of concurrency, and
latency grows roughly linearly with it, because every concurrent
request serializes against the same single runtime (see
`docs/architecture.md` - a pool of runtimes per function is an explicit
advanced extension, not built here). Zero errors throughout: the
generous 10s timeout absorbed the queueing delay rather than any request
actually failing. This is the direct, measured cost of the
single-runtime-per-function design, not a guess.

### Scale-to-zero trade-off

`hello:v1`, one cold invocation, then polling until the runtime
disappears, then one more invocation to measure recovery.

| Idle timeout | Time to zero | Recovery latency | Recreated correctly |
|-------------:|-------------:|------------------:|:--------------------:|
| 2000ms  | 2.91s  | 1333ms | yes |
| 5000ms  | 5.87s  | 1331ms | yes |
| 10000ms | 10.58s | 1022ms | yes |

Time-to-zero tracks the configured idle timeout closely (the small
excess - roughly 0.5-0.9s - comes from the reaper's 500ms poll interval
plus measurement overhead, not drift in the timeout itself). Recovery
latency after scale-to-zero matches the cold-start numbers above, as
expected: recreating a scaled-to-zero runtime *is* a cold start.

### Failure injection

Three scenarios against a real control plane and real Docker containers,
each timed rather than just asserted pass/fail.

| Scenario | Recovery time | Correct outcome |
|----------|---------------:|:-----------------:|
| Warm runtime's container killed externally, then invoked | 1064ms | yes - self-healing retry, reported as a normal success |
| Function invocation forced past its timeout, then invoked again | 1069ms | yes - reported `"timeout"`, runtime torn down, next call is a fresh cold start |
| Control-plane process killed and restarted | 14ms to healthy | yes - function metadata and invocation history intact (SQLite) |

The first two recovery times land almost exactly at the cold-start
median above, because both recoveries *are* a cold start under the
hood (see `docs/architecture.md` for the self-healing retry design).
The control-plane restart itself is near-instant because startup work
is minimal (open the SQLite file, bind a socket); what actually takes
time after a real production restart would be re-establishing warm
runtimes via new cold starts on the next traffic, which is the same
number as everything else in this table.

## What these results say about the central engineering question

The project's central question (`README.md`) is how to execute
short-lived functions efficiently while providing isolation, lifecycle
management, reliability, and low invocation latency. These four
experiments answer it concretely for this implementation:

- Isolation and lifecycle correctness hold up under real failures (all
  three failure scenarios recovered correctly, not just quickly).
- Warm reuse is the dominant lever for low latency - roughly three
  orders of magnitude faster than a cold start - which is exactly why
  scale-to-zero's idle-timeout choice is a real trade-off and not a free
  optimization: a shorter timeout means faster resource reclamation but
  more frequent trips back to cold-start latency.
- The single-runtime-per-function design is simple and correct, and
  the concurrency numbers show precisely what it costs: throughput
  plateaus early and latency grows with queued concurrent load. A
  runtime pool (section 7 of the specification, "Advanced Serverless
  Extensions") is the documented next step if that ceiling matters for
  a given workload - not attempted here, in keeping with the project's
  stated preference for a small number of correctly implemented
  mechanisms over a larger, shallower feature set.
