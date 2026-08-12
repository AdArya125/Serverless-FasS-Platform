#!/usr/bin/env bash
# Exercises timeout enforcement and idle-expiry cleanup against real
# Docker containers: a function that sleeps past its timeout should be
# reported as "timeout" and its runtime torn down, not reused; an idle
# runtime should be reaped after the configured idle window and a later
# invocation should recreate it (a cold start). Requires a working
# Docker daemon; not part of `make test`.
set -euo pipefail
cd "$(dirname "$0")/../.."

PORT=18081
SLEEP_IMAGE=sleep:v1
DB_PATH=/tmp/faas-test-lifecycle.db

echo "building project"
make build >/dev/null

echo "building sleep function image"
docker build -t "$SLEEP_IMAGE" functions/sleep >/dev/null

rm -f "$DB_PATH"
cleanup() {
    [[ -n "${SERVER_PID:-}" ]] && kill "$SERVER_PID" >/dev/null 2>&1 || true
    docker ps -q --filter "ancestor=$SLEEP_IMAGE" | xargs -r docker rm -f >/dev/null 2>&1 || true
    rm -f "$DB_PATH"
}
trap cleanup EXIT

echo "starting control plane (idle timeout: 2s)"
FAAS_PORT=$PORT FAAS_IDLE_TIMEOUT_MS=2000 FAAS_DB_PATH=$DB_PATH ./control-plane/build/faas-control-plane > /tmp/faas-lifecycle.log 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 50); do
    if curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

export FAAS_API="127.0.0.1:$PORT"
CLOUDFN=./cli/build/cloudfn

echo "deploying sleep with a 1s timeout"
$CLOUDFN deploy sleep --image "$SLEEP_IMAGE" --timeout 1s

echo "invoking with a 3s sleep (expect a timeout)"
timeout_output=$($CLOUDFN invoke sleep --data '{"sleep_ms":3000}' || true)
echo "$timeout_output"
echo "$timeout_output" | grep -q "status: timeout" || { echo "FAIL: expected status: timeout"; exit 1; }

echo "checking that the timed-out runtime was cleaned up, not left running"
after_timeout=$(curl -s "http://127.0.0.1:$PORT/functions/sleep/runtime")
echo "$after_timeout"
echo "$after_timeout" | grep -q '"error"' || { echo "FAIL: expected no runtime after a timeout"; exit 1; }

echo "invoking with a fast sleep (expect success, runtime goes IDLE)"
ok_output=$($CLOUDFN invoke sleep --data '{"sleep_ms":10}')
echo "$ok_output"
echo "$ok_output" | grep -q "status: success" || { echo "FAIL: expected status: success"; exit 1; }

running_state=$(curl -s "http://127.0.0.1:$PORT/functions/sleep/runtime")
echo "$running_state"
echo "$running_state" | grep -q '"state":"IDLE"' || { echo "FAIL: expected IDLE state right after success"; exit 1; }

echo "waiting for idle expiry (scale-to-zero should kick in within ~2-3s)"
sleep 3
after_idle=$(curl -s "http://127.0.0.1:$PORT/functions/sleep/runtime")
echo "$after_idle"
echo "$after_idle" | grep -q '"error"' || { echo "FAIL: expected the idle runtime to have been reaped"; exit 1; }

echo "invoking again after scale-to-zero (expect a fresh cold start)"
recreate_output=$($CLOUDFN invoke sleep --data '{"sleep_ms":10}')
echo "$recreate_output"
echo "$recreate_output" | grep -q "cold_start: true" || { echo "FAIL: expected a fresh cold start after scale-to-zero"; exit 1; }

$CLOUDFN delete sleep

echo "PASS: lifecycle integration test"
