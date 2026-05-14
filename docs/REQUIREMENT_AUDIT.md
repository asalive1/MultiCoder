# MultiCoder Requirement Audit (Final Pass)

Date: 2026-04-05

This checklist audits the repository against the requested spec and highlights what has been fully implemented, partially implemented, and what remains blocked by environment/toolchain constraints.

## Legend

- COMPLETE: Implemented in this repo.
- PARTIAL: Implemented scaffold/behavior but not full production-grade media pipeline.
- BLOCKED: Cannot be fully validated in current environment (missing local C++ toolchain / runtime services).

---

## 1) Architecture and Repo Structure

- COMPLETE: Split binaries into supervisor (`multicoder-supervisor`) and worker (`multicoder-worker`).
- COMPLETE: CMake project, source folders, tests, docs, Docker assets, scripts, CI workflows.
- COMPLETE: Reference source tree created at `reference/` and populated from provided source files.
- PARTIAL: Worker-to-supervisor IPC is local REST/control stubs; production-grade robust IPC contracts can be expanded.

## 2) Baseline Behavior Preservation

- PARTIAL: Worker code scaffold includes control/meta listeners and stream state transitions.
- PARTIAL: Stream start/stop and status API are functional for orchestration.
- PARTIAL: Full PortAudio/AAC/MP3/HLS/SRT real encode pipeline is not yet production-complete in this scaffold.

## 3) Multi-Encoder Requirements (1-5)

- COMPLETE: Supervisor tracks 1-5 encoders.
- COMPLETE: Start/stop of one encoder stream does not toggle others.
- COMPLETE: Per-encoder config files separated by feature.
- PARTIAL: Process supervision/restart policy can be hardened further (backoff/jitter/health checks).

## 4) Docker and Directory Layout

- COMPLETE: `docker-compose.yml` exposes UI port 8050 by default.
- COMPLETE: `/etc/MC`, `/etc/encoder#`, `/logs`, `/hls`, `/hls/segments` initialization script.
- COMPLETE: Init script creates defaults and runtime state files.
- PARTIAL: Permission model is present; SELinux/AppArmor hardened profile not added.

## 5) Admin/Auth

- COMPLETE: Admin login endpoint and config persistence (`/api/admin/login`, `/api/admin/config`).
- COMPLETE: Default `Admin` / `change-me` behavior supported via system config.
- COMPLETE: First-login-required field parsed and returned.
- COMPLETE: Interface discovery endpoint (`/api/admin/interfaces`) implemented.

## 6) UI Requirements

- COMPLETE: Banner, top menu, stream status cards, per-encoder panel navigation.
- COMPLETE: Input, control, metadata, AAC/MP3/HLS/SRT config forms.
- COMPLETE: Tail log view refresh behavior with pin-to-bottom when user at bottom.
- COMPLETE: Metadata viewer sections and previous events scaffold.
- PARTIAL: Some production stream telemetry counters are placeholders.

## 7) Axia/Livewire Formula

- COMPLETE: Implemented in `LivewireMapping` module and unit-tested.
- COMPLETE: Forward formula, reverse formula, range validation, roundtrip tests.
- COMPLETE: RTP port constant 5004.

## 8) Listener State Persistence

- COMPLETE: Added persisted per-encoder runtime state file:
  - `/etc/encoder#/runtime_state.json`
  - keys: `controlListenerRunning`, `metadataListenerRunning`
- COMPLETE: Supervisor updates these flags on control/metadata start/stop API calls.
- COMPLETE: Encoder status API now reports listener state.

## 9) JSON Hardening

- COMPLETE: Added local JSON utility layer (`SimpleJson`) and removed ad-hoc string parsing for login/config state reads in supervisor and worker config port reads.
- COMPLETE: Added unit tests for JSON utility.

## 10) Logging

- COMPLETE: Per-encoder log files and system log tail endpoints exist.
- COMPLETE: Rotation threshold logic in worker (10MB default) and retention tests included.
- PARTIAL: Production retention sweeper for rotated files on schedule can be expanded.

## 11) CI/CD

- COMPLETE: Linux build/test workflow.
- COMPLETE: Docker build/smoke workflow.
- COMPLETE: Windows best-effort build/package workflow.

## 12) Tests

- COMPLETE: Livewire mapping tests.
- COMPLETE: HLS playlist and purge policy tests.
- COMPLETE: Config validation tests.
- COMPLETE: Metadata parsing/template tests.
- COMPLETE: Log policy tests.
- COMPLETE: Simple JSON utility tests.
- COMPLETE: Integration test plan and smoke script.

## 13) IntelliSense/Include-Path Problems Reported by User

- COMPLETE: Added workspace C/C++ settings and `c_cpp_properties.json`.
- COMPLETE: Added local Catch2 IntelliSense fallback header in `third_party/catch2`.
- BLOCKED: Host machine has no discoverable C/C++ compiler toolchain (`cl`, `g++`, `clang++` all unavailable in terminal and common install paths). Standard-library headers like `<string>` cannot be resolved by cpptools without a valid compiler/sysroot.
- Mitigation in repo: VS Code config is in place; once a compiler is installed and selected, those squiggles should clear.

---

## Remaining Functional Gaps to Fully Productionize

1. Implement full media encode pipeline parity with baseline (PortAudio/RTP/SRT input + AAC/MP3/HLS/SRT outputs with real codecs and reconnect loops).
2. Strengthen supervisor-worker IPC contract (timeouts, retries, structured errors, health probes).
3. Add robust auth security (hashing/salting admin password, auth tokens/sessions).
4. Add end-to-end integration test automation in CI environment with real media fixtures.

---

## Immediate Next Action (for local environment)

Install a C++ toolchain on the machine running VS Code (MSVC Build Tools, LLVM, or MSYS2 GCC), then run C/C++: Select IntelliSense Configuration to bind cpptools to that compiler.
