# MultiCoder Reliability Hardening

## Phase Status (Refreshed 2026-09-13)

### Completed
- P0 repository hygiene and contamination remediation completed.
- LF line-ending policy enforced and normalization commits merged.
- Worker/supervisor lifecycle hardening merged (idempotent starts/stops, cleanup protections).
- HLS failover-safe cleanup bypass groundwork merged (`failoverConfigured`, `storageBackend`).
- Supervisor SCTE metadata runtime persistence hardened to atomic writes.
- Bounded worker dispatch queue added for metadata and SCTE sidecar fan-out.
- Deterministic per-stream lifecycle states exposed through runtime state and supervisor status.
- Linux CI expanded with sanitizer and clang-tidy enforcement builds.

### In Progress
- P1 reliability hardening is implemented; remaining follow-up is operational tuning based on test/field feedback.

### Not Started
- Failover activation beyond current groundwork.
- Stress-harness and soak-test automation.

## Next 3 Implementation Slices

## Slice 1: Build-Time Memory/UB Detection Scaffolding (Executed)

Status: Executed in this update.

Files changed:
- `CMakeLists.txt`
- `README.md`

Code-level modifications:
- Added `MULTICODER_ENABLE_SANITIZERS` CMake option (ASan+UBSan for Debug on GCC/Clang).
- Added `MULTICODER_ENABLE_CLANG_TIDY` CMake option with auto-discovery and warning when unavailable.
- Added README commands for sanitizer and clang-tidy build modes.

Risk of regression:
- Low. Defaults are OFF, so production/runtime behavior is unchanged.

Validation:
- Standard debug build/test remains green.

Rollback:
- Revert `CMakeLists.txt` and `README.md` changes from this slice.

## Slice 2: Bounded Metadata + Sidecar Event Queue (Executed)

Status: Executed in this update.

Primary files likely to change:
- `src/worker/worker.cpp`
- `src/worker/worker.h`
- `configs/metadata.json.default`
- `configs/srt.json.default`
- `tests/unit/test_metadata.cpp`

Implemented modifications:
- Introduce bounded in-memory queue for accepted metadata/SCTE sidecar events.
- Explicit backpressure policy:
  - Metadata path: preserve ordering for queued events.
  - Sidecar path: bounded retry and deterministic drop accounting when queue full.
- Add queue depth and dropped-event counters to runtime metadata status.

Risk of regression:
- Medium. Concurrency and ordering behavior changes require careful locking and tests.

Tests to add:
- Queue ordering under burst input.
- Backpressure behavior under sustained overload.
- No-loss assertion for accepted metadata events within queue capacity.

Rollback:
- Compile-time or config kill-switch to bypass queue layer and use prior direct dispatch path.

## Slice 3: Deterministic Stream Lifecycle State Machine (Executed)

Status: Executed in this update.

Primary files likely to change:
- `src/worker/worker.cpp`
- `src/supervisor/supervisor_api.cpp`
- `docs/MULTICODER_TECHNICAL_WRITEUP.md`
- `tests/unit/test_config.cpp`

Implemented modifications:
- Add explicit per-stream states: `stopped`, `starting`, `running`, `stopping`, `failed`.
- Persist state transitions in runtime state and expose through status endpoints.
- Tie state transitions to process checks and command ACK paths for consistency.

Risk of regression:
- Medium. Touches status semantics and operator-visible state.

Tests to add:
- Transition validity tests.
- Failure-path state tests (start failure, unexpected process exit).
- API contract tests ensuring backward compatibility for existing boolean flags.

Rollback:
- Retain and prioritize legacy boolean running flags in API; feature-flag state machine publication.

## Acceptance Gates For Current Plan
- Backward compatibility preserved for existing config/API fields.
- Metadata integrity maintained with ordering guarantees for accepted events.
- No destructive HLS cleanup in failover-configured mode.
- Build-time reliability tooling available and documented.

## Immediate Outcome Of This Update
- Slice 1 has been executed immediately.
- Slices 2 and 3 are now implemented.
- Remaining work is refinement, stress coverage, and eventual failover activation.