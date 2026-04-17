# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.46.0] - 2026-04-17

### Added

- Secure error-handling test suite (74 tests): device init/deinit, volume CRUD, LEB I/O, resize, and edge-case contract enforcement mirrored from the plain backend.
- Secure fault-injection test suite (2 tests): transactional safety under allocation failures and overwrite faults.
- Secure mutation-gate test suite (1 test): write-shutdown gate enforcement via `ubi_test_set_write_shutdown()`.
- Secure vol-id-watermark test suite (3 tests): persistent volume-ID high-watermark across remove, reinit, and slot re-indexing.

## [0.45.0] - 2026-04-16

### Changed

- Documentation aligned with secure backend implementation: updated flash footprint, struct sizes, test counts, coverage numbers, and Kconfig reference across all docs.
- Merged README Design Trade-offs into Key Properties; added authenticated encryption and dynamic resize entries.
- Removed completed crypto layer from roadmap (only shell commands remain as planned).

### Fixed

- CI `format-check`: clang-format version-dependent goto label formatting resolved with off/on guards.
- CI `cross-build` nRF5340 secure: added test random generator for builds without BLE IPC.
- CI `cross-build` STM32U5 secure: `ubi_device` size BUILD_ASSERT guarded for ARM vs POSIX `k_mutex` layout difference.

## [0.44.0] - 2026-04-16

### Added

- Forensic scan test suite (5 tests): verifies no plaintext data, volume names, or key material appear on flash after secure writes.
- Host-side Python forensic scanner and test docblock checker scripts.
- CI jobs: `format-check`, `forensic-scan`, cross-build secure dimension for STM32U5 and nRF5340.
- Init-time anchor re-creation: orphaned volumes automatically get a new hidden anchor at attach.

### Changed

- Test and coverage scripts accept mode parameter (plain/secure/chunked).

## [0.43.0] - 2026-04-16

### Added

- Chunked secure LEB mode: LEB records split into independently authenticated chunks for partial-read support. Configurable chunk size (256-65536 B).
- Chunked partial-read authentication: only touched chunks are verified, reducing read latency and RAM.
- Chunked geometry validation at init and budget accounting for chunked writes.
- Secure recovery test suite (8 tests): interrupted writes, anchor rewrites, reserved PEB commit faults, generation replay rejection.
- Flash write fault injection wired into the secure backend.

## [0.42.0] - 2026-04-15

### Added

- Sticky crypto read-only mode: event callback can escalate to device-wide write block (`-EROFS`); reads remain functional.
- Security event infrastructure: 10 event types (AUTH_FAILURE, FORMAT_VIOLATION, KEY_ROTATE_SOON/NOW, KEY_RETIRABLE, RNG_FAILURE, etc.) delivered through application callback.
- Freshness sync after every commit-visible mutation with configurable cadence.
- Key-version PEB refcount tracking with KEY_RETIRABLE signalling when a key version is fully erased.
- LEB usage budget tracking with soft/hard thresholds and pre-write rejection.
- Read-path key-version allowlist enforcement.
- Sensitive buffer zeroization (compiler-safe volatile memset).
- Runtime policy test suite (15 tests): read-only transitions, event escalation, budget thresholds, key retirement, allowlist rejection, mixed-key rotation.

## [0.41.0] - 2026-04-15

### Added

- Hidden per-volume anchor PEBs: each secure volume reserves one internal PEB preserving monotonic counter state across unmap, shrink, and erase.
- VID-domain counter floor: monotonic VID counter persisted in secure device header, reconstructed at attach.
- Last-writable-witness check: erase path rewrites anchor before erasing the last carrier of a volume's counter floor.
- Emergency free-PEB reserve: write path reclaims a dirty PEB before consuming the last free PEB.

### Changed

- Volume create rolls back on anchor allocation failure.
- Tests updated for anchor PEB overhead and wear-leveling participation.
- New docs: secure volume lifecycle and secure recovery scenarios.

## [0.40.0] - 2026-04-15

### Added

- Complete secure data-PEB runtime: volume create/resize/remove, LEB write/read/map/unmap, erase — all with authenticated encryption.
- Data-PEB scan pipeline: classifies PEBs into free/dirty/bad pools with full AEAD verification.
- Secure parity test suites (26 tests across 7 suites): functional parity with plain backend on encrypted flash.
- Flash geometry overlays for nRF5340 (4 KB erase) and STM32U5 (8 KB erase) on native_sim.
- Multi-geometry CI: tests run against three flash geometries (default, nRF5340, STM32U5).

### Fixed

- LEB write/nonce counters recovered from existing VID header (were hardcoded to 0).
- Write-block alignment padding for secure ciphertext+tag buffer.
- Missing bad-block torture and degraded-mode recovery in secure runtime.
- Geometry validation for erase/write block alignment.

## [0.39.0] - 2026-04-14

### Added

- Secure data-PEB I/O: authenticated read/write for EC headers, VID headers, and LEB data in single-tag mode.
- Domain-separated AAD serialization for all record types.

### Changed

- Key derivation centralized; parent authentication passed via typed context structs.

## [0.38.0] - 2026-04-15

### Added

- Secure reserved-PEB attach path: format-on-blank, attach-to-existing, mode mismatch detection.
- PSA Crypto integration: HKDF-SHA-256 key derivation, AES-128-CCM AEAD, salt generation.
- Encrypted dual-bank reserved PEB commit and authentication.
- Secure attach test suite (8 tests): format, re-attach, mode mismatch, freshness rejection, callback validation.

## [0.37.0] - 2026-04-14

### Added

- Secure public API types: crypto config, event types (tagged union), freshness descriptor, policy struct, and callback typedefs.
- Secure Kconfig surface: master enable, budget limits, rotation thresholds, chunked mode, PEB cache, freshness sync, strict RO policies.
- Crypto fault injection hooks (7 stages) for integration tests.
- Secure test profiles and board configs with Mbed TLS PSA.

## [0.36.0] - 2026-04-14

### Changed

- Full backend ops dispatch: all public API functions route through a vtable (11 ops). Plain backend functions renamed to `ubi_plain_*`.
- Internal headers decoupled from plain-specific includes, making them backend-agnostic.

## [0.35.0] - 2026-04-14

### Changed

- Unified init API: `ubi_device_init(mtd, crypto_cfg, &ubi)`. Passing `NULL` selects plain; non-NULL selects secure. Runtime backend dispatch via ops vtable.

## [0.34.0] - 2026-04-13

### Changed

- Secure architecture spec: per-device runtime mode selection (not per-build), tagged-union events, `check_freshness` confirmed as attach-time only.
- Repository layout split into `common/`, `plain/`, `secure/` namespaces for library and test sources.
- Format script made recursive with `--check` mode for CI.

## [0.33.0] - 2026-04-11

### Changed

- `design_proposal_crypto.md` promoted to `secure_architecture.md` as a first-class architecture document.
- `architecture.md` renamed to `plain_architecture.md`.
- All doc navigation updated for the new names.

## [0.32.0] - 2026-04-11

### Changed

- Secure architecture spec rewrite (v6): hidden per-volume anchor PEBs, secure device header with crypto metadata, full counter continuity framework, renamed child keys to domain names, simplified chunked mode (no per-chunk subkeys), expanded write-budget enforcement, precise AAD byte layouts for all record types, tagged-union events with verdicts.

## [0.31.0] - 2026-04-10

### Changed

- Secure architecture spec rewrite (v5): mode detection rules, normative KDF labels, single-tag CCM payload limit, zero-length LEB encoding, tail-padding rules, parent EC key_version in VID and LEB AAD, release checklist (Appendix C).

## [0.30.0] - 2026-04-09

### Added

- "Why UBI for Zephyr" positioning document: gap analysis, comparison with FCB/NVS/ZMS/LittleFS.

### Changed

- Secure architecture spec rewrite (v4): plain-core baseline assumptions, application boundary clarification, expanded key derivation and freshness-sync.

## [0.29.0] - 2026-04-09

### Added

- Persistent vol_id high-watermark: volume IDs are never reused across the device lifetime. Overflow returns `-ENOSPC`.
- Test suite (4 tests): reuse prevention, cross-reboot persistence, slot stability, overflow.

### Changed

- Volume matching by `vol_id` instead of positional index; re-index loop eliminated.

## [0.28.0] - 2026-04-09

### Added

- Central mutation gate: per-device read-only flag checked before every public mutator. Three mutation classes (reserved metadata, data path, maintenance).
- Runtime degradation detection and self-healing via reserved PEB bank recovery.
- Test-only write-shutdown API.
- Test suite (5 tests): write-shutdown, degradation, transparent recovery, bank recovery.

## [0.27.0] - 2026-04-10

### Changed

- Data PEB commit order changed to EC -> DATA -> VID. VID header is now the sole commit-visible record.
- Init scan distinguishes free PEBs from uncommitted writes by probing the data area.

### Fixed

- Uncommitted writes were misclassified as free under the new write order.

## [0.26.0] - 2026-04-09

### Changed

- Erased-state detection uses hardware-reported erase value instead of hardcoded `0xFF`.

## [0.25.0] - 2026-04-08

### Changed

- Secure architecture spec rewrite (v3): init classification, write/read/erase paths, key lifecycle with refcount retirement, events and policy, cost model, illustrative API (Appendix A).

## [0.24.0] - 2026-04-03

### Added

- Secure architecture design proposal (v2): AES-128-CCM for all on-flash structures, ESSIV nonces, per-domain key derivation, anti-rollback, crash-safe key rotation, external AAD callback.

## [0.23.0] - 2026-04-03

### Added

- nRF5340 DK board support (64 KB UBI partition, 4 KB erase blocks).

### Changed

- CI split into parallel jobs: native-tests, cross-build, coverage.
- Concurrency control, least-privilege permissions, path filters, SHA-pinned actions.

## [0.22.0] - 2026-04-02

### Added

- Flash I/O fault injection: controllable write and erase failures.
- Test suite `ubi_io_faults` (24 tests): flash I/O and malloc fault sweeps.
- Test suite `ubi_init_errors` (33 tests): geometry validation, partition guard, format/header corruption at init.
- 34 new error-handling tests and 6 new recovery tests.
- Long-term EC counter equality test (500 cycles, max deviation <= 2).

## [0.21.1] - 2026-04-02

### Fixed

- Sphinx `-W` flag: warnings now fail the doc build.
- Breathe + Doxygen 1.9.8 compatibility: switched to `doxygengroup` directives.

## [0.21.0] - 2026-04-02

### Added

- Static memory backend (default): all allocations via `k_mem_slab` pools instead of global heap.
- Memory abstraction layer with Kconfig-selectable backend (static/heap).
- Init-time validation: static backend verifies flash geometry fits configured pools.
- Dual-backend CI runs.

### Changed

- PEB tracking items share a union for in-place retyping during state transitions.
- Fault injection routed through memory abstraction layer.

## [0.20.1] - 2026-04-02

### Changed

- Removed read-write lock from roadmap (per-device mutex is sufficient).
- Renamed "User-space tools" to "Shell commands" in roadmap.

### Fixed

- Test API functions visible in Doxygen output.

## [0.20.0] - 2026-04-01

### Added

- Single-handle-per-partition guard: prevents two device handles for the same flash partition.
- Concurrency test suite (5 tests): multi-threaded readers/writers, deinit quiescence, double-init guard.

### Fixed

- `ubi_device_deinit()` acquires mutex before teardown, preventing races with in-flight operations.

## [0.19.0] - 2026-04-01

### Added

- Volume config validation, device/volume header semantic checks.
- Invariant checker API for tests.
- Allocation fault injection via Kconfig.
- Shared test fixtures and raw flash write helpers.

### Changed

- Transactional `ubi_volume_create()` and shrink: RAM state consistent with flash on failure.
- Copy-on-write `leb_write()`: old mapping preserved until new PEB is fully written.
- `ubi_leb_unmap()` is idempotent; `ubi_leb_map()` is a no-op when already mapped.

### Fixed

- PEB tracking loss on allocation failure during bad-block handling.

## [0.18.0] - 2026-04-01

### Added

- Overview doc with mental model, six-step lifecycle, and Mermaid diagrams.
- Architecture guide expanded: core invariants, Mermaid flowcharts, degraded-mode table.

### Changed

- README rewritten as landing page with stack diagram, key properties, quality metrics.
- All doc pages restructured with "what this page covers" framing.

## [0.17.1] - 2026-03-31

### Fixed

- Doc source file table and test counts aligned with Phase 3 file splits.
- Sample app expanded to demonstrate full lifecycle.

### Changed

- Removed per-file Doxygen version/date tags; replaced Yoda conditions with idiomatic style.

## [0.17.0] - 2026-03-31

### Changed

- Split `ubi_core.c` into init and runtime modules; split `ubi_io.c` into metadata and data modules. No functional changes.

## [0.16.0] - 2026-03-31

### Added

- `read_only_degraded` exposed in device info struct.
- Cached geometry in device struct (no flash I/O for `get_info()`).
- Thread-safety notes and precise `\retval` docs on all public functions.

### Changed

- `allocated_peb_count` renamed to `reserved_peb_count`.
- `-EROFS` propagated from degraded reserved PEB scan through init and mutators.

## [0.15.0] - 2026-03-31

### Added

- Volume name validation helpers (bounded, NUL-safe).
- Semantic validation of on-flash device headers.
- Flash geometry validation at init.

### Fixed

- Eliminated `strlen()` on raw flash fields (memory safety).
- Sequence number monotonicity: `global_sqnum` set to `max + 1` after scan.
- Volume headers always read from highest-revision reserved PEB.
- Reclaim error paths no longer leak PEBs.

## [0.14.0] - 2026-03-30

### Changed

- Reserved PEB module renamed with `ubi_flash_res_peb_*` prefix.
- Volume module deduplicated with shared helpers.

## [0.13.0] - 2026-03-30

### Added

- Bad block torture test with configurable cycles and per-PEB erase attempts.
- Runtime average erase counter tracking.

### Fixed

- Use-after-free in `leb_write()` write-fail path.
- Bad-block paths now update erase counter averages.

## [0.12.0] - 2026-03-30

### Added

- Configurable write retry count for data PEBs.

### Fixed

- Write failure in `leb_write()` now properly marks PEB as bad.

## [0.11.0] - 2026-03-27

### Added

- Initial crypto layer design proposal (AES-128-CCM via PSA Crypto API).

## [0.10.0] - 2026-03-27

### Added

- Reserved PEB management extracted into dedicated module.
- Configurable reserved PEB count (2-4) for cold spare support.
- 12 new tests (recovery, data_size boundary).

## [0.9.0] - 2026-03-26

### Changed

- Monolithic source restructured into core, volume, LEB, I/O, and cache modules.
- Init scan decomposed into 5 helpers.
- Overhauled Doxygen and Kconfig.

### Fixed

- 11 bugs from code quality audit.
- Memory leak in duplicate-LEB resolution.

## [0.8.0] - 2026-03-25

### Added

- Sphinx documentation with Read the Docs theme, deployed to GitHub Pages.
- Doxygen + Breathe auto-generated API reference.
- Architecture guide, getting started, configuration reference, contributing guide.

## [0.7.0] - 2026-03-25

### Added

- native_sim board support.
- Test suites: error handling, boundary, recovery, stress — 87 tests total.
- GitHub Actions CI with build, test, and Codecov.
- Coverage infrastructure and CI scripts.
- Test strategy documentation.

### Fixed

- `west.yml`: `cmsis_6` renamed to `cmsis` for Zephyr v4.0.0.

## [0.6.0] - 2026-03-24

### Added

- Architecture guide with ASCII diagrams (on-flash layout, PEB lifecycle, init flow, wear-leveling).
- Development roadmap and contributor guide.

### Fixed

- EBA table corruption during init when resolving sequence number conflicts.

## [0.5.0] - 2025-09-25

### Added

- Mutex-based synchronization for thread-safe operations.

## [0.4.0] - 2025-09-24

### Added

- Sample application for STM32U5.

### Changed

- Optimized flash I/O and improved logging.

## [0.3.0] - 2025-09-21

### Added

- `.clang-format` configuration.

### Changed

- Migrated from low-level flash APIs to Zephyr Flash Map (Flash Area API).

## [0.2.0] - 2025-09-10

### Added

- Volume support with static and dynamic types.
- Runtime resizing, write block alignment, partial dual-bank support.
- Hardware tests on STM32U5.

## [0.1.0] - 2025-07-25

### Added

- Initial release: device init/deinit, LEB I/O (map, unmap, read, write), PEB statistics.
- Hardware integration tests and sample application for STM32U5.
- Environment setup documentation.
