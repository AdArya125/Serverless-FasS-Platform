#!/usr/bin/env bash
# Exercises per-function idle timeouts and the GET /runtimes listing:
# deploys the same image under two names with different idle timeouts,
# invokes both, and checks that the short-timeout one scales to zero
# while the long-timeout one is still listed as active. Requires a
# working Docker daemon; not part of `make test`.
set -euo pipefail
cd "$(dirname "$0")/../.."

PORT=18083
IMAGE=hello:v1

echo "building project"
make build >/dev/null

echo "building hello function image"
docker build -t "$IMAGE" functions/hello >/dev/null

cleanup() {
    [[ -n "${SERVER_PID:-}" ]] && kill "$SERVER_PID" >/dev/null 2>&1 || true
    docker ps -q --filter "ancestor=$IMAGE" | xargs -r docker rm -f >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "starting control plane"
FAAS_PORT=$PORT ./control-plane/build/faas-control-plane > /tmp/faas-scale-to-zero.log 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 50); do
    if curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

export FAAS_API="127.0.0.1:$PORT"
CLOUDFN=./cli/build/cloudfn

echo "deploying short-lived (2s idle timeout) and long-lived (30s idle timeout)"
$CLOUDFN deploy short-lived --image "$IMAGE" --idle-timeout 2s
$CLOUDFN deploy long-lived --image "$IMAGE" --idle-timeout 30s

echo "invoking both once, to create their runtimes"
$CLOUDFN invoke short-lived --data '{"name":"a"}' >/dev/null
$CLOUDFN invoke long-lived --data '{"name":"b"}' >/dev/null

echo "runtime listing right after invoking both"
$CLOUDFN runtimes
running_count=$($CLOUDFN runtimes | wc -l)
[[ "$running_count" -eq 2 ]] || { echo "FAIL: expected 2 active runtimes, saw $running_count"; exit 1; }

echo "waiting 3s: short-lived should expire, long-lived should not"
sleep 3

echo "runtime listing after the wait"
$CLOUDFN runtimes
$CLOUDFN runtimes | grep -q "^short-lived" && { echo "FAIL: short-lived should have scaled to zero by now"; exit 1; }
$CLOUDFN runtimes | grep -q "^long-lived" || { echo "FAIL: long-lived should still be active"; exit 1; }

echo "short-lived recreates on demand (fresh cold start)"
recreate_output=$($CLOUDFN invoke short-lived --data '{"name":"a"}')
echo "$recreate_output"
echo "$recreate_output" | grep -q "cold_start: true" || { echo "FAIL: expected a fresh cold start"; exit 1; }

$CLOUDFN delete short-lived
$CLOUDFN delete long-lived

echo "PASS: scale-to-zero integration test"
