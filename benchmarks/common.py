"""Shared HTTP helpers for the benchmark scripts.

Talks directly to the control-plane API (not through cloudfn) so
results come back as structured JSON instead of parsed CLI text. Uses
only the standard library, consistent with the rest of the project's
dependency choices.
"""

import json
import os
import time
import urllib.error
import urllib.request

API_BASE = os.environ.get("FAAS_API_URL", "http://127.0.0.1:8080")


def _request(method, path, body=None, timeout=30):
    url = API_BASE + path
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
            parsed = json.loads(raw) if raw else None
            return resp.status, parsed
    except urllib.error.HTTPError as e:
        raw = e.read()
        try:
            parsed = json.loads(raw) if raw else None
        except json.JSONDecodeError:
            parsed = {"error": raw.decode("utf-8", "replace")}
        return e.code, parsed


def deploy(name, image, timeout_ms=5000, idle_timeout_ms=60000, **extra):
    body = {"name": name, "image": image, "timeout_ms": timeout_ms, "idle_timeout_ms": idle_timeout_ms}
    body.update(extra)
    return _request("POST", "/functions", body)


def delete(name):
    return _request("DELETE", "/functions/{}".format(name))


def invoke(name, data):
    return _request("POST", "/functions/{}/invoke".format(name), data)


def get_runtime(name):
    return _request("GET", "/functions/{}/runtime".format(name))


def list_runtimes():
    return _request("GET", "/runtimes")


def percentiles(values, ps=(50, 95, 99)):
    if not values:
        return {p: None for p in ps}
    ordered = sorted(values)
    result = {}
    for p in ps:
        k = (len(ordered) - 1) * (p / 100)
        f = int(k)
        c = min(f + 1, len(ordered) - 1)
        if f == c:
            result[p] = ordered[f]
        else:
            result[p] = ordered[f] + (ordered[c] - ordered[f]) * (k - f)
    return result


def wait_for_health(retries=50, interval=0.1):
    for _ in range(retries):
        try:
            status, _ = _request("GET", "/health")
            if status == 200:
                return True
        except Exception:
            pass
        time.sleep(interval)
    return False
