#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
make build

export FAAS_PORT="${FAAS_PORT:-8080}"
echo "starting control plane on port ${FAAS_PORT}"
./control-plane/build/faas-control-plane
