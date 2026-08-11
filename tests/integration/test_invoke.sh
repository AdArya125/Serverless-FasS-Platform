#!/usr/bin/env bash
# Exercises a real invoke path end to end: builds the hello function
# image, starts the control plane, deploys hello, invokes it twice, and
# checks that the first invocation is a cold start and the second is a
# warm start. Requires a working Docker daemon; not part of `make test`.
set -euo pipefail
cd "$(dirname "$0")/../.."

PORT=18080
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

echo "starting control plane on port $PORT"
FAAS_PORT=$PORT ./control-plane/build/faas-control-plane > /tmp/faas-integration.log 2>&1 &
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

echo "cold invocation"
cold_output=$($CLOUDFN invoke hello --data '{"name":"Adi"}')
echo "$cold_output"
echo "$cold_output" | grep -q "cold_start: true" || { echo "FAIL: expected cold_start: true"; exit 1; }
echo "$cold_output" | grep -q 'result: "Hello, Adi!"' || { echo "FAIL: unexpected result"; exit 1; }

echo "warm invocation"
warm_output=$($CLOUDFN invoke hello --data '{"name":"Adi"}')
echo "$warm_output"
echo "$warm_output" | grep -q "cold_start: false" || { echo "FAIL: expected cold_start: false"; exit 1; }

echo "invoking an unregistered function (expect an error, not a crash)"
$CLOUDFN invoke does-not-exist --data '{}' && { echo "FAIL: expected non-zero exit"; exit 1; } || true

$CLOUDFN delete hello

echo "PASS: invoke integration test"
