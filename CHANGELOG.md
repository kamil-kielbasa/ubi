# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.65.0] - 2026-05-07

### Changed

- The in-tree test suites are now organised to match the project
  coding style. Lifecycle hooks, helpers and ZTEST cases are grouped
  under canonical section banners with documented setup, teardown
  and per-case scenarios. This change is internal: behaviour of the
  library, public API and on-flash format are unaffected. Downstream
  users who run the bundled tests will see clearer log headings and
  consistent file structure across every suite.

## [0.64.0] - 2026-05-07

### Changed

- The secure backend now treats misuse of internal vtable entry
  points the same way as the public dispatcher: passing `NULL`
  arguments to `ubi_secure_anchor_create()` or
  `ubi_secure_device_init()` returns `-EINVAL` with a diagnostic
  log instead of tripping an assertion. Production builds without
  asserts no longer experience undefined behaviour on a `NULL`
  misuse — the call fails cleanly.
- The secure onboarding sample now emits its output through the
  Zephyr logging subsystem (`LOG_INF` / `LOG_ERR`) instead of
  `printk`. Sample messages can therefore be filtered, redirected
  and silenced via the same log-level controls as the library
  itself, and every failure path includes the underlying return
  code.

## [0.63.0] - 2026-05-07

### Changed

- Misuse of the public API now surfaces as an actionable diagnostic
  instead of a silent `-EINVAL`. Calling `ubi_device_*`,
  `ubi_volume_*` or `ubi_leb_*` with `NULL` arguments or out-of-range
  identifiers logs which argument was invalid (including pointer
  values for multi-argument checks), so integration bugs are caught
  at the first failing call. Return codes are unchanged.
- The plain sample now emits its output through the Zephyr logging
  subsystem (`LOG_INF` / `LOG_ERR`) instead of `printk`. Sample
  messages can therefore be filtered, redirected and silenced via
  the same log-level controls as the library itself, and every
  failure path includes the underlying return code.

## [0.62.0] - 2026-05-07

### Added

- Cross-reference tables in the secure documentation so users can
  jump from a documented behaviour straight to the regression test
  that locks it. `secure_recovery_notes.md` now ends with a
  "Scenario → ZTEST coverage" table covering all nine power-cut /
  anchor-migration / dual-bank scenarios. `secure_volume_lifecycle.md`
  ends with a "Lifecycle step → code & ZTEST coverage" table mapping
  each of the seven volume lifecycle steps (create / grow / shrink /
  remove / unmap / erase & reclaim / reboot recovery) onto the
  implementing function in `lib/src/secure/` and the secure ZTESTs
  that exercise it. `secure_architecture.md` gains an Appendix D
  ("Release checklist → ZTEST coverage") that mirrors Appendix C
  bullet-by-bullet, pointing each release-checklist item at the
  concrete tests under `tests/src/secure/`. Two review-only bullets
  (scratch zeroization, `device_revision` ordering) are tagged as
  such with a short rationale.

## [0.61.0] - 2026-05-07

### Added

- A ready-to-run onboarding sample for the secure (AEAD-backed)
  UBI backend, so users have a single, copy-pasteable starting
  point for bringing up an encrypted volume. Build it with
  `west build -p -b <board> sample/ -- -DOVERLAY_CONFIG=boards/secure.conf`;
  CMake automatically selects the secure entry point when
  `CONFIG_UBI_CRYPTO=y`. The sample shows how to initialise PSA
  Crypto, import a root key, wire the four secure callbacks
  (`get_key_id`, `check_freshness`, `sync_freshness`, `event_cb`)
  and run a minimal single-version key policy end-to-end.
- The secure sample is now compile-verified on every PR for the
  supported Cortex-M targets (`b_u585i_iot02a` and
  `nrf5340dk/nrf5340/cpuapp`, both `static` and `heap` backends),
  so regressions in the secure onboarding flow are caught in CI
  instead of by downstream users.

### Fixed

- The library no longer fails to build for downstream consumers
  that link it without `CONFIG_UBI_TEST_API_ENABLE=y` (including
  the new sample). The `struct ubi_device` size build-time
  assertions previously hard-coded the in-tree test layout; they
  now account for the test-only field and keep locking both the
  production and test layouts on every supported architecture.

## [0.60.0] - 2026-05-07

### Added

- Forced-rekey end-to-end regression test
  (`ubi_secure_runtime_policy.test_forced_rekey_with_stale_objects`)
  covering the scenario from `secure_architecture.md` Appendix B §3:
  after the operator raises `requested_write_key_version` while
  stale free, dirty and mapped data PEBs from the previous key
  version still exist on flash, the attach path eagerly upgrades
  reserved metadata to the new key version, every previously
  committed payload remains readable across the rotation, new
  writes commit under the new key version, and the
  `KEY_RETIRABLE` event for the retired key version is withheld
  until the last stale object has been recycled and then fires
  exactly for the retired key version. This closes the final
  outstanding security-relevant audit gap.

## [0.59.0] - 2026-05-07

### Added

- Chunked-write 48-bit AEAD counter overflow regression test
  (`ubi_secure_chunked.test_chunked_write_overflow_rejected`). A
  multi-chunk write whose projected per-LEB write counter would
  exceed `UBI_SECURE_COUNTER_MAX` is now proven to fail closed before
  any flash mutation: the call returns `-EOVERFLOW`, exactly one
  `KEY_ROTATE_NOW` event is emitted with `usage_pct=100`, no free
  PEB is consumed, and the previously committed payload survives.
  This closes the remaining safety-net gap identified by the audit
  for the chunked write path.

### Changed

- New test-only API
  `ubi_secure_test_set_leb_write_counter_floor()` /
  `ubi_secure_test_get_leb_write_counter_floor()` (compiled in only
  when `CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION=y`). It clamps the
  per-LEB write counter recovered from flash up to a configurable
  floor so the chunked-write overflow guard can be exercised
  end-to-end without performing 2^48 real chunk encryptions.

## [0.58.0] - 2026-05-06

### Fixed

- Restored the `native_sim` chunked test build. The chunked board
  config inherited an outdated baseline and no longer matched what
  the secure test suites depend on; this prevented the chunked
  configuration from being verified at all.
- Removed a cascading-failure pattern in the secure test runner:
  when a single test aborted before completing teardown, every
  subsequent test in the same binary observed exhausted internal
  pools and reported a misleading allocation failure. The shared
  test-only reset path now fully restores the allocator state, so
  a leak in one test no longer poisons the rest of the run.
- Hardened the chunked tamper test against the flash simulator's
  NAND-style write semantics. The previous bit-flip strategy was
  silently a no-op on roughly half of the runs, depending on the
  random ciphertext byte, and is now replaced by a deterministic
  destructive write with a read-back guard.
- Aligned the coverage-secure CI configuration with the runtime
  contract assumed by the coexistence test (two concurrent device
  handles). The CI build was previously inheriting the single-handle
  default, so the coexistence test failed only on the
  `coverage-secure` job.
- Tightened the host-side flash forensic scanner
  (`scripts/scan_flash.py`) so that it no longer flags ciphertext as
  a leak. The previous 8-character printable-ASCII threshold was
  statistically guaranteed to false-positive on encrypted regions
  once a second secure partition was added to the test image; the
  scanner now uses a 20-character threshold and ignores the secure
  prefix magic, while explicit binary patterns (test arrays, root
  key, volume names) still detect real plaintext leaks.

## [0.57.0] - 2026-05-06

### Added

- Plain + secure backend coexistence regression test
  (`ubi_secure_coexistence` suite): a plain UBI device on
  `ubi_partition` and a secure UBI device on a second partition
  `ubi_partition_2` operate in parallel without state bleed-through,
  survive a deinit/reattach cycle, and the partition guard still
  rejects a second attach to either partition with `-EBUSY`.

### Changed

- Test build raises `CONFIG_UBI_MAX_NR_OF_DEVICES` to `2` for the
  `native_sim` secure config so the coexistence suite can hold two
  device handles concurrently. Production defaults are unaffected.

## [0.56.0] - 2026-05-06

### Added

- Replay-to-other-location regression tests (`ubi_secure_replay` suite):
  forging an authentic EC, VID, or LEB record into a different physical
  PEB is rejected by the parent-child AAD binding (`peb_index` in AAD).
  EC and VID replay marks the destination PEB as bad on reattach; LEB
  replay raises `AUTH_FAILURE` on the next read of the affected mapping.

## [0.55.0] - 2026-05-06

### Added

- Plain dispatcher test verifying `ubi_device_init()` returns `-ENOTSUP`
  when a non-NULL `crypto_cfg` is passed but `CONFIG_UBI_CRYPTO=n`.

### Changed

- `ubi_device_init()` now rejects, with `-EINVAL`, a `crypto_cfg` whose
  `allowed_key_versions` contains duplicate entries.
- Secure attach now rejects, with `-EINVAL`, a reattach whose
  `requested_write_key_version` is below the on-flash
  `write_active_key_version` (key versions must be monotonically
  non-decreasing; protects against accidental downgrade and uint8_t
  wrap-around).

## [0.54.0] - 2026-05-06

### Added

- Public API `ubi_secure_get_write_active_key_version()` that returns the
  authenticated write-active key version of a secure UBI device.  Returns
  `-EINVAL` on NULL arguments and `-ENOTSUP` on a plain-mode device.
- Init-time reserved-generation fit guard: secure init rejects
  geometries where one reserved generation
  (`UBI_SECURE_DEV_HDR_SIZE + CONFIG_UBI_MAX_NR_OF_VOLUMES * UBI_SECURE_VOL_HDR_SIZE`)
  cannot fit inside one reserved PEB.
- Five ZTESTs in `ubi_secure_api` and `ubi_secure_forensic`:
  `test_get_write_active_kv_null_args`,
  `test_get_write_active_kv_plain_mode`,
  `test_get_write_active_kv_after_format_and_rotation`,
  `test_reserved_generation_fit_guard_rejects_small_eb`,
  `test_leb_tail_padding_uses_erased_value`.

### Changed

- LEB write tail padding now uses the flash erased value (typically `0xFF`)
  instead of `0x00`, so the unused tail of the final write block matches
  what an erase would leave behind.  Applies to both single-tag and
  chunked write paths.
- `secure_architecture.md` §7.10 expanded with an implementation note
  stating that the secure read path deliberately does not re-verify
  inner CRC fields after a successful AEAD verification (the CCM tag
  already covers full record integrity).

## [0.53.0] - 2026-05-06

### Changed

- Eager reserved-PEB upgrade on key-version rotation now resets the
  on-flash and in-RAM VID-domain counter floor to 0.  Reattaching
  with the same key version still preserves the monotonic floor.
- Reserved-PEB key-version refcount now also counts secure volume
  headers (one per volume on every reserved PEB), and reserved
  metadata commits release the old (kv, vol_count) contribution only
  after the new one has been added.  Documentation in
  `secure_runtime_policy.md` updated to match `secure_architecture.md`.

### Added

- Four ZTESTs in `ubi_secure_runtime_policy`:
  `test_vid_floor_resets_on_rotation`,
  `test_vid_floor_persists_within_same_kv`,
  `test_vid_floor_reset_writes_use_low_counters`,
  `test_reserved_refcount_no_spurious_key_retirable`.

## [0.52.0] - 2026-05-05

### Changed

- Secure read paths reject records with an unknown `prefix32.wrapper_version`
  (`-EBADMSG`) right after the magic check, before any key derivation.
  Applied at all six deserialize sites: EC, VID, LEB single-tag, LEB
  chunked, reserved-PEB device header, reserved-PEB volume header.
- Single-tag LEB IO path enforces the AES-128-CCM payload limit at
  runtime: with q = 2 the CCM length field is 2 bytes, so any payload
  of 65536 bytes or more is unrepresentable.
  `ubi_secure_leb_data_write` rejects `len > 65535` with `-EFBIG`;
  `ubi_secure_leb_data_read` rejects `vid_hdr->data_size > 65535` with
  `-EBADMSG`; both checks fire before any key derivation or flash IO.
  In non-chunked builds, device init rejects geometries whose
  `leb_size > 65535` with `-EINVAL`.

### Added

- New constant `UBI_SECURE_LEB_SINGLE_TAG_MAX_PAYLOAD` (= 65535).
- Seven defensive ZTESTs in `ubi_secure_defensive`:
  `test_init_dev_hdr_bad_wrapper_version`,
  `test_init_vol_hdr_bad_wrapper_version`,
  `test_io_ec_hdr_read_bad_wrapper_version`,
  `test_io_vid_hdr_read_bad_wrapper_version`,
  `test_io_leb_data_read_bad_wrapper_version`,
  `test_io_leb_data_write_payload_exceeds_ccm_limit`,
  `test_io_leb_data_read_data_size_exceeds_ccm_limit`.

## [0.51.0] - 2026-05-05

### Added

- Secure backend now enforces a per-domain write-budget for all four
  metadata domains (device header, volume header, erase counter, volume
  identifier) under the active write key version, alongside the existing
  per-LEB data budget.  Reaching `CONFIG_UBI_CRYPTO_ROTATE_SOON_PCT` of
  any budget emits `KEY_ROTATE_SOON` while the operation still completes;
  reaching `CONFIG_UBI_CRYPTO_ROTATE_NOW_PCT` emits `KEY_ROTATE_NOW`,
  rejects the operation with `-ENOSPC` and transitions the device to
  read-only.  Subsequent writes, erases and volume mutations return
  `-EROFS` until the device is reattached with a new
  `requested_write_key_version`; reads remain available throughout.
- New `ubi_secure_budget_pre()` / `ubi_secure_budget_post()` API takes a
  `ubi_secure_domain` so callers no longer have to pick between
  metadata-only and LEB-only variants.

## [0.50.0] - 2026-05-04

### Changed

- Plain backend: public API functions now return `-EINVAL` with a log message on NULL arguments instead of triggering a kernel assert.
- Plain backend: all error paths now emit `LOG_ERR` for diagnostics.
- Plain backend: `reclaim_peb_to_dirty()` no longer returns an error code (always succeeded).
- Plain backend: `leb_write()` no longer acquires the device mutex — the public `ubi_leb_write()` wrapper handles locking.
- Plain backend: reserved-PEB overwrite fixed a dual-bank safety issue where a replacement PEB could be re-erased within the same commit.

## [0.49.0] - 2026-05-04

### Changed

- Renamed `struct ubi_mtd` → `struct ubi_flash_desc` and `.mtd` → `.flash` field across the codebase.
- Reordered `struct ubi_device` fields for better logical grouping.
- Consolidated `CONFIG_UBI_CRYPTO_MAX_ALLOWLIST_LEN` into `CONFIG_UBI_CRYPTO_MAX_KEY_VERSIONS`.
- Renamed `CONFIG_UBI_MEM_STATS` → `CONFIG_UBI_TEST_MEM_STATS`.
- Added `depends on MBEDTLS_ENTROPY_POLL_ZEPHYR` to `UBI_CRYPTO`.
- Removed dead code, improved doxygen, and standardized comment style across all files.

## [0.48.0] - 2026-04-17

### Changed

- Secure crypto: central allowlist check in `derive_domain_key()` and `derive_leb_key()` — all domains and both read/write paths validated before key derivation.
- Secure backend: monotonic AEAD counters for EC and device-header domains — recovered from flash during init, incremented per write.
- Secure backend: VID counter overflow check (`COUNTER_MAX`) with `KEY_ROTATE_NOW` event.
- Secure backend: overflow guards in `ec_hdr_write`, `vid_hdr_write`, and `res_peb_commit`.
- Secure backend: eager reserved-PEB key upgrade during attach — when `requested_write_key_version` differs from flash, `res_peb_commit` rewrites device and volume headers under the new key immediately, with graceful fallback if the key is not yet provisioned.
- Secure backend: reserved-PEB refcount tracking via `reserved_key_version` field — `KEY_RETIRABLE` blocked while reserved PEBs still depend on old key version.

### Added

- Test: VID counter floor survives `remove→create→reboot` sequence (2 cycles).
- Test: refcount E2E lifecycle with key rotation (`kv=1→kv=2`), volume ops, unmap, resize, erase cycles, and `KEY_RETIRABLE` verification.
- Tests: counter overflow boundary tests for EC, VID, and reserved-PEB AEAD counters.
- Tests: allowlist rejection tests for `derive_domain_key` and `derive_leb_key`.

## [0.47.0] - 2026-04-17

### Changed

- Plain backend: renamed 7 `.c` and 2 `.h` files to `ubi_plain_*` prefix; updated all includes and CMake.
- Secure backend: replaced `ubi_secure_zeroize` volatile-loop with `mbedtls_platform_zeroize`.
- Secure backend: added `LOG_ERR` to silent error returns in volume, LEB, runtime, and crypto modules.
- Secure backend: removed `§` references from comments; fixed `ubi_secure_anchor_create` section label.
- Tests: normalized section divider comments to 100-char centered format.

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
