# MultiCoder Integration Test Plan

## Overview

These integration tests verify the end-to-end behaviour of a running MultiCoder
instance inside Docker Compose.

## Prerequisites

- `docker` and `docker compose` installed
- `curl` and `jq` in PATH
- Ports 8050 and 8000 available on the test host

## Test Environment Setup

```bash
# Start services
docker compose up --build -d
sleep 15  # Allow supervisor + workers to fully start
```

## Test Cases

### IT-01: Health Check

**Command:**
```bash
curl -sf http://localhost:8050/health
```
**Expected:** `{"status":"ok","service":"multicoder-supervisor"}`

---

### IT-02: Encoder List

**Command:**
```bash
curl -sf http://localhost:8050/api/encoders | jq 'length'
```
**Expected:** `5` (all five encoders listed)

---

### IT-03: Independent Stream Start — does not affect other encoders

**Commands:**
```bash
# Start encoder 2 AAC
curl -sf -X POST http://localhost:8050/api/encoder/2/aac/start

# Verify encoder 1 and 3 are not affected
curl -sf http://localhost:8050/api/encoder/1 | jq '.aac'   # expect: false
curl -sf http://localhost:8050/api/encoder/3 | jq '.aac'   # expect: false
curl -sf http://localhost:8050/api/encoder/2 | jq '.aac'   # expect: true
```

---

### IT-04: Config Save + Read Round-Trip

```bash
PAYLOAD='{"stationId":"IntegTest","url":"http://localhost:8000/integ-aac","user":"source","pass":"test","icyMetaInt":8192,"metaEnabled":true,"iface":""}'
curl -sf -X POST http://localhost:8050/api/encoder/1/config/aac \
    -H 'Content-Type: application/json' \
    -d "$PAYLOAD"

# Read back
curl -sf http://localhost:8050/api/encoder/1/config | jq '.aac.stationId'
# Expected: "IntegTest"
```

---

### IT-05: Metadata Ingest Simulation

Send a minimal XML metadata payload to encoder 1's metadata port (9000):

```bash
cat <<'EOF' | nc -q1 localhost 9000
<?xml version="1.0"?>
<item><artist>Test Artist</artist><title>Test Title</title><duration>210</duration></item>
EOF

# Verify cache file created
docker compose exec multicoder test -f /etc/encoder1/meta_current.xml && echo "PASS"
```

---

### IT-06: Log Tail Accessible

```bash
curl -sf http://localhost:8050/api/encoder/1/log
# Expected: text/plain response (may include heartbeat lines or startup messages)
```

---

### IT-07: Icecast Service Healthy

```bash
curl -sf http://localhost:8000/status.xsl -o /dev/null && echo "Icecast OK"
```

---

### IT-08: UI Static Assets Load

```bash
curl -sf -o /dev/null -w '%{http_code}' http://localhost:8050/          # expect 200
curl -sf -o /dev/null -w '%{http_code}' http://localhost:8050/styles.css # expect 200
curl -sf -o /dev/null -w '%{http_code}' http://localhost:8050/app.js     # expect 200
```

---

## Automated Run

Use the provided smoke test script which covers most of the above:

```bash
chmod +x scripts/smoke-test.sh
./scripts/smoke-test.sh localhost 8050
```

## Teardown

```bash
docker compose down -v
```
