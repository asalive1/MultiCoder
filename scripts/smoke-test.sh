#!/usr/bin/env bash
# smoke-test.sh — Basic health + start/stop smoke tests against a running MultiCoder instance.
# Requires: curl, jq
# Usage:  ./scripts/smoke-test.sh [host] [port]
#         defaults: localhost 8050

set -euo pipefail

HOST=${1:-localhost}
PORT=${2:-8050}
BASE="http://${HOST}:${PORT}"
PASS=0
FAIL=0

check() {
    local desc="$1"; local expect="$2"; local actual="$3"
    if [[ "$actual" == *"$expect"* ]]; then
        echo "  PASS: $desc"; ((PASS++))
    else
        echo "  FAIL: $desc — expected '$expect', got '$actual'"; ((FAIL++))
    fi
}

echo "=== MultiCoder Smoke Tests ==="
echo "    Target: $BASE"
echo ""

# 1. Health endpoint
echo "--- Health ---"
body=$(curl -sf "$BASE/health" || echo "CURL_FAILED")
check "/health returns ok" '"status":"ok"' "$body"

# 2. Encoder list
echo "--- Encoder list ---"
body=$(curl -sf "$BASE/api/encoders" || echo "CURL_FAILED")
check "/api/encoders returns array" '"id":1' "$body"
check "/api/encoders has 5 encoders" '"id":5' "$body"

# 3. Individual encoder state
echo "--- Individual encoder ---"
body=$(curl -sf "$BASE/api/encoder/1" || echo "CURL_FAILED")
check "/api/encoder/1 returns id 1" '"id":1' "$body"

# 4. Start AAC on encoder 1
echo "--- Start/stop streams ---"
body=$(curl -sf -X POST "$BASE/api/encoder/1/aac/start" || echo "CURL_FAILED")
check "Start encoder 1 AAC" '"aac":true' "$body"

# 5. Verify encoder 2 not affected
body=$(curl -sf "$BASE/api/encoder/2" || echo "CURL_FAILED")
check "Encoder 2 AAC still stopped" '"aac":false' "$body"

# 6. Stop encoder 1 AAC
body=$(curl -sf -X POST "$BASE/api/encoder/1/aac/stop" || echo "CURL_FAILED")
check "Stop encoder 1 AAC" '"aac":false' "$body"

# 7. Start HLS on encoder 3
body=$(curl -sf -X POST "$BASE/api/encoder/3/hls/start" || echo "CURL_FAILED")
check "Start encoder 3 HLS" '"hls":true' "$body"

# 8. Verify encoder 1 + 2 HLS not affected
body=$(curl -sf "$BASE/api/encoder/1" || echo "CURL_FAILED")
check "Encoder 1 HLS not started" '"hls":false' "$body"

# 9. Log tail accessible
body=$(curl -sf "$BASE/api/encoder/1/log" || echo "CURL_FAILED")
# Response should be text (may be empty or a message)
if [[ "$body" != "CURL_FAILED" ]]; then
    echo "  PASS: /api/encoder/1/log accessible"; ((PASS++))
else
    echo "  FAIL: /api/encoder/1/log not accessible"; ((FAIL++))
fi

# 10. Config save + read round-trip
echo "--- Config round-trip ---"
PAYLOAD='{"stationId":"TestStation","url":"http://localhost:8000/test-aac","user":"source","pass":"test","icyMetaInt":8192,"metaEnabled":true,"iface":""}'
curl -sf -X POST "$BASE/api/encoder/2/config/aac" \
    -H "Content-Type: application/json" \
    -d "$PAYLOAD" > /dev/null
body=$(curl -sf "$BASE/api/encoder/2/config" || echo "CURL_FAILED")
check "Config round-trip saves stationId" 'TestStation' "$body"

echo ""
echo "=== Results: ${PASS} passed, ${FAIL} failed ==="
[[ "$FAIL" -eq 0 ]]
