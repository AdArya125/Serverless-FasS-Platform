#!/usr/bin/env bash
# Failure injection experiment (spec section 4.4): measures recovery
# time for three failure modes - a warm runtime dying, a function
# exceeding its timeout, and a control-plane restart - reusing the same
# scenarios already covered by tests/integration/, but timed and
# reported as a quantitative result rather than a pass/fail assertion.
set -euo pipefail
cd "$(dirname "$0")/../.."

PORT=18090
IMAGE=hello:v1
SLEEP_IMAGE=sleep:v1
DB_PATH=/tmp/faas-bench-failures.db
RESULTS_DIR=benchmarks/failures/results
mkdir -p "$RESULTS_DIR"
RESULTS_FILE="$RESULTS_DIR/results_$(date +%s).json"

now_ms() { date +%s%3N; }

echo "building project and function images"
make build >/dev/null
docker build -t "$IMAGE" functions/hello >/dev/null
docker build -t "$SLEEP_IMAGE" functions/sleep >/dev/null

rm -f "$DB_PATH"
cleanup() {
    [[ -n "${SERVER_PID:-}" ]] && kill "$SERVER_PID" >/dev/null 2>&1 || true
    docker ps -q --filter "ancestor=$IMAGE" | xargs -r docker rm -f >/dev/null 2>&1 || true
    docker ps -q --filter "ancestor=$SLEEP_IMAGE" | xargs -r docker rm -f >/dev/null 2>&1 || true
    rm -f "$DB_PATH"
}
trap cleanup EXIT

start_server() {
    FAAS_PORT=$PORT FAAS_DB_PATH=$DB_PATH FAAS_IDLE_TIMEOUT_MS=300000 \
        ./control-plane/build/faas-control-plane > /tmp/faas-bench-failures.log 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 1 50); do
        curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && return
        sleep 0.1
    done
    echo "FAIL: control plane did not become healthy"
    exit 1
}

export FAAS_API="127.0.0.1:$PORT"
CLOUDFN=./cli/build/cloudfn

echo "starting control plane"
start_server

echo
echo "== scenario 1: kill a warm runtime's container, then invoke =="
$CLOUDFN deploy hello --image "$IMAGE" >/dev/null
$CLOUDFN invoke hello --data '{"name":"bench"}' >/dev/null # cold, creates the runtime
container_id=$(curl -s "http://127.0.0.1:$PORT/functions/hello/runtime" \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)["container_id"])')
docker rm -f "$container_id" >/dev/null

t0=$(now_ms)
recover_output=$($CLOUDFN invoke hello --data '{"name":"bench"}')
t1=$(now_ms)
scenario1_ms=$((t1 - t0))
if echo "$recover_output" | grep -q "status: success"; then scenario1_correct=true; else scenario1_correct=false; fi
echo "recovery time: ${scenario1_ms}ms, correct outcome: $scenario1_correct"
$CLOUDFN delete hello >/dev/null

echo
echo "== scenario 2: force a function timeout, then invoke again =="
$CLOUDFN deploy sleep-bench --image "$SLEEP_IMAGE" --timeout 1s >/dev/null
$CLOUDFN invoke sleep-bench --data '{"sleep_ms":3000}' >/dev/null || true # forces a timeout

t0=$(now_ms)
recreate_output=$($CLOUDFN invoke sleep-bench --data '{"sleep_ms":10}')
t1=$(now_ms)
scenario2_ms=$((t1 - t0))
if echo "$recreate_output" | grep -q "cold_start: true"; then scenario2_correct=true; else scenario2_correct=false; fi
echo "recovery time: ${scenario2_ms}ms, correct outcome (fresh cold start): $scenario2_correct"
$CLOUDFN delete sleep-bench >/dev/null

echo
echo "== scenario 3: restart the control plane, then check health + persisted state =="
$CLOUDFN deploy hello --image "$IMAGE" >/dev/null
kill "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null || true

t0=$(now_ms)
start_server
t1=$(now_ms)
scenario3_ms=$((t1 - t0))
describe_output=$($CLOUDFN describe hello)
if echo "$describe_output" | grep -q "Image:       $IMAGE"; then scenario3_correct=true; else scenario3_correct=false; fi
echo "restart-to-healthy time: ${scenario3_ms}ms, function metadata intact: $scenario3_correct"
$CLOUDFN delete hello >/dev/null

cat > "$RESULTS_FILE" <<EOF
{
  "dead_warm_runtime_recovery_ms": $scenario1_ms,
  "dead_warm_runtime_recovered_correctly": $scenario1_correct,
  "timeout_recovery_ms": $scenario2_ms,
  "timeout_recovered_correctly": $scenario2_correct,
  "control_plane_restart_to_healthy_ms": $scenario3_ms,
  "state_persisted_correctly": $scenario3_correct
}
EOF

echo
echo "raw results written to $RESULTS_FILE"
