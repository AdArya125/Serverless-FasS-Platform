#!/usr/bin/env bash
# Exercises the exact same invoke path as test_invoke.sh, but against
# the Kubernetes backend instead of Docker: deploys hello, invokes it
# twice, and checks that the first call is a cold start (pod + NodePort
# service created) and the second is a warm start. Requires a working
# kubectl pointed at a reachable cluster (a local k3s node works) with
# the hello:v1 image already imported into its containerd - not part of
# `make test`.
#
#   docker save hello:v1 | sudo k3s ctr images import -
set -euo pipefail
cd "$(dirname "$0")/../.."

PORT=18086
IMAGE=hello:v1
DB_PATH=/tmp/faas-test-kubernetes.db

echo "building project"
make build >/dev/null

echo "checking a cluster is reachable"
kubectl get nodes >/dev/null

rm -f "$DB_PATH"
cleanup() {
    [[ -n "${SERVER_PID:-}" ]] && kill "$SERVER_PID" >/dev/null 2>&1 || true
    kubectl get pods -o name 2>/dev/null | grep '^pod/faas-' | xargs -r kubectl delete --ignore-not-found --wait=false >/dev/null 2>&1 || true
    kubectl get svc -o name 2>/dev/null | grep '^service/faas-' | xargs -r kubectl delete --ignore-not-found --wait=false >/dev/null 2>&1 || true
    rm -f "$DB_PATH"
}
trap cleanup EXIT

echo "starting control plane (Kubernetes backend)"
FAAS_PORT=$PORT FAAS_DB_PATH=$DB_PATH FAAS_RUNTIME_BACKEND=kubernetes \
    ./control-plane/build/faas-control-plane > /tmp/faas-kubernetes.log 2>&1 &
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

echo "cold invocation (creates a pod + NodePort service)"
cold_output=$($CLOUDFN invoke hello --data '{"name":"Adi"}')
echo "$cold_output"
echo "$cold_output" | grep -q "cold_start: true" || { echo "FAIL: expected cold_start: true"; exit 1; }
echo "$cold_output" | grep -q 'result: "Hello, Adi!"' || { echo "FAIL: unexpected result"; exit 1; }

echo "confirming a pod and service actually exist"
kubectl get pods -o name | grep -q '^pod/faas-' || { echo "FAIL: expected a faas- pod to exist"; exit 1; }
kubectl get svc -o name | grep -q '^service/faas-' || { echo "FAIL: expected a faas- service to exist"; exit 1; }

echo "warm invocation (reuses the same pod)"
warm_output=$($CLOUDFN invoke hello --data '{"name":"Adi"}')
echo "$warm_output"
echo "$warm_output" | grep -q "cold_start: false" || { echo "FAIL: expected cold_start: false"; exit 1; }

echo "deleting the function"
$CLOUDFN delete hello

echo "PASS: kubernetes integration test"
