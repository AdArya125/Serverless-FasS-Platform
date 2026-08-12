#!/usr/bin/env python3
"""Concurrency experiment (spec section 4.3): throughput, latency, and
error rate as concurrent request count increases against a single warm
runtime.

This platform keeps exactly one runtime per function (see
docs/architecture.md - a pool of runtimes per function is an explicit
"advanced extension", not built here), so concurrent requests serialize
against that one runtime rather than scaling out. This experiment is
what actually demonstrates that trade-off with numbers instead of just
asserting it.

Requires the control plane running and sleep:v1 built (`make functions`).
"""

import json
import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import common

FUNCTION_NAME = "bench-concurrency"
LEVELS = [int(x) for x in os.environ.get("LEVELS", "1,5,10,25,50").split(",")]
RESULTS_DIR = os.path.join(os.path.dirname(__file__), "results")


def fire_one(_):
    status, body = common.invoke(FUNCTION_NAME, {"sleep_ms": 50})
    ok = status == 200 and body is not None and body.get("status") == "success"
    duration_ms = body["duration_ms"] if body and "duration_ms" in body else None
    return ok, duration_ms


def run_level(level):
    start = time.monotonic()
    with ThreadPoolExecutor(max_workers=level) as pool:
        outcomes = list(pool.map(fire_one, range(level)))
    total_s = time.monotonic() - start

    durations = [d for ok, d in outcomes if ok and d is not None]
    errors = sum(1 for ok, _ in outcomes if not ok)
    throughput = level / total_s if total_s > 0 else 0.0

    _, runtime = common.get_runtime(FUNCTION_NAME)
    active_runtimes = 1 if runtime else 0

    p = common.percentiles(durations) if durations else {50: None, 95: None, 99: None}
    print("  concurrency={:<3} throughput={:.1f} req/s  median={}  p95={}  errors={}/{}  active_runtimes={}".format(
        level, throughput,
        "{:.1f}ms".format(p[50]) if p[50] is not None else "n/a",
        "{:.1f}ms".format(p[95]) if p[95] is not None else "n/a",
        errors, level, active_runtimes))

    return {
        "concurrency": level,
        "throughput_req_s": throughput,
        "median_ms": p[50],
        "p95_ms": p[95],
        "p99_ms": p[99],
        "errors": errors,
        "active_runtimes": active_runtimes,
    }


def main():
    if not common.wait_for_health():
        print("control plane is not reachable at {}".format(common.API_BASE))
        sys.exit(1)

    common.delete(FUNCTION_NAME)
    common.deploy(FUNCTION_NAME, "sleep:v1", timeout_ms=10000, idle_timeout_ms=300000)
    common.invoke(FUNCTION_NAME, {"sleep_ms": 0})  # warm it up; discard the cold-start sample

    os.makedirs(RESULTS_DIR, exist_ok=True)
    results = []
    for level in LEVELS:
        print("== concurrency {} ==".format(level))
        results.append(run_level(level))

    common.delete(FUNCTION_NAME)

    out_path = os.path.join(RESULTS_DIR, "concurrency_{}.json".format(int(time.time())))
    with open(out_path, "w") as f:
        json.dump(results, f, indent=2)
    print("\nraw results written to {}".format(out_path))


if __name__ == "__main__":
    main()
