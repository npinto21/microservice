#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)"

cd "${ROOT_DIR}"

echo "[1/5] Building Pinto21 tools"
make -B step-interpreter step-semantic step-modtool

echo "[2/5] Preparing microservice module"
./build/bin/pinto21-mod prepare-module modules/npinto21/microservice/module.p21

echo "[3/5] Running semantic checks for module tests"
./build/bin/pinto21-semantic modules/npinto21/microservice/tests/semantic/basic.p21 >/dev/null
./build/bin/pinto21-semantic modules/npinto21/microservice/tests/semantic/timeout_basic.p21 >/dev/null

echo "[4/5] Running semantic checks for examples"
./build/bin/pinto21-semantic modules/npinto21/microservice/examples/basic.p21 >/dev/null
./build/bin/pinto21-semantic modules/npinto21/microservice/examples/health_server.p21 >/dev/null
./build/bin/pinto21-semantic modules/npinto21/microservice/examples/timeout_server.p21 >/dev/null

echo "[5/5] Running OTLP integration test"
sh modules/npinto21/microservice/tests/integration/run_otlp_http_integration.sh

echo "Microservice module validation passed"
