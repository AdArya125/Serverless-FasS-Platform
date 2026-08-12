#!/usr/bin/env python3
"""Scale-to-zero trade-off experiment (spec section 4.2): relationship
between a function's idle timeout and (a) how long it actually takes to
reach zero running containers and (b) the recovery latency of the
invocation that follows.

Uses shorter idle timeouts than the spec's suggested 30s/60s/300s so the
experiment finishes in a reasonable time locally; the mechanism (a
background reaper checking every 500ms, see docs/architecture.md) scales
to any idle timeout identically - only the numbers would take longer to
collect.

Requires the control plane running and hello:v1 built (`make functions`).
"""

import json
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import common

FUNCTION_NAME = "bench-scale-to-zero"
IDLE_TIMEOUTS_MS = [int(x) for x in os.environ.get("IDLE_TIMEOUTS_MS", "2000,5000,10000").split(",")]
RESULTS_DIR = os.path.join(os.path.dirname(__file__), "results")


def wait_for_scale_to_zero(name, poll_interval=0.25, max_wait=30):
    start = time.monotonic()
    while time.monotonic() - start < max_wait:
        status, _ = common.get_runtime(name)
        if status == 404:
            return time.monotonic() - start
        time.sleep(poll_interval)
    return None


def run_idle_timeout(idle_timeout_ms):
    common.delete(FUNCTION_NAME)
    common.deploy(FUNCTION_NAME, "hello:v1", idle_timeout_ms=idle_timeout_ms)

    status, body = common.invoke(FUNCTION_NAME, {"name": "bench"})
    if status != 200 or not body or not body.get("cold_start"):
        print("  unexpected initial invoke result, skipping")
        return None

    time_to_zero_s = wait_for_scale_to_zero(FUNCTION_NAME)

    status, body = common.invoke(FUNCTION_NAME, {"name": "bench"})
    recovery_ms = body.get("duration_ms") if status == 200 and body else None
    recreated = bool(body and body.get("cold_start"))

    print("  idle_timeout={}ms  time_to_zero={}  recovery={}  recreated={}".format(
        idle_timeout_ms,
        "{:.2f}s".format(time_to_zero_s) if time_to_zero_s is not None else "did not reach zero in time",
        "{}ms".format(recovery_ms) if recovery_ms is not None else "n/a",
        recreated))

    return {
        "idle_timeout_ms": idle_timeout_ms,
        "time_to_zero_s": time_to_zero_s,
        "recovery_ms": recovery_ms,
        "recreated": recreated,
    }


def main():
    if not common.wait_for_health():
        print("control plane is not reachable at {}".format(common.API_BASE))
        sys.exit(1)

    os.makedirs(RESULTS_DIR, exist_ok=True)
    results = []
    for idle_timeout_ms in IDLE_TIMEOUTS_MS:
        print("== idle_timeout {}ms ==".format(idle_timeout_ms))
        result = run_idle_timeout(idle_timeout_ms)
        if result:
            results.append(result)

    common.delete(FUNCTION_NAME)

    out_path = os.path.join(RESULTS_DIR, "scale_to_zero_{}.json".format(int(time.time())))
    with open(out_path, "w") as f:
        json.dump(results, f, indent=2)
    print("\nraw results written to {}".format(out_path))


if __name__ == "__main__":
    main()
