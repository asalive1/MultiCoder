# MultiCoder SCTE/Cue and SRT Input Delay QA Plan

## Scope

This plan validates:
- SCTE cue ingest and exact command matching.
- Global and per-encoder SCTE controls.
- HTTP-first cue transport behavior.
- Optional transport readiness for TCP mode configuration.
- Allowlist, rate limit, and dedupe/replay controls.
- SRT input delay UI, persistence, and runtime application.
- HLS and SRT output compatibility after SCTE additions.
- Sidecar configuration behavior and logging.

## Prerequisites

1. Start MultiCoder and verify supervisor health at `/health`.
2. Ensure at least 1 encoder is configured and selectable.
3. Enable Admin access and verify global SCTE settings are visible.
4. Have test tools available:
- `curl` (Linux/macOS)
- PowerShell `Invoke-RestMethod` (Windows)

## Baseline Setup

1. In Admin -> Application Settings:
- Enable SCTE-35 Cues = Yes.
- Rate limit defaults = `5 per 10 seconds`.
- Dedupe default = `30 seconds`.
- SCTE log level = Info.
2. In Encoder Metadata Configuration:
- Enable SCTE engine/listening.
- Listen transport = HTTP.
- Cue listen port = `904n` where `n` is encoder number.
- Cue delivery type = JSON Only.
- Add at least these command rows:
- `BREAK -> START_BREAK`
- `END_BREAK -> END_BREAK`
- Set passthrough mode to `off` first for safe validation.

## Sample Cue Payloads

Matching JSON:
```json
{"command":"BREAK","eventId":"evt-001"}
```

Non-matching JSON:
```json
{"command":"Send_Break","eventId":"evt-002"}
```

XML example:
```xml
<Cue><SCTE>BREAK</SCTE><Event_ID>evt-003</Event_ID><Event_Duration>30000</Event_Duration></Cue>
```

## Test Cases

### 1) UI Rendering and Control State

1. Open Metadata panel.
2. Toggle SCTE listening OFF.
3. Verify dependent SCTE fields are greyed/disabled.
4. Toggle SCTE listening ON.
5. Verify dependent fields are enabled.

Expected:
- UI clearly reflects enabled/disabled state.
- No console errors.

### 2) Cue Endpoint Availability

1. POST to `/api/encoder/{id}/cue` with matching JSON.

Expected:
- HTTP 200.
- Response includes `matched:true`.
- Logs include `SCTE cue received` and `matched` lines.

### 3) Exact Match and Case Sensitivity

1. Send `BREAK` (exact configured match).
2. Send `break` (different case).
3. Send `Send_Break` (different text).

Expected:
- `BREAK` matches.
- Others do not match.
- Non-matches log `No Matching Command` and no action execution.

### 4) Allowlist Behavior

1. Enable whitelist and add current client IP.
2. Send cue; verify accepted.
3. Remove client IP and retry.

Expected:
- Allowed IP accepted.
- Non-allowlisted source rejected with clear log.

### 5) Rate Limit Behavior

1. Keep default `5/10s`.
2. Send 6+ valid cues quickly.

Expected:
- First cues accepted until threshold.
- Excess cues rejected with rate-limit log.

### 6) Dedupe/Replay Behavior

1. Keep dedupe window `30s`.
2. Send same matching cue twice within 30 seconds.

Expected:
- First cue processed.
- Second cue deduped/replay-rejected with log entry.

### 7) Require Event ID and Token

1. Enable `Require Event ID` and send cue without eventId.
2. Enable `Require Token` and set token.
3. Send with missing/wrong token, then correct token.

Expected:
- Missing required values rejected with clear error/log.
- Correct values accepted.

### 8) HLS and SRT Output SCTE Visibility

1. Open HLS metadata viewer and SRT metadata viewer.
2. Send matching cue.

Expected:
- Viewer includes:
- `SCTE-35 Received: <value>`
- `SCTE-35 Sent: <value>`
- Encoder and system logs include received/matched/sent or rejected reasons.

### 9) SRT Input Delay UI Validation

1. Set Input Source = SRT.
2. Try invalid latency (`-1`, `95000`, non-number).
3. Try valid latency (`0`, `500`, `90000`).

Expected:
- Invalid values are rejected with clear UI feedback.
- Valid values accepted.

### 10) SRT Input Delay Persistence

1. Save input config with latency `500`.
2. Reload encoder config.

Expected:
- `input.json.srtLatency = 500`.

### 11) SRT Input Delay Runtime Application

1. Connect SRT input with latency `500`.
2. Inspect input status and logs.

Expected:
- Input status includes effective latency.
- Worker logs show configured latency and SRT URI latency parameter.
- Relay and direct-fallback paths both include latency behavior.

### 12) Sidecar Configuration Behavior

1. Enable SRT sidecar.
2. Set transport `HTTP POST JSON`, mode `mirror`, endpoint URL.
3. Save and inspect `srt.json`.

Expected:
- Sidecar settings persist correctly.
- UI re-renders saved values.
- No impact when sidecar is disabled.

### 13) HTTP-only and TCP-optional

1. Set listen transport `http` and validate cue endpoint works.
2. Set `tcp` and verify HTTP cue endpoint returns disabled message.
3. Set `both` and verify HTTP cue endpoint works.

Expected:
- Transport policy enforced as configured.

## Expected Log Categories

Verify logs can capture:
- received cues
- matched cues
- rejected cues
- sent cues
- deduped cues
- replay rejected cues

## Regression Checks

1. Existing HLS metadata behavior still functions.
2. Existing SRT output start/stop still functions.
3. Existing AAC/MP3 paths unaffected.
4. Existing control command behavior remains case-sensitive.

## Smoke Execution

- Linux: `./scripts/smoke-test.sh`
- Windows: `./scripts/smoke-test.ps1`

Both scripts should pass all checks in a clean environment.
