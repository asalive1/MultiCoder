# MultiCoder Technical Writeup (Engineering Handoff)

Last updated: 2026-05-16

## 1. Purpose and Scope

MultiCoder is a multi-instance audio encoding supervisor with per-encoder workers.
A single supervisor process serves the web UI and REST API, while each encoder runs in its own worker process.

Current functional target:
- 1 to 5 independent encoders.
- Independent start/stop for AAC, MP3, HLS, and SRT outputs per encoder.
- Runtime metadata ingestion and fan-out formatting for each output type.
- Input session controls and real-time input meters.

## 2. Runtime Architecture

### 2.1 Processes

- Supervisor: `multicoder-supervisor`
  - Hosts HTTP UI/API (default port 8050).
  - Stores/serves config files under `/etc/MC` and `/etc/encoderN`.
  - Starts worker processes on demand.
  - Proxies HLS playback requests.

- Worker (one per encoder): `multicoder-worker <1..5>`
  - Reads per-encoder config.
  - Runs control listener and metadata listener loops.
  - Launches FFmpeg subprocesses for each stream type.
  - Updates runtime state and input telemetry.

### 2.2 Process Isolation Model

Each encoder has independent stream state and files:
- `/etc/encoderN/input.json`
- `/etc/encoderN/control.json`
- `/etc/encoderN/metadata.json`
- `/etc/encoderN/aac.json`
- `/etc/encoderN/mp3.json`
- `/etc/encoderN/hls.json`
- `/etc/encoderN/srt.json`
- `/etc/encoderN/runtime_state.json`
- `/etc/encoderN/metadata_runtime.json`
- `/etc/encoderN/logs/EncoderN.log`

Restarting or stopping one encoder/stream should not affect others.

### 2.3 Control and Metadata IPC Ports

Default per-encoder formulas:
- Control listener port: `9100 + (encoderIndex-1)*10`
- Metadata listen port: `9000 + (encoderIndex-1)*10`

Examples:
- Encoder 1: control 9100, metadata 9000
- Encoder 2: control 9110, metadata 9010

## 3. Input Pipeline

## 3.1 Input Types

Configured by `input.json` and session overrides in `runtime_state.json`.
Supported input types:
- `rtp`
- `axia`
- `audio`
- `srt`

Session values (`sessionInputType`, `sessionRtpAddress`, etc.) override persisted config when present.

## 3.2 Input Connect Lifecycle

1. UI calls `POST /api/encoder/{id}/input/connect`.
2. Supervisor validates parameters by input type.
3. Supervisor writes session values into `runtime_state.json` and sets `inputConnected=true`.
4. If worker heartbeat is stale, supervisor auto-launches worker.
5. Worker monitor loop reads session config and attaches telemetry source.
6. Meter values are updated (`inputLevelL`, `inputLevelR`) in runtime state.

Disconnect path:
- `POST /api/encoder/{id}/input/disconnect` sets `inputConnected=false`.

## 3.3 RTP and Axia Input Details

- FFmpeg input arguments are generated via SDP (`input_rtp.sdp`) for dynamic payload RTP.
- RTP multicast join is interface-aware.
- Axia mode uses L24 RTP assumptions (stereo, 24-bit default, RTP port 5004).
- Livewire channel mapping formula:
  - `A = floor(N / 256)`
  - `B = N mod 256`
  - Multicast IP: `239.192.A.B`
  - Reverse: `N = A*256 + B`

## 3.4 Audio Device Input Details

- Windows:
  - FFmpeg capture via DirectShow (`-f dshow`).
  - Metering via waveIn, with WASAPI fallback.
- Linux:
  - FFmpeg capture via ALSA (`-f alsa`).
  - Direct audio metering is currently Windows-focused.

## 3.5 SRT as Input

If input type is `srt`, worker builds FFmpeg input URI from `input.json` (`srtHost`, `srtPort`, optional latency/passphrase).

SRT input delay implementation details:
- Persisted key: `input.json.srtLatency`.
- Runtime key: `runtime_state.json.sessionSrtLatency` (set on input/connect).
- Validation range: `0..90000` ms.
- Worker applies session value first, then saved config fallback.
- Delay is applied in both relay SRT URI and direct SRT fallback URI paths.

## 4. Encoder Output Pipelines

Each `Start*` command launches a dedicated FFmpeg process.
Each `Stop*` command terminates that process and updates runtime flags.
Process liveness is polled; unexpected exits are detected and reflected in state.

## 4.1 AAC Icecast Pipeline

Start command path:
- Supervisor sends control command `StartAAC` to worker control listener.
- Worker reads `aac.json`, builds FFmpeg command.

Encoding/output characteristics:
- Codec: AAC (`-c:a aac`)
- Default bitrate: `128k` (clamped 16..512 kbps)
- Output format: ADTS (`-f adts`)
- Content type: `audio/aac`
- Destination: Icecast URL converted to `icecast://user:pass@host:port/mount`

Mode behavior:
- `mode=legacy`: basic settings.
- `mode=advanced`: honors `sampleRate` and `profile`.
- Allowed AAC profiles:
  - `aac_low`
  - `aac_he`
  - `aac_he_v2`
  - `mpeg2_aac_low`

## 4.2 MP3 Icecast Pipeline

Start command path:
- Control command `StartMP3`.
- Worker reads `mp3.json`, builds FFmpeg command.

Encoding/output characteristics:
- Codec: `libmp3lame`
- Default bitrate: `128k` (clamped 32..320 kbps)
- Output format: MP3 (`-f mp3`)

Mode behavior:
- `legacy`: baseline behavior.
- `cbr`: allows explicit sample rate usage.
- `vbr`: applies `-q:a` with `vbrQuality` (0..9).

## 4.3 HLS Pipeline

Start command path:
- Control command `StartHLS`.
- Worker reads `hls.json` and optional `encoder_admin.json`.

FFmpeg output model:
- Segment muxing with playlist generation.
- Segment files: AAC segments (`segment-%d.aac`)
- Playlist: `index.m3u8`
- Segment directory: `/etc/encoderN/hls/segments`

Worker pre-start behavior:
- Ensures output directories exist.
- Removes stale playlist and old segments.

HLS playback serving:
- Worker can start a dedicated HTTP server on `hlsPlaybackPort` from `encoder_admin.json`.
- Supervisor also exposes proxy/fallback route:
  - `GET /encoder/{N}/hls/{filename}`

Playlist normalization performed in serving path:
- Ensures HLS version/features required by downstream clients.
- Injects `EXT-X-PROGRAM-DATE-TIME` in UTC if missing/incorrect.
- Injects metadata payload onto `EXTINF` lines.
- Honors `startTimeOffset` and `segmentSeconds` semantics.

## 4.4 SRT Output Pipeline

Start command path:
- Control command `StartSRT`.
- Worker reads `srt.json`, builds SRT URI and FFmpeg command.

Current implementation status:
- Only MPEG-TS transport is implemented at runtime.
- Non-MPEG-TS values are rejected.

Encoding/output characteristics:
- Audio encoded as AAC into MPEG-TS (`-f mpegts`).
- SRT URI includes mode/latency/buffer/streamid/passphrase/pbkeylen when set.

Security/transport notes:
- `passphrase` is supported in URI construction.
- `pbkeylen` supported values: 16, 24, 32.
- Logs mask passphrase when printing SRT URI.

## 5. Metadata Pipeline and Controls

## 5.1 Metadata Ingestion Modes

Configured in `metadata.json` and controlled via API.

Modes:
- `listen`: worker opens TCP server on `listenPort`, accepts inbound XML payloads.
- `pull`: worker actively connects to `dataConnectHost:dataConnectPort` and reads payload stream.

Runtime control endpoints:
- `POST /api/encoder/{id}/metadata/start`
- `POST /api/encoder/{id}/metadata/stop`
- `POST /api/encoder/{id}/metadata/connect` (test-connect helper)
- `GET /api/encoder/{id}/metadata/status`

Listener state is reflected via `runtime_state.json` (`metadataListenerRunning`).

## 5.2 Metadata Processing Flow

For each payload received:
1. Raw XML payload logged and cached.
2. `metadata_runtime.json` updated:
   - `eventCount`
   - `lastPayloadUtc`
   - `lastRawXml`
   - `lastFormattedAAC`
   - `lastFormattedMP3`
   - `lastFormattedHLS`
   - `lastFormattedSRT`
3. Stream-specific formatter executed.
4. If target stream is running and metadata enabled:
   - AAC/MP3: send Icecast admin metadata update.
   - HLS: inject metadata into playlist payload path.
   - SRT: formatted string is generated/logged (transport-specific carriage depends on receiver compatibility).

## 5.3 Metadata Formatting Rules

### AAC and MP3

Metadata parser options in `aac.json` and `mp3.json`:
- Template mode (`metaParser.template`), for example:
  - `artist={artist} | title={title}`
- Legacy mode fields:
  - `metaParser.separator`
  - `metaParser.fields[]`
  - `metaParser.includeStationId`

Recognized token aliases include:
- `title`, `artist`, `duration`, `category`, `trivia`, `cart`, `media_type`, `station`, `stationId`, etc.

### HLS

Metadata parser options in `hls.json`:
- `metaParser.profile`:
  - `orban` (preserves existing Orban-compatible behavior)
  - `triton` (Triton-oriented EXT payload shape and ID3 frame output)
  - `universal` (conservative standards-oriented output for broad audio-only HLS clients)
- `metaParser.method`:
  - `id3` (default)
  - `id3v2` (alias of `id3` output mode)
  - `ext`
  - `xmlPassthrough`
- `metaParser.scope`:
  - `current`
  - `currentFuture`
- `metaParser.tags[]` selects fields for frame/content construction.

`ext` mode output is profile-aware:
- `orban`: Orban-style JSON payload.
- `triton`: Triton-oriented `track` object payload.
- `universal`: ATS-style free-form text (`Artist - Title`).

### SRT

Current formatter returns key-value style payload:
- `title=...;artist=...;duration=...`

## 6. Control Plane and API Surface

## 6.1 Global/System

- `GET /health`
- `GET /api/encoders`
- `GET /api/syslog`

Admin/auth:
- `POST /api/admin/login`
- `GET /api/admin/config`
- `POST /api/admin/config`
- `GET /api/admin/interfaces`
- `POST /api/admin/interfaces`
- `GET /api/admin/audio-inputs`

## 6.2 Per Encoder

Status/config/logs:
- `GET /api/encoder/{id}`
- `GET /api/encoder/{id}/log`
- `GET /api/encoder/{id}/config`
- `POST /api/encoder/{id}/config/{section}` where section in:
  - `input`, `control`, `metadata`, `aac`, `mp3`, `hls`, `srt`

Input controls:
- `POST /api/encoder/{id}/input/connect`
- `POST /api/encoder/{id}/input/disconnect`
- `POST /api/encoder/{id}/input/preview-gain`
- `GET /api/encoder/{id}/input/levels`
- `GET /api/encoder/{id}/input/status`

Metadata controls:
- `POST /api/encoder/{id}/metadata/start`
- `POST /api/encoder/{id}/metadata/stop`
- `POST /api/encoder/{id}/metadata/connect`
- `GET /api/encoder/{id}/metadata/status`

Cue ingest controls:
- `POST /api/encoder/{id}/cue`
  - HTTP cue ingest endpoint on the same supervisor listener (port 8050 by default).
  - Matches incoming cue values case-sensitively against `metadata.scte.commandRows`.
  - If no exact match exists, logs `No Matching Command` and does not execute.
  - Applies optional source allowlist (IPv4/CIDR), rate limit, and dedupe/replay checks.
  - Writes SCTE runtime fields into `metadata_runtime.json`.

Stream controls:
- `POST /api/encoder/{id}/aac/start|stop`
- `POST /api/encoder/{id}/mp3/start|stop`
- `POST /api/encoder/{id}/hls/start|stop`
- `POST /api/encoder/{id}/srt/start|stop`

HLS serving:
- `GET /encoder/{id}/hls/{filename}`

## 6.3 Worker Control Commands (TCP)

Configured in `control.json` under `commands`:
- `StartAAC`, `StopAAC`
- `StartMP3`, `StopMP3`
- `StartHLS`, `StopHLS`
- `StartSRT`, `StopSRT`

Supervisor currently maps stream actions to these canonical command strings.

## 7. Configuration Parameters (Reference)

## 7.1 system.json

- `uiPort` (int): supervisor HTTP port.
- `logLevel` (string): `info|warning|debug`.
- `logRotSize` (int MB): rotation threshold.
- `logRetention` (int days): retention window.
- `encoderCount` (int 1..5): active encoder count.
- `adminUser` (string): admin username.
- `adminPass` (string): admin password value.
- `firstLoginRequired` (bool): force password change flow.
- `iceURL`, `iceMountAAC`, `iceMountMP3` (string): default UI/system values.
- `interfaces` (object): optional interface map.
- SCTE global keys:
  - `scteGlobalEnabled` (bool).
  - `scteGlobalRateLimitCount` (int, default `5`).
  - `scteGlobalRateLimitWindowSec` (int, default `10`).
  - `scteGlobalDedupeSeconds` (int, default `30`).
  - `scteLogLevel` (`info|debug|warning`).

## 7.2 input.json

- `inputType` (`axia|rtp|audio|srt`).
- `rtpAddress` (string IPv4/multicast).
- `rtpPort` (int).
- `rtpInterface` (string interface token).
- `rtpGain` (double dB).
- `bitrate` (int).
- `channels` (int).
- `sampleRate` (int Hz).
- `bitDepth` (int; 24 default for Axia/L24, 16 for legacy L16).
- `deviceIndex` (int, legacy/local audio token usage context).
- For SRT input path: `srtHost`, `srtPort`, `srtLatency`, `srtPass` (consumed when `inputType=srt`).
- `srtLatency` validation: `0..90000` ms.

## 7.3 control.json

- `controlPort` (int).
- `controlEnabled` (bool).
- `commands.startAAC|stopAAC|startMP3|stopMP3|startHLS|stopHLS|startSRT|stopSRT` (string).

## 7.4 metadata.json

- `mode` (`listen|pull`).
- `listenPort` (int).
- `dataConnectHost` (string).
- `dataConnectPort` (int/null).
- `scte` (object): per-encoder SCTE/cue settings:
  - `enabled`, `listenEnabled`, `listenTransport`, `listenPort`.
  - `cueDeliveryType` (`json|xml|both`).
  - `passthroughMode` (`off|pass-through|generate-from-cues`).
  - `requireEventId`, `requireToken`, `token`.
  - `watchTags` (string array).
  - `commandRows` (array of `{match, action}`).
  - `whitelistEnabled`, `whitelistEntries` (IPv4/CIDR array).
  - `overrideRateLimit`, `rateLimitCount`, `rateLimitWindowSec`.
  - `overrideDedupe`, `dedupeSeconds`.

## 7.5 aac.json

- `url`, `user`, `pass`.
- `mode` (`legacy|advanced`).
- `bitrate` (bps).
- `sampleRate` (Hz).
- `profile` (`aac_low|aac_he|aac_he_v2|mpeg2_aac_low`).
- `icyMetaInt` (int).
- `stationId` (string).
- `metaEnabled` (bool).
- `iface` (string).
- `metaParser.template` (string) or legacy parser fields.

## 7.6 mp3.json

- `url`, `user`, `pass`.
- `bitrate` (bps).
- `sampleRate` (Hz).
- `mode` (`legacy|cbr|vbr`).
- `vbrQuality` (0..9).
- `icyMetaInt` (int).
- `stationId` (string).
- `metaEnabled` (bool).
- `iface` (string).
- `metaParser.template` (string) or legacy parser fields.

## 7.7 hls.json

- `segmentSeconds` (int).
- `window` (int).
- `startTimeOffset` (int; negative offsets implement delayed start behavior).
- `lowLatency` (bool).
- `metaEnabled` (bool).
- `iface` (string).
- `metaParser.method` (`id3|ext|xmlPassthrough`).
- `metaParser.scope` (`current|currentFuture`).
- `metaParser.tags` (string array).

## 7.8 srt.json

- `transport` (currently must resolve to MPEG-TS at runtime).
- `mode` (`caller|listener|rendezvous` depending on endpoint compatibility).
- `host` (string).
- `port` (int).
- `streamId` (string).
- `timestamp` (bool).
- `latency` (ms).
- `buffer` (KB units used to build `sndbuf`).
- `encryption` (string, UI/config semantics).
- `passphrase` (string).
- `pbkeylen` (16|24|32).
- SCTE/output fields:
  - `metaFormat` (`id3|klv`).
  - `metaHandling` (`xmlPassThrough|customExport`).
  - `metaCustomFields` (string array).
  - `scteEnabled`, `sctePassthrough`.
  - `sidecar` object (`enabled`, `transport`, `mode`, `url`, `retries`).

## 8. Runtime State Files

## 8.1 runtime_state.json

Typical keys used by supervisor/worker:
- `workerHeartbeatEpoch`
- `workerAacRunning`, `workerMp3Running`, `workerHlsRunning`, `workerSrtRunning`
- `controlListenerRunning`, `metadataListenerRunning`
- `inputConnected`
- Session overrides:
  - `sessionInputType`
  - `sessionRtpAddress`, `sessionRtpPort`, `sessionRtpInterface`
  - `sessionAudioDevice`, `sessionSampleRate`
  - `sessionGainDb`
- Telemetry:
  - `inputLevelL`, `inputLevelR`

## 8.2 metadata_runtime.json

Typical keys:
- `eventCount`
- `lastPayloadUtc`
- `lastRawXml`
- `lastFormattedAAC`
- `lastFormattedMP3`
- `lastFormattedHLS`
- `lastFormattedSRT`
- `lastScteReceived`
- `lastScteSent`
- `lastScteRejected`

## 9. Logging and Observability

Logs:
- Per encoder: `/etc/encoderN/logs/EncoderN.log`
- System: `/etc/MC/EncoderSys.log`

Observed log categories:
- Worker lifecycle, heartbeat, stream start/stop.
- FFmpeg command lines and start failures.
- Input attach/no-data conditions.
- Metadata raw and formatted payload events.
- Supervisor command requests and ACK/failure traces.

## 10. Input Gain and SRT Input Improvements (2026-05-15 Update)

### 10.1 Input Gain Applied to Encoder Streams

**Problem Fixed**: Previously, the `rtpGain` parameter only affected VU meter display, not actual encoder input levels.

**Solution**: 
- Added helper methods in `Worker` class:
  - `getInputGainDb()`: Retrieves gain from `input.json::rtpGain` + session override (`sessionGainDb`)
  - `buildAudioFilterWithGain(double gainDb)`: Constructs FFmpeg `-af "volume=..."` filter
  
- Updated all stream start functions (`startAAC`, `startMP3`, `startHLS`, `startSRT`) to:
  1. Retrieve gain in dB
  2. Build audio filter if gain ≠ 0
  3. Insert `-af "volume=<linear_factor>"` into FFmpeg command line
  
- Filter formula: FFmpeg volume filter converts dB to linear factor: `linear = 10^(gainDb/20)`
- Clamped range: -24 dB to +24 dB (factors: 0.063 to 15.85)

**Configuration** (`input.json`):
```json
{
  "rtpGain": 0.0,
  "_comment_rtpGain": "Input gain in dB (-24 to +24). Applied to all encoder streams AND VU meter display."
}
```

**Behavior**:
- `rtpGain: 0.0` → No filter, unity gain (raw input)
- `rtpGain: 6.0` → Factor ~2.0 (6 dB boost)
- `rtpGain: -12.0` → Factor ~0.251 (12 dB cut)

Gain is logged when stream starts (e.g., "AAC settings: mode=legacy bitrate=128k gain=6.0dB").

### 10.2 Enhanced SRT Input Capabilities

**Problem Fixed**: SRT input only supported `mode=caller` (outbound connection). Could not receive from SRT senders or other encoders' SRT output.

**Solution**:
- Updated `buildFfmpegInputArgs()` SRT section to support both modes:
  - `srtMode: "caller"` (default) — SRT input connects OUT to a listening server
  - `srtMode: "listener"` — SRT input binds and waits for incoming connections
  
- Added new configuration fields to `input.json`:
  - `srtMode`: "caller" | "listener"
  - `srtStreamId`: Optional stream identifier for filtering
  - `srtPbkeylen`: Encryption key length (0, 16, 24, 32 bytes)

**Configuration** (`input.json`):
```json
{
  "srtHost": "127.0.0.1",
  "srtPort": 9250,
  "srtMode": "caller",
  "srtLatency": 120,
  "srtStreamId": "",
  "srtPbkeylen": 0,
  "srtPass": "",
  "_comment_srtMode": "caller: connect OUT to server. listener: bind and wait for incoming SRT connections (e.g., from another encoder).",
  "_comment_srtStreamId": "Optional SRT stream ID for identification/filtering.",
  "_comment_srtPbkeylen": "SRT encryption key length: 0 (none), 16, 24, or 32 bytes. Must match peer.",
  "_comment_srtLatency": "SRT latency buffer in ms. Typical: 20-500 ms."
}
```

**Use Case: Receiving from Another Encoder's SRT Output**:
1. Encoder A outputs SRT on port 9150 with `mode=listener` (waits for connection)
2. Encoder B inputs SRT from Encoder A's address/port with `srtMode="caller"` (connects to Encoder A)
3. Optional: Use `srtStreamId` to identify the source stream
4. Optional: Use `srtPbkeylen` + `srtPass` for encryption if both endpoints support it

**FFmpeg SRT URI Construction**:
- Caller mode: `srt://host:port?mode=caller&latency=120&streamid=...&pbkeylen=16&passphrase=...`
- Listener mode: `srt://0.0.0.0:port?mode=listener&latency=120&streamid=...&pbkeylen=16&passphrase=...`

Note: When using listener mode, ensure firewall rules allow inbound SRT connections on the specified port.

## 11. Current Gaps and Engineering Priorities

Based on current code and audit docs, priority follow-up for next engineer:

1. Harden production media pipeline parity
- Validate full codec/container behavior against operational requirements and source fixtures.

2. Tighten control command mapping
- `control.json.commands` is stored but supervisor currently emits canonical command names.
- If custom command strings are required, align supervisor-to-worker command translation with config.

3. Security hardening
- `adminPass` currently behaves as direct value in config comparison.
- Replace with salted hash + secure auth/session/token model.

4. Robust retries/backoff and health probes
- Expand restart/backoff strategy around FFmpeg child failures and metadata pull reconnect policy.

5. Expand automated integration testing
- Add end-to-end tests with real RTP/SRT fixtures and metadata payload validation.

## 12. Practical Handoff Notes

- Start with the worker stream functions and their FFmpeg arg builders when changing output behavior.
- Keep `runtime_state.json` backward-compatible; UI status depends on these keys.
- Treat HLS playlist normalization carefully; downstream devices are sensitive to tag order and UTC timestamps.
- Validate both Windows and Linux paths whenever touching file/path logic (`/etc/...` path translation is deliberate).
