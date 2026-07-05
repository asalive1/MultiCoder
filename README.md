# MultiCoder

**v2.3.4 — Multi-channel audio encoder with Icecast AAC/MP3, HLS, SRT, and configurable SCTE cue ingest.**

MultiCoder manages 1–5 independent encoding instances from a single web UI.
Each encoder runs as its own worker process; restarting one encoder never disturbs the others.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Quick Start (Docker)](#quick-start-docker)
3. [Docker Directory Mapping](#docker-directory-mapping)
4. [Configuration Files](#configuration-files)
5. [Web UI Guide](#web-ui-guide)
6. [Axia/Livewire Channel Formula](#axialivewire-channel-formula)
7. [SRT Metadata Limitations](#srt-metadata-limitations)
8. [Logging](#logging)
9. [Windows Install / Uninstall](#windows-install--uninstall)
10. [Building from Source](#building-from-source)
11. [Running Tests](#running-tests)
12. [CI Workflows](#ci-workflows)
13. [Troubleshooting](#troubleshooting)

---

## Architecture Overview

```
┌─────────────────────────────────────────────┐
│              Docker Host                    │
│                                             │
│  ┌────────────────────┐  ┌───────────────┐  │
│  │  multicoder (main) │  │   icecast     │  │
│  │                    │  │  (port 8000)  │  │
│  │  supervisor        │  └───────────────┘  │
│  │  (port 8050)       │                     │
│  │  + workers 1-5     │                     │
│  └────────────────────┘                     │
└─────────────────────────────────────────────┘
```

| Component | Binary | Role |
|-----------|--------|------|
| Supervisor | `multicoder-supervisor` | HTTP REST API, web UI server, worker watchdog |
| Worker N | `multicoder-worker N` | Audio capture, encode, Icecast/HLS/SRT sink, metadata |
| Icecast | Docker sibling | AAC + MP3 stream receive and redistribution |

**IPC:** Workers listen on localhost TCP ports (control: `9010+N*10`, metadata: `9000+N*10`).
The supervisor communicates with workers via these same ports.

---

## Quick Start (Docker)

```bash
# 1. Clone
git clone https://github.com/your-org/MultiCoder.git
cd MultiCoder

# 2. Copy environment file
cp .env.example .env
# Edit .env if you need non-default ports
# Tip: set UI_HOST_PORT for the host mapping, and UI_CONTAINER_PORT only if you need
# the supervisor to listen on a non-8050 port inside the container.

# 3. Build + start
docker compose up --build -d

# 4. Open the UI
open http://localhost:8050
```

Default credentials: **Admin / change-me**  
You will be prompted to change the password on first login.

---

## Docker Directory Mapping

| Container path | Purpose | Docker volume |
|---------------|---------|---------------|
| `/etc/MC/` | `system.json`, system logs | `mc_system` |
| `/etc/encoder1/` – `/etc/encoder5/` | Per-encoder JSON configs | `mc_enc1` – `mc_enc5` |
| `/etc/encoder#/logs/` | Encoder log files | (same volume) |
| `/etc/encoder#/hls/` | HLS playlist + segments | (same volume) |

To use **bind mounts** instead of named volumes (e.g., for easy access from the host):

```yaml
# in docker-compose.yml, replace volumes section:
volumes:
  mc_system:
    driver: local
    driver_opts:
      type: none
      o: bind
      device: /srv/multicoder/system
```

Or map directly in the service:

```yaml
services:
  multicoder:
    volumes:
      - /srv/multicoder/system:/etc/MC
      - /srv/multicoder/encoder1:/etc/encoder1
      # etc.
```

---

## Configuration Files

### `/etc/MC/system.json` — Application settings

| Key | Default | Description |
|-----|---------|-------------|
| `uiPort` | 8050 | HTTP port for the web UI |
| `logLevel` | `"info"` | `info` / `warning` / `debug` |
| `logRotSize` | 10 | Log rotation file size (MB) |
| `logRetention` | 14 | Days to keep rotated logs |
| `encoderCount` | 5 | Active encoder instances (1-5) |
| `adminUser` | `"Admin"` | Admin username |
| `adminPass` | `"change-me"` | Admin password hash |
| `firstLoginRequired` | true | Force password change on first login |
| `iceURL` | `"http://icecast:8000"` | Default Icecast server URL |
| `iceMountAAC` | `"/stream-aac"` | Default AAC mountpoint |
| `iceMountMP3` | `"/stream-mp3"` | Default MP3 mountpoint |
| `interfaces` | `{}` | Map of `{interfaceName: friendlyName}` |

### `/etc/encoder#/` — Per-encoder configs

Each section lives in a separate file for independent restart granularity:

| File | Contents |
|------|---------|
| `input.json` | Audio input type + parameters |
| `control.json` | Control TCP port + command strings |
| `metadata.json` | Metadata listen port |
| `aac.json` | Icecast AAC URL, credentials, IcyMetaInt |
| `mp3.json` | Icecast MP3 URL, credentials |
| `hls.json` | Segment length, window, metadata method |
| `srt.json` | SRT transport, mode, host, port, encryption |

See `configs/*.json.default` for annotated templates.

---

## Web UI Guide

### Header

- **Left:** Application name + version + system message banner (reconnect errors, warnings)
- **Right:** Admin Settings | Manual | Sign Out

### Encoder Status Bar

Displays all active encoders with per-stream status badges:

- `AAC LIVE` / `AAC Stopped`
- `MP3 LIVE` / `MP3 Stopped`
- `HLS LIVE` / `HLS Stopped`
- `SRT LIVE` / `SRT Stopped`

### Left Panel

When an encoder is selected, shows navigation links:

- **Input Selection** — gain slider (−15 to +15 dB, 1 dB steps, 0.25 manual entry), VU meters, input type
- **Control Configuration** — control port, per-stream start/stop command strings
- **Metadata Input Configuration** — listen port, connect port
- **AAC Icecast Configuration** — URL, creds, IcyMetaInt, metadata parser editor
- **MP3 Icecast Configuration** — same as AAC, independent config
- **HLS Output Configuration** — segment length, window, offset, low-latency toggle, metadata parser
- **SRT Output Configuration** — transport, mode, host, port, stream ID, latency, encryption
- **Tail Encoder Logs** — terminal-style log tail; pinned to bottom unless user scrolls up

### Metadata Parser Editor

Shown in centre-right panel when "Edit Metadata Parser" is clicked on Icecast or HLS config.

**Template mode:**  
Embed arbitrary strings using `{placeholder}` tokens. Supported tokens:  
`{artist}` `{title}` `{trivia}` `{category}` `{duration}` `{cart}` `{media_type}` `{IRSC}` `{stationId}`

Example: `artist={artist} | title={title} | {stationId}`

**Legacy mode:**  
Select a separator and an ordered list of XML field names to concatenate.

### SCTE-35 Cueing and Matching

- Global SCTE enable/defaults are configured in **Admin → Application Settings**.
- Per-encoder SCTE listen/match settings live in **Metadata Input Configuration**.
- HTTP cue endpoint is exposed at `/api/encoder/{id}/cue`.
- Matching is exact and case-sensitive against configured command rows.
- Non-matching cues are logged as `No Matching Command` and are not executed.
- Source allowlist accepts single IPv4 addresses and CIDR subnets.
- Rate limit and dedupe/replay windows inherit global defaults unless overridden per encoder.

### SRT Input Delay

- SRT input delay (receiver-side latency) is configured per encoder in **Input Configuration** when `Input Source = SRT`.
- Valid range is `0` to `90000` ms.
- Saved value persists to `input.json` and is copied into runtime session state on connect.
- Effective latency is shown in Input status summary and logged by worker SRT input paths.

### HLS Metadata Profile Selector

In the HLS metadata parser editor, select one profile:

- `Orban` — preserves the current Orban-specific metadata behavior.
- `Triton` — emits Triton-oriented EXT payloads and ID3/ID3v2 frame text.
- `Universal Standard` — emits conservative standards-oriented metadata for broad audio-only HLS compatibility.

The selected profile is stored in `hls.json` as `metaParser.profile`.

### Admin Settings

Access via top-right "Admin Settings" link. Requires credentials.

- Change admin password
- Set log level / rotation / retention
- Assign friendly names to network interfaces
- Set default Icecast destination
- Set number of active encoders (1–5)

---

## Axia/Livewire Channel Formula

MultiCoder implements the Axia multicast address formula as documented in the
Axia Multicast CODEC-USE specification.

**Forward (channel → multicast IP):**

```
A = N ÷ 256   (integer division)
B = N mod 256
Multicast IP = 239.192.A.B
RTP Port = 5004
```

**Examples:**

| Channel N | Multicast IP | Port |
|-----------|-------------|------|
| 0 | 239.192.0.0 | 5004 |
| 255 | 239.192.0.255 | 5004 |
| 256 | 239.192.1.0 | 5004 |
| 14101 | 239.192.55.21 | 5004 |
| 65535 | 239.192.255.255 | 5004 |

**Reverse (IP → channel):**  
`N = A × 256 + B`  
Only IPs in `239.192.0.0/16` are valid.

In the UI, select **Input Source → Axia/Livewire** and enter the channel number.
The computed multicast IP is shown immediately next to the field.

See `src/livewire/LivewireMapping.cpp` and `tests/unit/test_livewire.cpp`.

---

## SRT Metadata Limitations

SRT is a transport-layer protocol; it does not natively carry metadata.
MultiCoder's best-effort approach depends on the transport payload type:

| SRT Transport | Metadata method | Receiver requirement |
|--------------|----------------|---------------------|
| MPEG-TS | ID3v2 block inside PID `0x0011`; also signalled via PMT descriptor | Decoder must support MPEG-TS timed metadata (e.g. VLC ≥ 3.0, FFmpeg) |
| RTP | RTCP APP packet (`name="META"`) carrying JSON payload | Custom parsing required on receiver |

**Recommendation:** Use MPEG-TS transport for the best metadata compatibility.
Metadata is "now playing" only; no future-segment pre-fill is implemented for SRT.

---

## Logging

### Per-encoder logs

- Path: `/etc/encoder#/logs/Encoder#.log`
- Rotation: when file exceeds `logRotSize` MB (default 10 MB), renamed to `.1`
- Retention: logs older than `logRetention` days (default 14) are deleted on startup
- Events logged: input connect/disconnect, stream start/stop, metadata received, reconnect attempts, heartbeats (every 30 s)

### System log

- Path: `/etc/MC/EncoderSys.log`
- Same rotation/retention policy
- Events: UI config saves, admin changes, start/stop actions, authentication events

### Icecast reconnect behaviour

If an Icecast connection drops, the worker retries every 5 seconds for 60 seconds (12 attempts).
Each attempt is logged. After 12 failed attempts the stream is marked stopped and a banner message
is displayed in the UI. The admin must manually restart the stream.

---

## Windows Install / Uninstall

### Install

```powershell
# Run as Administrator
Set-ExecutionPolicy RemoteSigned -Scope CurrentUser
.\scripts\windows-install.ps1 -InstallDir "C:\MultiCoder" -EncoderCount 5
```

Then copy the built binaries and `www\` assets into `C:\MultiCoder\bin\` and `C:\MultiCoder\www\`.

To register as a Windows Service:

```powershell
sc.exe create MultiCoder-Supervisor binPath= "C:\MultiCoder\bin\multicoder-supervisor.exe" start= auto
sc.exe start MultiCoder-Supervisor
```

### Uninstall

```powershell
.\scripts\windows-install.ps1 -Uninstall -InstallDir "C:\MultiCoder"
```

### Windows directory layout

```
C:\MultiCoder\
├── system.json
├── logs\
├── bin\
│   ├── multicoder-supervisor.exe
│   └── multicoder-worker.exe
├── www\
├── encoder1\
│   ├── input.json  control.json  metadata.json
│   ├── aac.json  mp3.json  hls.json  srt.json
│   ├── logs\
│   └── hls\
│       └── segments\
├── encoder2\ … encoder5\
```

---

## Building from Source

### Prerequisites (Linux)

```bash
sudo apt-get install -y \
    build-essential cmake ninja-build git pkg-config \
    libportaudio2 libportaudio-dev libmp3lame-dev libfdk-aac-dev \
    libssl-dev libcurl4-openssl-dev libxml2-dev
```

### Build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -GNinja
cmake --build build
```

Binaries output to `build/src/`.

### Install

```bash
cmake --install build --prefix /opt/multicoder
cp -r www/ /opt/multicoder/www/
```

---

## Running Tests

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -GNinja
cmake --build build
ctest --test-dir build --output-on-failure
```

Unit tests cover:
- `LivewireMapping` — 13 test cases (formula, reverse, roundtrip, error cases)
- `HLS` — playlist generation, segment purge policy, media sequence

---

## CI Workflows

| Workflow | File | Trigger |
|----------|------|---------|
| Linux build + unit tests | `.github/workflows/build-linux.yml` | Push/PR to main |
| Docker build + smoke test | `.github/workflows/docker.yml` | Push to main |
| Windows build (best-effort) | `.github/workflows/build-windows.yml` | Push to main |

---

## Troubleshooting

**UI not loading (port 8050 shows nothing)**  
Check the container is running: `docker compose ps`  
Check logs: `docker compose logs multicoder`

**Icecast connection refused**  
Verify the `icecast` service is healthy: `docker compose logs icecast`  
Check credentials in `/etc/encoder#/aac.json` match `ICECAST_SOURCE_PASSWORD` in `.env`.

**Worker not starting**  
Check `/etc/encoder#/logs/` for log output.  
Ensure ports `9000-9049` (metadata) and `9010-9049` (control) are not in use.

**HLS segments not appearing**  
Check `/etc/encoder#/hls/segments/` exists and is writable.  
Verify `hlsEnabled: true` in the HLS config and that the encoder is running.

**Metadata not displaying**  
Confirm the metadata source is sending TCP data to the configured `listenPort`.  
Raw XML is written to `/etc/encoder#/meta_current.xml` for inspection.

---

## License

See [LICENSE](LICENSE).  
Not intended for commercial use. All dependencies are free and open-source.
