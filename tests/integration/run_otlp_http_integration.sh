#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/../../../../.." && pwd)"
PYTHON_BIN="${PYTHON_BIN:-python3}"
CURL_BIN="${CURL_BIN:-curl}"
COLLECTOR_PORT="${P21_OTLP_TEST_PORT:-43218}"
SERVER_PORT="${P21_MS_TEST_PORT:-8021}"
CAPTURE_FILE="${TMPDIR:-/tmp}/p21_otlp_capture.jsonl"
COLLECTOR_LOG="${TMPDIR:-/tmp}/p21_otlp_capture.log"
SERVER_LOG="${TMPDIR:-/tmp}/p21_ms_server.log"
HEALTH_FILE="${TMPDIR:-/tmp}/p21_ms_health_response.json"
COLLECTOR_PID=""
SERVER_PID=""

cleanup() {
    if [ -n "${SERVER_PID}" ]; then
        kill "${SERVER_PID}" >/dev/null 2>&1 || true
        wait "${SERVER_PID}" >/dev/null 2>&1 || true
    fi
    if [ -n "${COLLECTOR_PID}" ]; then
        kill "${COLLECTOR_PID}" >/dev/null 2>&1 || true
        wait "${COLLECTOR_PID}" >/dev/null 2>&1 || true
    fi
}

trap cleanup EXIT INT TERM

cd "${ROOT_DIR}"

rm -f "${CAPTURE_FILE}" "${COLLECTOR_LOG}" "${SERVER_LOG}" "${HEALTH_FILE}"

make -B step-interpreter step-semantic step-modtool
./build/bin/pinto21-mod prepare-module modules/npinto21/microservice/module.p21
./build/bin/pinto21-semantic modules/npinto21/microservice/examples/health_server.p21 >/dev/null

"${PYTHON_BIN}" modules/npinto21/microservice/tests/integration/otlp_capture.py "${CAPTURE_FILE}" "${COLLECTOR_PORT}" >"${COLLECTOR_LOG}" 2>&1 &
COLLECTOR_PID=$!
sleep 1

OTEL_SERVICE_NAME=p21-ms-test \
OTEL_EXPORTER_OTLP_ENDPOINT="http://127.0.0.1:${COLLECTOR_PORT}" \
OTEL_EXPORTER_OTLP_PROTOCOL=http/json \
PINTO21_OTEL_EXPORT_LOGS=true \
./build/bin/pinto21 modules/npinto21/microservice/examples/health_server.p21 >"${SERVER_LOG}" 2>&1 &
SERVER_PID=$!
sleep 2

"${CURL_BIN}" -s "http://127.0.0.1:${SERVER_PORT}/health" >"${HEALTH_FILE}"
sleep 3

CAPTURE_FILE="${CAPTURE_FILE}" "${PYTHON_BIN}" - <<'PY'
import json
import os
from pathlib import Path

capture_path = Path(os.environ["CAPTURE_FILE"])
if not capture_path.exists():
    raise SystemExit("OTLP capture file was not created")

rows = [json.loads(line) for line in capture_path.read_text(encoding="utf-8").splitlines() if line.strip()]
if not rows:
    raise SystemExit("OTLP capture file is empty")

has_traces = any(row.get("path") == "/v1/traces" for row in rows)
has_logs = any(row.get("path") == "/v1/logs" for row in rows)
if not has_traces:
    raise SystemExit("Missing OTLP trace export to /v1/traces")
if not has_logs:
    raise SystemExit("Missing OTLP log export to /v1/logs")

for row in rows:
    body = json.loads(row["body"])
    if row["path"] == "/v1/logs":
        record = body["resourceLogs"][0]["scopeLogs"][0]["logRecords"][0]
        if record.get("severityText") != "info":
            raise SystemExit("Unexpected OTLP log severity")
        if not any(attr.get("key") == "trace_id" for attr in record.get("attributes", [])):
            raise SystemExit("OTLP log payload missing trace_id attribute")
    elif row["path"] == "/v1/traces":
        span = body["resourceSpans"][0]["scopeSpans"][0]["spans"][0]
        if span.get("name") != "http.request":
            raise SystemExit("Unexpected OTLP trace span name")
        if "traceId" not in span or "spanId" not in span:
            raise SystemExit("OTLP trace payload missing traceId or spanId")

print(json.dumps({
    "ok": True,
    "paths": [row["path"] for row in rows],
    "count": len(rows),
}))
PY

echo "OTLP integration test passed"
