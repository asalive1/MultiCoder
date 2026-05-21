#!/usr/bin/env bash
# smoke-test.sh ? Basic health + start/stop smoke tests against a running MultiCoder instance.
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
        echo "  PASS: $desc"; ((PASS+=1))
    else
        echo "  FAIL: $desc ? expected '$expect', got '$actual'"; ((FAIL+=1))
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
    echo "  PASS: /api/encoder/1/log accessible"; ((PASS+=1))
else
    echo "  FAIL: /api/encoder/1/log not accessible"; ((FAIL+=1))
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

echo "--- Critical stability checks ---"

# TEST 2 (both platforms): StartHLS ACK should return within 3 seconds.
curl -sf -X POST "$BASE/api/encoder/1/hls/stop" > /dev/null || true
start_ms=$(date +%s%3N)
start_hls_body=$(curl -s -X POST "$BASE/api/encoder/1/hls/start")
end_ms=$(date +%s%3N)
ack_ms=$((end_ms - start_ms))
if [[ "$start_hls_body" == *"\"ok\":true"* && "$ack_ms" -le 3000 ]]; then
    echo "  PASS: StartHLS ACK within 3 seconds (${ack_ms} ms)"; ((PASS+=1))
else
    echo "  FAIL: StartHLS ACK exceeded 3 seconds or returned error (${ack_ms} ms, body=${start_hls_body})"; ((FAIL+=1))
fi

# TEST 3 (both platforms): optional delayed StartHLS ACK validation.
# Enable by setting MC_ENABLE_SLOW_ACK_TEST=1 and using a test build that injects ~5s delay at startHLS().
if [[ "${MC_ENABLE_SLOW_ACK_TEST:-0}" == "1" ]]; then
    curl -sf -X POST "$BASE/api/encoder/1/hls/stop" > /dev/null || true
    slow_start_ms=$(date +%s%3N)
    slow_body=$(curl -s -X POST "$BASE/api/encoder/1/hls/start")
    slow_end_ms=$(date +%s%3N)
    slow_ack_ms=$((slow_end_ms - slow_start_ms))
    if [[ "$slow_body" == *"\"ok\":true"* && "$slow_ack_ms" -ge 4000 && "$slow_ack_ms" -le 8000 ]]; then
        echo "  PASS: Slow StartHLS ACK succeeded within extended timeout (${slow_ack_ms} ms)"; ((PASS+=1))
    else
        echo "  FAIL: Slow StartHLS ACK validation failed (${slow_ack_ms} ms, body=${slow_body})"; ((FAIL+=1))
    fi
else
    echo "  WARN: Skipping delayed-ACK test (set MC_ENABLE_SLOW_ACK_TEST=1 with delay-instrumented build)"
fi

# TEST 4 (both platforms): HLS proxy timeout should return 504 around recv timeout window.
admin_cfg="/etc/encoder1/encoder_admin.json"
admin_backup="${admin_cfg}.bak.smoke"
dummy_pid=""
if [[ -f "$admin_cfg" ]]; then
    cp "$admin_cfg" "$admin_backup"
    dummy_port=18991
    jq --argjson p "$dummy_port" '.hlsPlaybackPort = $p' "$admin_cfg" > "${admin_cfg}.tmp"
    mv "${admin_cfg}.tmp" "$admin_cfg"

    python3 - "$dummy_port" <<'PY' &
import socket
import sys
import time

port = int(sys.argv[1])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", port))
s.listen(1)
conn, _ = s.accept()
time.sleep(12)
conn.close()
s.close()
PY
    dummy_pid=$!
    sleep 1

    proxy_start_ms=$(date +%s%3N)
    proxy_code=$(curl -s -o /tmp/mc_proxy_timeout.out -w "%{http_code}" --max-time 15 "$BASE/encoder/1/hls/index.m3u8" || echo "000")
    proxy_end_ms=$(date +%s%3N)
    proxy_ms=$((proxy_end_ms - proxy_start_ms))

    if [[ "$proxy_code" == "504" && "$proxy_ms" -le 10000 ]]; then
        echo "  PASS: HLS proxy timeout returned 504 in ${proxy_ms} ms"; ((PASS+=1))
    else
        echo "  FAIL: HLS proxy timeout check failed (status=${proxy_code}, elapsed=${proxy_ms} ms)"; ((FAIL+=1))
    fi
else
    echo "  WARN: Missing ${admin_cfg}; skipping HLS proxy timeout check"
fi

if [[ -n "$dummy_pid" ]]; then
    kill "$dummy_pid" >/dev/null 2>&1 || true
fi
if [[ -f "$admin_backup" ]]; then
    mv "$admin_backup" "$admin_cfg"
fi

echo ""
echo "=== Results: ${PASS} passed, ${FAIL} failed ==="
if [[ "$FAIL" -eq 0 ]]; then
    exit 0
fi
exit 1
