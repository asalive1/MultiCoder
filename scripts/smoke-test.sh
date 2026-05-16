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

# 11. Enable global SCTE and save encoder SCTE config
echo "--- SCTE/Cue endpoint ---"
curl -sf -X POST "$BASE/api/admin/config" \
    -H "Content-Type: application/json" \
    -d '{"uiPort":8050,"logLevel":"info","logRotSize":10,"logRetention":14,"encoderCount":5,"adminUser":"Admin","adminPass":"change-me","firstLoginRequired":false,"scteGlobalEnabled":true,"scteGlobalRateLimitCount":5,"scteGlobalRateLimitWindowSec":10,"scteGlobalDedupeSeconds":30,"scteLogLevel":"info"}' > /dev/null

META_PAYLOAD='{"mode":"listen","listenPort":9000,"dataConnectHost":"","dataConnectPort":null,"scte":{"enabled":true,"listenEnabled":true,"listenTransport":"http","listenPort":9041,"cueDeliveryType":"json","passthroughMode":"off","requireEventId":false,"requireToken":false,"token":"","watchTags":["SCTE","Event_ID","Event_Duration"],"commandRows":[{"match":"BREAK","action":"START_BREAK"}],"whitelistEnabled":false,"whitelistEntries":[],"overrideRateLimit":false,"rateLimitCount":5,"rateLimitWindowSec":10,"overrideDedupe":false,"dedupeSeconds":30}}'
curl -sf -X POST "$BASE/api/encoder/1/config/metadata" \
    -H "Content-Type: application/json" \
    -d "$META_PAYLOAD" > /dev/null

body=$(curl -sf -X POST "$BASE/api/encoder/1/cue" -H "Content-Type: application/json" -d '{"command":"BREAK","eventId":"evt-1"}' || echo "CURL_FAILED")
check "Cue endpoint accepts matching command" '"matched":true' "$body"
check "Cue endpoint reports unsent when passthrough OFF" '"sent":false' "$body"

body=$(curl -sf -X POST "$BASE/api/encoder/1/cue" -H "Content-Type: application/json" -d '{"command":"Send_Break"}' || echo "CURL_FAILED")
check "Cue endpoint logs no-match safely" '"matched":false' "$body"

# 12. SRT input delay validation
echo "--- SRT Input Delay validation ---"
BAD_INPUT='{"inputType":"srt","srtMode":"caller","srtHost":"127.0.0.1","srtPort":9250,"srtLatency":95000,"srtPass":"","srtStreamId":"","srtPbkeylen":0,"rtpGain":0,"sampleRate":48000,"bitrate":128000}'
code=$(curl -s -o /tmp/mc_bad_input.json -w "%{http_code}" -X POST "$BASE/api/encoder/1/input/connect" -H "Content-Type: application/json" -d "$BAD_INPUT" || echo "000")
check "SRT latency > 90000 rejected" "400" "$code"

GOOD_INPUT='{"inputType":"srt","srtMode":"caller","srtHost":"127.0.0.1","srtPort":9250,"srtLatency":500,"srtPass":"","srtStreamId":"","srtPbkeylen":0,"rtpGain":0,"sampleRate":48000,"bitrate":128000}'
body=$(curl -sf -X POST "$BASE/api/encoder/1/input/connect" -H "Content-Type: application/json" -d "$GOOD_INPUT" || echo "CURL_FAILED")
check "SRT latency in range accepted" '"ok":true' "$body"

body=$(curl -sf "$BASE/api/encoder/1/input/status" || echo "CURL_FAILED")
check "Input status includes effective SRT latency" 'latency=500 ms' "$body"

curl -sf -X POST "$BASE/api/encoder/1/input/disconnect" -H "Content-Type: application/json" -d '{}' > /dev/null

echo ""
echo "=== Results: ${PASS} passed, ${FAIL} failed ==="
[[ "$FAIL" -eq 0 ]]
