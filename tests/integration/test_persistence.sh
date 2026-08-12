#!/usr/bin/env bash
# Exercises the actual claim of persistence: function metadata and
# invocation history survive a control-plane restart, not just a
# database file existing on disk. Requires a working Docker daemon; not
# part of `make test`.
set -euo pipefail
cd "$(dirname "$0")/../.."

PORT=18084
IMAGE=hello:v1
DB_PATH=/tmp/faas-test-persistence.db

echo "building project"
make build >/dev/null

echo "building hello function image"
docker build -t "$IMAGE" functions/hello >/dev/null

rm -f "$DB_PATH"
cleanup() {
    [[ -n "${SERVER_PID:-}" ]] && kill "$SERVER_PID" >/dev/null 2>&1 || true
    docker ps -q --filter "ancestor=$IMAGE" | xargs -r docker rm -f >/dev/null 2>&1 || true
    rm -f "$DB_PATH"
}
trap cleanup EXIT

start_server() {
    FAAS_PORT=$PORT FAAS_DB_PATH=$DB_PATH ./control-plane/build/faas-control-plane > /tmp/faas-persistence.log 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 1 50); do
        if curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then
            return
        fi
        sleep 0.1
    done
    echo "FAIL: control plane did not become healthy"
    exit 1
}

export FAAS_API="127.0.0.1:$PORT"
CLOUDFN=./cli/build/cloudfn

echo "starting control plane (first run)"
start_server

echo "deploying hello and invoking it once"
$CLOUDFN deploy hello --image "$IMAGE" --idle-timeout 5m
invoke_output=$($CLOUDFN invoke hello --data '{"name":"Adi"}')
echo "$invoke_output"
invocation_id=$(echo "$invoke_output" | awk '/^invocation_id:/ {print $2}')
[[ -n "$invocation_id" ]] || { echo "FAIL: could not read invocation_id"; exit 1; }
echo "recorded invocation id: $invocation_id"

echo "stopping control plane"
kill "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null || true
unset SERVER_PID

echo "restarting control plane against the same database"
start_server

echo "checking the function survived the restart"
describe_output=$($CLOUDFN describe hello)
echo "$describe_output"
echo "$describe_output" | grep -q "Image:       $IMAGE" || { echo "FAIL: function metadata lost across restart"; exit 1; }

echo "checking the invocation record survived the restart"
record_output=$($CLOUDFN invocation "$invocation_id")
echo "$record_output"
echo "$record_output" | grep -q "function:   hello" || { echo "FAIL: invocation record lost across restart"; exit 1; }
echo "$record_output" | grep -q 'result:     "Hello, Adi!"' || { echo "FAIL: invocation output not preserved"; exit 1; }

$CLOUDFN delete hello

echo "PASS: persistence integration test"
