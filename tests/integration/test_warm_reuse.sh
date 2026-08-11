#!/usr/bin/env bash
# Exercises warm reuse and the self-healing retry when a warm runtime's
# container has been killed out from under the platform (e.g. by an
# external operator, an OOM kill, or a crash). The platform has no
# proactive health check on the warm path, so this is the scenario that
# actually proves a dead warm runtime does not surface as a caller error.
# Requires a working Docker daemon; not part of `make test`.
set -euo pipefail
cd "$(dirname "$0")/../.."

PORT=18082
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
FAAS_PORT=$PORT ./control-plane/build/faas-control-plane > /tmp/faas-warm-reuse.log 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 50); do
    if curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

export FAAS_API="127.0.0.1:$PORT"
CLOUDFN=./cli/build/cloudfn

echo "deploying hello"
$CLOUDFN deploy hello --image "$IMAGE"

echo "cold invocation (creates the runtime)"
cold_output=$($CLOUDFN invoke hello --data '{"name":"Adi"}')
echo "$cold_output"
echo "$cold_output" | grep -q "cold_start: true" || { echo "FAIL: expected cold_start: true"; exit 1; }

container_id=$(curl -s "http://127.0.0.1:$PORT/functions/hello/runtime" \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)["container_id"])')
echo "runtime container: $container_id"

echo "warm invocation (expect cold_start: false)"
warm_output=$($CLOUDFN invoke hello --data '{"name":"Adi"}')
echo "$warm_output"
echo "$warm_output" | grep -q "cold_start: false" || { echo "FAIL: expected cold_start: false"; exit 1; }

echo "killing the container out from under the platform"
docker rm -f "$container_id" >/dev/null

echo "invoking again: the platform should self-heal with a single retry, not surface an error"
recover_output=$($CLOUDFN invoke hello --data '{"name":"Adi"}')
echo "$recover_output"
echo "$recover_output" | grep -q "status: success" || { echo "FAIL: expected the platform to recover transparently"; exit 1; }
echo "$recover_output" | grep -q "cold_start: true" || { echo "FAIL: expected a fresh cold start after recovery"; exit 1; }

$CLOUDFN delete hello

echo "PASS: warm reuse integration test"
