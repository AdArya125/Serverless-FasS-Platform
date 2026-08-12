#!/usr/bin/env python3
"""Cold start vs warm start latency experiment (spec section 4.1).

For each workload: force a cold start (delete + redeploy resets the
runtime, since DELETE tears down any active runtime immediately),
invoke once (cold), invoke again right away (warm), and repeat for
TRIALS iterations. Reports median/p95/p99 for cold and warm separately -
the spec is explicit that a single average is not enough here.

Requires the control plane running and hello:v1 / sleep:v1 built
(`make functions`).
"""

import json
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import common

TRIALS = int(os.environ.get("TRIALS", "15"))
RESULTS_DIR = os.path.join(os.path.dirname(__file__), "results")

WORKLOADS = [
    {"name": "bench-hello", "image": "hello:v1", "data": {"name": "bench"}},
    {"name": "bench-sleep0", "image": "sleep:v1", "data": {"sleep_ms": 0}},
]


def run_workload(workload):
    name = workload["name"]
    cold_ms, warm_ms = [], []

    for trial in range(TRIALS):
        common.delete(name)  # tears down any runtime immediately; 404 if none, that's fine
        status, _ = common.deploy(name, workload["image"], idle_timeout_ms=300000)
        if status not in (200, 201):
            print("  trial {}: deploy failed (status {})".format(trial, status))
            continue

        status, body = common.invoke(name, workload["data"])
        if status != 200 or not body or not body.get("cold_start"):
            print("  trial {}: expected a cold start, got status={} cold_start={}".format(
                trial, status, body.get("cold_start") if body else None))
            continue
        cold_ms.append(body["duration_ms"])

        status, body = common.invoke(name, workload["data"])
        if status != 200 or not body or body.get("cold_start"):
            print("  trial {}: expected a warm start, got status={} cold_start={}".format(
                trial, status, body.get("cold_start") if body else None))
            continue
        warm_ms.append(body["duration_ms"])

    common.delete(name)
    return cold_ms, warm_ms


def summarize(label, values):
    if not values:
        print("  {}: no samples".format(label))
        return None
    p = common.percentiles(values)
    print("  {}: n={} median={:.1f}ms p95={:.1f}ms p99={:.1f}ms".format(
        label, len(values), p[50], p[95], p[99]))
    return {"n": len(values), "median_ms": p[50], "p95_ms": p[95], "p99_ms": p[99], "raw_ms": values}


def main():
    if not common.wait_for_health():
        print("control plane is not reachable at {}".format(common.API_BASE))
        sys.exit(1)

    os.makedirs(RESULTS_DIR, exist_ok=True)
    results = {}

    for workload in WORKLOADS:
        print("== {} ==".format(workload["name"]))
        cold_ms, warm_ms = run_workload(workload)
        results[workload["name"]] = {
            "cold": summarize("cold", cold_ms),
            "warm": summarize("warm", warm_ms),
        }

    out_path = os.path.join(RESULTS_DIR, "cold_start_{}.json".format(int(time.time())))
    with open(out_path, "w") as f:
        json.dump(results, f, indent=2)
    print("\nraw results written to {}".format(out_path))


if __name__ == "__main__":
    main()
