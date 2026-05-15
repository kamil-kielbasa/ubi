# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.0.0] - 2026-05-15

First stable release. The pre-1.0 history below (0.1.0 – 0.113.0) is the
incremental record; this entry summarises what v1.0.0 delivers as a
whole and what stability commitments come with it.

### Stability commitment

- Public API in `lib/include/ubi.h` and `lib/include/ubi_secure.h` is
  stable. Breaking changes require a major version bump.
- The on-flash format (plain and secure) is frozen. Future v1.x
  releases will read v1.0.0 partitions.
- The Kconfig surface (`CONFIG_UBI_*`) follows the same compatibility
  guarantee.

### Plain backend (always built)

- **Device lifecycle.** Unified `ubi_device_init(flash, secure_cfg, &ubi)`
  / `ubi_device_deinit()` with backend dispatch via an internal vtable.
  Per-device static memory backend (`k_mem_slab`) is the default; legacy
  heap backend is selectable via Kconfig.
- **Volumes.** Static and dynamic types, runtime `ubi_volume_create` /
  `_remove` / `_resize` with grow + shrink, transactional rollback on
  failure, persistent `vol_id` high-watermark (IDs are never reused
  across the device lifetime).
- **LEB I/O.** `ubi_leb_map` / `_unmap` / `_read` / `_write` over the
  Zephyr Flash Map API, with copy-on-write semantics (old mapping
  preserved until the new PEB is fully written) and write-block-aligned
  tail padding using the hardware-reported erase value.
- **Wear-leveling.** Global wear budget across all PEBs of a partition;
  monotonic erase counters with average-tracking; configurable
  per-write retry count; bad-block torture before isolation.
- **Crash safety.** Reserved-PEB dual bank with monotonic
  `vid_sqnum`-based recovery; degraded mode that exposes a sticky
  read-only flag (`read_only_degraded`) when one bank fails.
- **Mutation gate.** A per-device read-only flag is checked before every
  public mutator; data path, reserved-metadata path, and maintenance
  path are gated independently.
- **Concurrency.** Per-device mutex; multiple `ubi_device` handles per
  application supported, each on its own partition; single-handle-per-
  partition guard rejects double-attach.
- **Misuse handling.** All public entry points return `-EINVAL` with a
  diagnostic log on `NULL` arguments instead of asserting; integration
  bugs are non-fatal in production builds.

### Secure backend (`CONFIG_UBI_SECURE=y`, opt-in)

- **AEAD coverage.** AES-128-CCM via PSA Crypto over **every**
  commit-visible on-flash structure: device header, volume headers, EC
  headers, VID headers, reserved PEBs, and LEB payloads. Block location
  and identity are bound into the AAD, so relocating an authentic
  record to a different PEB or replaying it under a different
  `volume_id` / `lnum` fails MAC verification.
- **Key hierarchy.** HKDF-SHA-256 key derivation, parent-child binding,
  per-domain child keys, salt generation. Application supplies key
  material through the `get_key_id` callback returning `psa_key_id_t`.
- **Versioned keys & allowlist.** Per-PEB key-version refcount; the
  active write-key version is authenticated on flash; allowlist is
  enforced on both read and write before any key derivation. Key-life-
  cycle events `KEY_ROTATE_SOON`, `KEY_ROTATE_NOW`, `KEY_RETIRABLE` are
  delivered to the application; `KEY_RETIRABLE` fires only after the
  last carrier of an old version is recycled.
- **Anti-rollback.** Application-supplied `check_freshness` /
  `sync_freshness` callbacks bound to the device-header revision and
  the VID-header global sequence number — attach-time check plus
  post-commit sync. Optional `CONFIG_UBI_SECURE_SYNC_FRESHNESS_VERIFY`
  re-reads each successful sync to catch ack-but-not-persisted
  integrations.
- **Counter continuity.** Monotonic AEAD counters per LEB, VID, EC, and
  device-header domain, recovered from flash at attach via a per-volume
  hidden anchor PEB and a last-writable-witness rule on erase. 48-bit
  counter saturation fails closed before any flash mutation.
- **Per-domain write budgets.** Soft/hard thresholds for each metadata
  and data domain under the active write key version; reaching the soft
  threshold emits `KEY_ROTATE_SOON`, hard threshold emits
  `KEY_ROTATE_NOW`, rejects the operation with `-ENOSPC`, and
  transitions the device to read-only. Reads remain available.
- **Sticky read-only mode.** AEAD failure, RNG failure, or write-budget
  exhaustion escalates the device to `-EROFS`-on-write while reads
  continue. Reset clears the flag.
- **Chunked LEB mode.** Optional independently-authenticated 256 B –
  64 KiB chunks for large LEBs; only touched chunks are verified on
  partial read.
- **Coexistence.** Plain and secure devices run side by side on
  different partitions; plain dispatcher returns `-ENOTSUP` if a
  non-`NULL` `secure_cfg` is passed without `CONFIG_UBI_SECURE`.

### Validated targets

- Zephyr `native_sim` (flash simulator) with both 4 KB and 8 KB erase-
  block geometries, plain + secure + chunked configurations.
- STM32U585 (`b_u585i_iot02a`) — 128 KiB UBI partition, plain + secure
  cross-build verified on every PR.
- nRF5340 DK (`nrf5340dk/nrf5340/cpuapp`) — 64 KiB UBI partition,
  plain + secure cross-build verified on every PR.

### Quality bar

- **55 ZTEST suites, 609 tests** (270 plain + 339 secure) covering API
  contracts, recovery, fault injection (allocation, flash I/O, crypto,
  RNG, freshness), concurrency, stress, replay-to-other-PEB resistance,
  forensic scanning, and budget exhaustion.
- **Live coverage** reported by Codecov on every push to `main`.
- **Forensic scanner** (`scripts/scan_flash.py`) verifies that no
  plaintext data, volume names, or key material appear on flash after
  secure writes; runs in CI.
- **Build hygiene.** `format-check`, `forensic-scan`, and per-target
  cross-build jobs gate every PR; warnings are errors
  (`-Werror -Wextra -Wshadow`); test docblocks (`\brief`, `\details`,
  `\expected`, optional `\oracle` / `\trace` / `\precondition`) are
  checked in `--strict` mode.

### Resource profile

- Plain build: ~9.5 KB flash, ~1.5 KB BSS.
- Secure build: ~28.6 KB flash, ~1.8 KB BSS.
- Cortex-M33, `-Os`, STM32U585; UBI library archive only — PSA Crypto
  and mbedTLS are provided by the platform and not counted.

### Documentation

- Sphinx site under the **Furo** theme, organised in five Diátaxis-
  aligned sections (Getting Started, User Guide, Architecture,
  Reference, Project), deployed to <https://kamil-kielbasa.github.io/ubi/>.
- Topic pages: *What is UBI?*, *Comparison vs LittleFS / NVS / ZMS*,
  *Concepts at a Glance*, *Quick Start*, *Configuration*, *Cookbook*
  (six end-to-end recipes including STM32U5 / nRF5340 bring-up, A/B
  firmware, periodic GC, key rotation, freshness store), *Plain UBI
  Workflow*, *Secure UBI Workflow*, *Plain Architecture*, *Secure
  Architecture*, *Secure On-Flash Format Specification*, *API
  Reference*, *Kconfig Reference*, *Error Codes*, *Glossary*, *Test
  Strategy*, *Contributing*.
- ZTEST traceability tables in *Test Strategy* link normative
  behaviours to their regression tests.

### Community files

- `SECURITY.md` (Private Vulnerability Reporting + email + 7-day
  acknowledgement SLA).
- `CODE_OF_CONDUCT.md` (Contributor Covenant 2.1).
- `CITATION.cff` (CFF 1.2.0).
- `.github/ISSUE_TEMPLATE/` — bug, feature, and question forms with a
  security redirect to SECURITY.md.
- `.github/pull_request_template.md` mirroring the contributing-guide
  PR checklist.

## [0.113.0] - 2026-05-14

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.112.0] - 2026-05-14

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.111.0] - 2026-05-13

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.110.0] - 2026-05-13

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.109.0] - 2026-05-13

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.108.0] - 2026-05-13

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.107.0] - 2026-05-13

### Added

- Per-allocator-kind fault-injection selector in the test API
  (`enum ubi_test_alloc_kind`,
  `ubi_test_fault_set_kind_alloc_fail_after()`), letting integrators
  target a specific allocation site without disturbing others. No-op
  in non-test builds.

## [0.106.0] - 2026-05-13

### Added

- Test docblock template recognises optional `\oracle`, `\trace`, and
  `\precondition` tags; `--strict` mode promotes them to errors.

## [0.105.0] - 2026-05-13

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.104.0] - 2026-05-13

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.103.0] - 2026-05-13

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.102.0] - 2026-05-13

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.101.0] - 2026-05-13

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.100.0] - 2026-05-13

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.99.0] - 2026-05-13

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.98.0] - 2026-05-12

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.97.0] - 2026-05-12

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.96.0] - 2026-05-12

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.95.0] - 2026-05-12

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.94.0] - 2026-05-12

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.93.0] - 2026-05-12

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.92.0] - 2026-05-12

### Changed

- Internal test cleanup; no API or behaviour changes.

## [0.91.0] - 2026-05-12

### Added

- Two test-only secure hooks (`ubi_secure_test_get_peb_for_lnum`,
  `ubi_secure_test_read_vid_meta_from_peb`), gated by
  `CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION`.

## [0.90.0] - 2026-05-12

### Added

- New Kconfig `CONFIG_UBI_CRYPTO_SYNC_FRESHNESS_VERIFY` (default `n`):
  when enabled, every successful `sync_freshness` callback is
  immediately re-checked via `check_freshness`; a mismatch raises
  `ROLLBACK_POLICY_MISMATCH`. Catches integrations that ack the sync
  but fail to durably persist it.

### Changed

- Internal field grouping in `struct ubi_device` (e.g.
  `ubi->next_vid_counter` → `ubi->aead.next_vid`). Public
  `struct ubi_device_info` is unchanged. No on-flash format change.

## [0.89.0] - 2026-05-12

### Fixed

- Stack byte buffers used to build AEAD nonces are now always
  zero-initialised, eliminating a path that could carry stack garbage
  into a chunked-LEB nonce.

## [0.88.0] - 2026-05-12

### Changed

- AEAD entry points now reject the under-specified case
  `aad == NULL && aad_len != 0` with `-EINVAL` at the boundary.
- `ubi_secure_destroy_key()` now warns when PSA reports an unexpected
  error so a key-slot leak is no longer silent on cleanup paths.

## [0.87.0] - 2026-05-12

### Changed

- Public ABI rename: `ubi_secure_get_write_active_key_version` →
  `ubi_secure_key_get_active_version` in `ubi_crypto.h`. Call
  semantics unchanged.

## [0.86.0] - 2026-05-12

### Changed

- Erase-time maintenance failures on the per-volume anchor reserve are
  now reported via `LOG_WRN` (with PEB index and return code) instead
  of being silently swallowed.

## [0.85.0] - 2026-05-12

### Changed

- Internal refactor of secure AAD builders and LEB I/O signatures.
  No API, behaviour, or on-flash format change.

## [0.84.0] - 2026-05-12

### Changed

- Internal refactor of the secure backend; no behaviour, API, or
  on-flash format change.

## [0.83.0] - 2026-05-12

### Changed

- On-flash record sizes are now compile-time bound to their C structs.
  A future change that would shift any committed record's byte count
  fails the build.

## [0.82.0] - 2026-05-12

### Changed

- **BREAKING:** the secure backend now uses `psa_key_id_t` for key
  identifiers throughout its public API. Applications that provide the
  `get_key_id` callback (`ubi_crypto_get_key_id_cb_t`) must update its
  second parameter from `uint32_t *` to `psa_key_id_t *`. No on-flash
  format change; secure devices attach and continue to operate exactly
  as before.

## [0.81.0] - 2026-05-12

### Security

- **Fix AEAD nonce-uniqueness regression in the secure backend.** The
  previous counter-recovery helper only consulted the PEB currently
  mapped to the requested `lnum`. After
  `write → unmap → erase-all-dirty → write` the new mapping restarted
  `leb_write_counter` at 0 under the same HKDF child key, violating
  AEAD nonce uniqueness for the affected `{key_version, volume_id}`
  pair. **Versions 0.79.0 and 0.80.0 are vulnerable; do not deploy
  them.**

### Changed

- Per-volume AEAD counter floor is now cached in RAM, reseeded from
  the anchor plus every authenticated data PEB during attach, bumped
  before each `leb_data_write`, and refreshed on anchor rewrite.
- Anchor witness-check on dirty erase is now O(1) instead of O(N²).

### Added

- New `ubi_secure_anchor` test suite covering counter inheritance,
  cold-attach reseed, 48-bit counter saturation, and strict
  monotonicity loops.

## [0.80.0] - 2026-05-11

### Changed

- Plain UBI flow diagrams in *Plain UBI Architecture* are now a single
  side-by-side SVG instead of three stacked Mermaid blocks.

## [0.79.0] - 2026-05-11

### Added

- New documentation page **Comparison: UBI vs LittleFS vs NVS vs ZMS**
  with a side-by-side table and a two-question decision shortcut.

### Changed

- Removed redundant per-page `{contents}` blocks from Cookbook, Error
  Codes, and Kconfig Reference (Furo renders a per-page outline).

### Fixed

- Footer GitHub icon now renders as inline SVG so it appears on nested
  doc URLs.

## [0.78.0] - 2026-05-11

### Changed

- Documentation site now uses the **Furo** theme: light/dark toggle,
  copy-button on every code block, "Edit on GitHub" links. No content
  or URLs change.

## [0.77.0] - 2026-05-11

### Changed

- Plain UBI Architecture prose now uses the canonical field names
  `volume_id` and `vid_sqnum` (matching the C headers and the
  Glossary).

### Fixed

- Removed a stale placeholder in the Secure Architecture Overview
  "What's next" list.

### Added

- Stylesheet (`_static/custom.css`) caps Mermaid diagrams at 720 px
  and centres them.

## [0.76.0] - 2026-05-08

### Changed

- The Secure On-Flash Format Specification illustrations are now
  static SVGs (key hierarchy + attach/runtime sequences) instead of
  Mermaid; they render identically on GitHub, GitHub Pages, and
  offline / PDF.

## [0.75.0] - 2026-05-08

### Added

- README **Acknowledgments** section credits the Linux UBI subsystem
  as the direct inspiration and frames Secure UBI as the author's
  addition on top of that foundation.
- Social preview banner ships in `doc/img/`.

### Changed

- README release-status note promoted to a top-of-file blockquote;
  Documentation section now links to the new Glossary.

## [0.74.0] - 2026-05-08

### Added

- Single-page **Glossary** under `reference/glossary` collects every
  UBI-specific term used across the documentation.

### Changed

- Plain UBI Architecture now uses the precise field names `vid_sqnum`
  and `volume_id`.

## [0.73.0] - 2026-05-08

### Changed

- The Secure On-Flash Format Specification is now a tighter normative
  reference. Architectural rationale moved to the Secure Architecture
  Overview and the Secure UBI Workflow guide.

## [0.72.0] - 2026-05-08

### Added

- New **Plain UBI Workflow** guide — when to use plain UBI, the
  `ubi_device` lifecycle, volume management, the LEB read/write
  contract, GC, error handling, degraded read-only mode.
- New **Error Codes** reference page listing every value the public
  API can return with the recommended application reaction.

### Fixed

- Quick Start example: read length now uses `sizeof(buf)` instead of
  `sizeof(data)` (no more silent truncation).
- Cookbook *Periodic garbage collection* clarifies that the
  application's main TU must contain a matching
  `LOG_MODULE_REGISTER(app)`.

## [0.71.0] - 2026-05-08

### Documentation

- Documentation reorganised into five topic folders
  (`getting_started/`, `guide/`, `architecture/`, `reference/`,
  `project/`). Old URLs continue to work via redirects.
- New **Kconfig Reference** page lists every UBI Kconfig symbol with
  type, default, range, dependencies, and description.

### Changed

- Reported flash footprint of the UBI library is now ~9.5 KB plain
  and ~29 KB secure on Cortex-M33 (`-Os`), measured against the UBI
  archive only. PSA Crypto / mbedTLS is provided by the platform and
  not counted.

### Fixed

- `key_hierarchy.svg`: labels no longer overflow their boxes.
- `onflash_layout.svg`: added the missing reserved-PEB zoom.
- On-flash format spec chapter ordering corrected.

### Removed

- *Appendix B* and *Appendix C* of `onflash_format_spec.md` (covered
  by `project/roadmap.md` and the ZTEST traceability tables).

## [0.70.0] - 2026-05-07

### Changed

- The Cookbook chapter has graduated from a stub list to six runnable,
  copy-paste-ready end-to-end recipes: STM32U5 bring-up
  (`b_u585i_iot02a`, 128 KiB partition); nRF5340 bring-up
  (`nrf5340dk/nrf5340/cpuapp`, 64 KiB partition); A/B firmware slots
  with a flip volume; periodic GC via `k_work`; key rotation against
  PSA from version 1 to version 2 (including when it is safe to call
  `psa_destroy_key()`); freshness store on Zephyr Settings with both
  `check_freshness` and `sync_freshness` wired up.
- Three hero diagrams (`stack.svg`, `onflash_layout.svg`,
  `key_hierarchy.svg`) promoted from ASCII / Mermaid to hand-authored
  SVG.
- README slimmed to its v1.0.0 target shape.

## [0.69.0] - 2026-05-07

### Changed

- Secure UBI documentation split along audience boundary into three
  pages: a normative byte-level reference (`onflash_format_spec.md`),
  a developer-onboarding overview (`secure_overview.md`), and a
  user-guide workflow page (`secure_workflow.md`). The three small
  satellites (volume lifecycle, recovery notes, runtime policy) folded
  into the spec as chapters 19–21. ZTEST traceability tables moved to
  `test_strategy.md`. Old URLs continue to work via redirects.

## [0.68.0] - 2026-05-07

### Changed

- Plain Architecture documentation slimmed: removed duplicate ASCII
  rendering of the PEB lifecycle (Mermaid version retained) and moved
  the per-file source-code map to a contributor-only *Developer Notes*
  page. The legacy *Getting Started* page split into a hands-on
  *Quick Start* and a build/test/coverage section folded into *Test
  Strategy*. Old URLs forward to *Quick Start*.

## [0.67.0] - 2026-05-07

### Changed

- Legacy `Overview` and `Introduction` pages merged into a single
  canonical *What is UBI?* plus a *Concepts at a Glance* page. The
  resource-usage profile moved into *Plain Architecture* alongside
  the in-RAM data structures it describes. Old URLs forward to *What
  is UBI?*.

## [0.66.0] - 2026-05-07

### Changed

- Sphinx documentation sidebar reorganised into five Diátaxis-aligned
  sections (Getting Started, User Guide, Architecture, Reference,
  Project) with a four-card landing page and a v1.0.0 status banner.
  Legacy pages remain reachable while content migration completes;
  `sphinx-reredirects` keeps every old URL alive.

## [0.65.0] - 2026-05-07

### Changed

- Internal test cleanup (consistent section banners, lifecycle hooks).
  Library, public API, and on-flash format unaffected.

## [0.64.0] - 2026-05-07

### Changed

- Secure-backend internal vtable entry points now return `-EINVAL`
  with a diagnostic log on `NULL` arguments instead of tripping an
  assertion. Production builds without asserts no longer experience
  undefined behaviour on misuse.
- Secure onboarding sample now uses Zephyr logging (`LOG_INF` /
  `LOG_ERR`) instead of `printk` so output can be filtered/silenced.

## [0.63.0] - 2026-05-07

### Changed

- Misuse of the public API now logs which argument was invalid
  (including pointer values for multi-argument checks). Return codes
  are unchanged.
- Plain sample now uses Zephyr logging instead of `printk`.

## [0.62.0] - 2026-05-07

### Added

- Cross-reference tables in the secure documentation linking each
  documented behaviour to the regression test that locks it
  (recovery scenarios → ZTEST, lifecycle steps → ZTEST, release
  checklist → ZTEST).

## [0.61.0] - 2026-05-07

### Added

- Ready-to-run onboarding **sample for the secure backend**. Build
  with
  `west build -p -b <board> sample/ -- -DOVERLAY_CONFIG=boards/secure.conf`.
  CMake automatically selects the secure entry point when
  `CONFIG_UBI_CRYPTO=y`.
- The secure sample is now compile-verified on every PR for the
  supported Cortex-M targets.

### Fixed

- The library no longer fails to build for downstream consumers that
  link it without `CONFIG_UBI_TEST_API_ENABLE=y` (including the new
  sample).

## [0.60.0] - 2026-05-07

### Added

- End-to-end forced-rekey regression test covering eager reserved
  metadata upgrade, payload survival across rotation, and
  `KEY_RETIRABLE` firing exactly when the last stale object is
  recycled.

## [0.59.0] - 2026-05-07

### Added

- Chunked-write 48-bit AEAD counter overflow is proven to fail closed
  before any flash mutation: returns `-EOVERFLOW`, emits exactly one
  `KEY_ROTATE_NOW`, consumes no PEB.

### Changed

- New test-only API
  `ubi_secure_test_set_leb_write_counter_floor()` /
  `_get_leb_write_counter_floor()` (gated by
  `CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION`).

## [0.58.0] - 2026-05-06

### Fixed

- Restored the `native_sim` chunked test build (config drift had
  silently disabled the chunked configuration).
- Test runner no longer cascades failures: a single test aborting
  before teardown previously poisoned the rest of the run with
  misleading allocation failures.
- Chunked tamper test no longer silently no-ops on roughly half of
  runs; deterministic destructive write with read-back guard.
- Coverage-secure CI now matches the runtime contract assumed by the
  coexistence test (two device handles).
- Host-side flash forensic scanner (`scripts/scan_flash.py`) no
  longer flags ciphertext as a leak.

## [0.57.0] - 2026-05-06

### Added

- Plain + secure backend coexistence regression test: a plain device
  on `ubi_partition` and a secure device on `ubi_partition_2` operate
  in parallel without state bleed-through and survive a deinit/reattach
  cycle.

### Changed

- Test build raises `CONFIG_UBI_MAX_NR_OF_DEVICES` to `2` for the
  secure native_sim config. Production defaults unaffected.

## [0.56.0] - 2026-05-06

### Added

- Replay-to-other-location regression tests: forging an authentic EC,
  VID, or LEB record into a different physical PEB is rejected by the
  parent-child AAD binding (`peb_index` in AAD).

## [0.55.0] - 2026-05-06

### Added

- Plain dispatcher test verifying `ubi_device_init()` returns
  `-ENOTSUP` when a non-NULL `crypto_cfg` is passed but
  `CONFIG_UBI_CRYPTO=n`.

### Changed

- `ubi_device_init()` now rejects, with `-EINVAL`, a `crypto_cfg`
  whose `allowed_key_versions` contains duplicate entries.
- Secure attach now rejects, with `-EINVAL`, a reattach whose
  `requested_write_key_version` is below the on-flash
  `write_active_key_version` (protects against accidental downgrade
  and `uint8_t` wrap-around).

## [0.54.0] - 2026-05-06

### Added

- Public API `ubi_secure_get_write_active_key_version()` returns the
  authenticated write-active key version of a secure UBI device.
- Init-time guard: secure init rejects geometries where one reserved
  generation cannot fit inside one reserved PEB.

### Changed

- LEB write tail padding now uses the flash erased value (typically
  `0xFF`) instead of `0x00`, matching what an erase would leave.
- Documentation note: the secure read path deliberately does not
  re-verify inner CRC fields after a successful AEAD verification
  (the CCM tag already covers full record integrity).

## [0.53.0] - 2026-05-06

### Changed

- Eager reserved-PEB upgrade on key-version rotation now resets the
  on-flash and in-RAM VID-domain counter floor to 0. Reattaching with
  the same key version still preserves the monotonic floor.
- Reserved-PEB key-version refcount now counts secure volume headers
  (one per volume on every reserved PEB); the old `(kv, vol_count)`
  contribution is released only after the new one has been added.

## [0.52.0] - 2026-05-05

### Changed

- Secure read paths reject records with an unknown
  `prefix32.wrapper_version` (`-EBADMSG`), before any key derivation.
- Single-tag LEB I/O enforces the AES-128-CCM payload limit:
  `ubi_secure_leb_data_write` rejects `len > 65535` with `-EFBIG`;
  the read path rejects `data_size > 65535` with `-EBADMSG`. In
  non-chunked builds, device init rejects geometries whose
  `leb_size > 65535` with `-EINVAL`.

### Added

- New constant `UBI_SECURE_LEB_SINGLE_TAG_MAX_PAYLOAD` (= 65535).

## [0.51.0] - 2026-05-05

### Added

- Per-domain write-budget enforcement for all four metadata domains
  (device header, volume header, EC, VID) under the active write
  key version, alongside the existing per-LEB data budget. Reaching
  `CONFIG_UBI_CRYPTO_ROTATE_SOON_PCT` of any budget emits
  `KEY_ROTATE_SOON`; reaching `CONFIG_UBI_CRYPTO_ROTATE_NOW_PCT`
  emits `KEY_ROTATE_NOW`, rejects the operation with `-ENOSPC`, and
  transitions the device to read-only. Reads remain available
  throughout.

## [0.50.0] - 2026-05-04

### Changed

- Plain backend: public API functions now return `-EINVAL` with a log
  message on NULL arguments instead of triggering a kernel assert.
- Plain backend: error paths now emit `LOG_ERR` for diagnostics.
- Plain backend: reserved-PEB overwrite fixed a dual-bank safety
  issue where a replacement PEB could be re-erased within the same
  commit.

## [0.49.0] - 2026-05-04

### Changed

- **API rename:** `struct ubi_mtd` → `struct ubi_flash_desc`,
  `.mtd` → `.flash` field across the codebase.
- **Kconfig rename:** `CONFIG_UBI_CRYPTO_MAX_ALLOWLIST_LEN`
  consolidated into `CONFIG_UBI_CRYPTO_MAX_KEY_VERSIONS`;
  `CONFIG_UBI_MEM_STATS` → `CONFIG_UBI_TEST_MEM_STATS`.
- `UBI_CRYPTO` now `depends on MBEDTLS_ENTROPY_POLL_ZEPHYR`.

## [0.48.0] - 2026-04-17

### Added

- Monotonic AEAD counters for EC and device-header domains (recovered
  from flash during init, incremented per write).
- VID counter overflow check (`COUNTER_MAX`) emits `KEY_ROTATE_NOW`.
- Eager reserved-PEB key upgrade during attach: when
  `requested_write_key_version` differs from flash, reserved metadata
  is rewritten under the new key immediately (graceful fallback if
  the key is not yet provisioned).
- Reserved-PEB refcount tracking: `KEY_RETIRABLE` is withheld while
  reserved PEBs still depend on the old key version.

### Changed

- Secure crypto: central allowlist check now in `derive_domain_key()`
  and `derive_leb_key()` — all domains and both read/write paths
  validated before key derivation.

## [0.47.0] - 2026-04-17

### Changed

- Internal cleanup (file renames, sensitive-buffer zeroization via
  `mbedtls_platform_zeroize`); no API or behaviour changes.

## [0.46.0] - 2026-04-17

### Added

- Functional parity test coverage for the secure backend (device
  init/deinit, volume CRUD, LEB I/O, resize, edge cases) mirrored
  from the plain backend, plus secure fault-injection,
  mutation-gate, and vol-id-watermark suites.

## [0.45.0] - 2026-04-16

### Changed

- Documentation aligned with secure backend implementation:
  refreshed flash footprint, struct sizes, test counts, coverage
  numbers, and Kconfig reference across all docs.

### Fixed

- CI: clang-format version-dependent goto label formatting handled
  with off/on guards.
- CI cross-build nRF5340 secure: added test random generator for
  builds without BLE IPC.
- CI cross-build STM32U5 secure: `ubi_device` size BUILD_ASSERT
  guarded for ARM vs POSIX `k_mutex` layout difference.

## [0.44.0] - 2026-04-16

### Added

- Forensic scan test suite: verifies no plaintext data, volume names,
  or key material appear on flash after secure writes.
- Host-side Python forensic scanner (`scripts/scan_flash.py`) and
  test docblock checker scripts.
- CI jobs: `format-check`, `forensic-scan`, cross-build secure for
  STM32U5 and nRF5340.
- Init-time anchor re-creation: orphaned volumes automatically get
  a new hidden anchor at attach.

### Changed

- Test and coverage scripts accept a mode parameter
  (`plain` / `secure` / `chunked`).

## [0.43.0] - 2026-04-16

### Added

- **Chunked secure LEB mode:** LEB records split into independently
  authenticated chunks for partial-read support. Configurable chunk
  size (256–65536 B). Only touched chunks are verified, reducing
  read latency and RAM.
- Chunked geometry validation at init and budget accounting for
  chunked writes.
- Secure recovery test suite: interrupted writes, anchor rewrites,
  reserved-PEB commit faults, generation replay rejection.
- Flash write fault injection wired into the secure backend.

## [0.42.0] - 2026-04-15

### Added

- **Sticky crypto read-only mode:** the event callback can escalate
  to a device-wide write block (`-EROFS`); reads remain functional.
- **Security event infrastructure:** 10 event types
  (`AUTH_FAILURE`, `FORMAT_VIOLATION`, `KEY_ROTATE_SOON`/`NOW`,
  `KEY_RETIRABLE`, `RNG_FAILURE`, etc.) delivered through the
  application callback.
- Freshness sync after every commit-visible mutation with
  configurable cadence.
- Key-version PEB refcount tracking with `KEY_RETIRABLE` signalling
  when a key version is fully erased.
- LEB usage budget tracking with soft/hard thresholds and pre-write
  rejection.
- Read-path key-version allowlist enforcement.
- Sensitive-buffer zeroization (compiler-safe volatile memset).

## [0.41.0] - 2026-04-15

### Added

- **Hidden per-volume anchor PEBs:** each secure volume reserves one
  internal PEB preserving monotonic counter state across unmap,
  shrink, and erase.
- **VID-domain counter floor:** monotonic VID counter persisted in
  the secure device header, reconstructed at attach.
- **Last-writable-witness check:** the erase path rewrites the anchor
  before erasing the last carrier of a volume's counter floor.
- Emergency free-PEB reserve: write path reclaims a dirty PEB before
  consuming the last free PEB.

### Changed

- Volume create rolls back on anchor allocation failure.
- New docs: secure volume lifecycle and secure recovery scenarios.

## [0.40.0] - 2026-04-15

### Added

- **Complete secure data-PEB runtime:** volume create / resize /
  remove, LEB write / read / map / unmap, erase — all with
  authenticated encryption.
- Data-PEB scan pipeline: classifies PEBs into free/dirty/bad pools
  with full AEAD verification.
- Functional parity test coverage with the plain backend on
  encrypted flash.
- Flash geometry overlays for nRF5340 (4 KB erase) and STM32U5
  (8 KB erase) on `native_sim`; multi-geometry CI runs.

### Fixed

- LEB write/nonce counters now recovered from existing VID header
  (were hardcoded to 0).
- Write-block alignment padding for the secure ciphertext+tag buffer.
- Missing bad-block torture and degraded-mode recovery in the secure
  runtime.
- Geometry validation for erase/write block alignment.

## [0.39.0] - 2026-04-14

### Added

- **First secure backend release:** authenticated read/write for EC
  headers, VID headers, and LEB data in single-tag mode.
- Domain-separated AAD serialisation for all record types.

### Changed

- Key derivation centralised; parent authentication passed via typed
  context structs.

## [0.38.0] - 2026-04-15

### Added

- Secure reserved-PEB attach path: format-on-blank, attach-to-existing,
  mode-mismatch detection.
- PSA Crypto integration: HKDF-SHA-256 key derivation, AES-128-CCM
  AEAD, salt generation.
- Encrypted dual-bank reserved-PEB commit and authentication.

## [0.37.0] - 2026-04-14

### Added

- **Secure public API surface:** crypto config, event types (tagged
  union), freshness descriptor, policy struct, and callback typedefs.
- Secure Kconfig surface: master enable, budget limits, rotation
  thresholds, chunked mode, PEB cache, freshness sync, strict RO
  policies.
- Crypto fault-injection hooks (7 stages) for integration tests.
- Secure test profiles and board configs with mbedTLS PSA.

## [0.36.0] - 2026-04-14

### Changed

- **Backend dispatch:** every public API function now routes through
  an 11-op vtable. Plain backend functions renamed to `ubi_plain_*`.
  Internal headers decoupled from plain-specific includes, making
  them backend-agnostic.

## [0.35.0] - 2026-04-14

### Changed

- **Unified init API:** `ubi_device_init(flash, crypto_cfg, &ubi)`.
  Passing `NULL` selects plain; non-NULL selects secure. Runtime
  backend dispatch via the ops vtable.

## [0.34.0] - 2026-04-13

### Changed

- Secure architecture spec: per-device runtime mode selection (not
  per-build), tagged-union events, `check_freshness` confirmed as
  attach-time only.
- Repository layout split into `common/`, `plain/`, `secure/`
  namespaces for library and test sources.
- `format.sh` is now recursive with `--check` mode for CI.

## [0.33.0] - 2026-04-11

### Changed

- `design_proposal_crypto.md` promoted to `secure_architecture.md` as
  a first-class architecture document.
- `architecture.md` renamed to `plain_architecture.md`.

## [0.32.0] - 2026-04-11

### Changed

- Secure architecture spec rewrite (v6): hidden per-volume anchor
  PEBs, secure device header with crypto metadata, full counter
  continuity framework, renamed child keys to domain names,
  simplified chunked mode, expanded write-budget enforcement, precise
  AAD byte layouts, tagged-union events with verdicts.

## [0.31.0] - 2026-04-10

### Changed

- Secure architecture spec rewrite (v5): mode detection rules,
  normative KDF labels, single-tag CCM payload limit, zero-length LEB
  encoding, tail-padding rules, parent EC `key_version` in VID and
  LEB AAD.

## [0.30.0] - 2026-04-09

### Added

- "Why UBI for Zephyr" positioning document: gap analysis, comparison
  with FCB / NVS / ZMS / LittleFS.

### Changed

- Secure architecture spec rewrite (v4).

## [0.29.0] - 2026-04-09

### Added

- **Persistent `vol_id` high-watermark:** volume IDs are never reused
  across the device lifetime. Overflow returns `-ENOSPC`.

### Changed

- Volume matching by `vol_id` instead of positional index; re-index
  loop eliminated.

## [0.28.0] - 2026-04-09

### Added

- **Central mutation gate:** per-device read-only flag checked before
  every public mutator. Three mutation classes (reserved metadata,
  data path, maintenance).
- Runtime degradation detection and self-healing via reserved-PEB
  bank recovery.
- Test-only write-shutdown API.

## [0.27.0] - 2026-04-10

### Changed

- **Data-PEB commit order changed to EC → DATA → VID.** The VID
  header is now the sole commit-visible record.
- Init scan distinguishes free PEBs from uncommitted writes by
  probing the data area.

### Fixed

- Uncommitted writes were misclassified as free under the new write
  order.

## [0.26.0] - 2026-04-09

### Changed

- Erased-state detection now uses the hardware-reported erase value
  instead of a hard-coded `0xFF`.

## [0.25.0] - 2026-04-08

### Changed

- Secure architecture spec rewrite (v3): init classification,
  write/read/erase paths, key lifecycle with refcount retirement,
  events and policy, cost model, illustrative API (Appendix A).

## [0.24.0] - 2026-04-03

### Added

- Secure architecture design proposal (v2): AES-128-CCM for all
  on-flash structures, ESSIV nonces, per-domain key derivation,
  anti-rollback, crash-safe key rotation, external AAD callback.

## [0.23.0] - 2026-04-03

### Added

- **nRF5340 DK board support** (64 KB UBI partition, 4 KB erase
  blocks).

### Changed

- CI split into parallel jobs (native-tests, cross-build, coverage)
  with concurrency control, least-privilege permissions, path
  filters, and SHA-pinned actions.

## [0.22.0] - 2026-04-02

### Added

- **Flash I/O fault injection:** controllable write and erase
  failures.
- New regression coverage: `ubi_io_faults`, `ubi_init_errors`, plus
  34 new error-handling and 6 new recovery tests.
- Long-term EC counter equality test (500 cycles, max deviation ≤ 2).

## [0.21.1] - 2026-04-02

### Fixed

- Sphinx `-W`: warnings now fail the doc build.
- Breathe + Doxygen 1.9.8 compatibility (switched to
  `doxygengroup` directives).

## [0.21.0] - 2026-04-02

### Added

- **Static memory backend (default):** all allocations via
  `k_mem_slab` pools instead of the global heap.
- Memory abstraction layer with Kconfig-selectable backend
  (`static` / `heap`).
- Init-time validation: static backend verifies flash geometry fits
  configured pools.
- Dual-backend CI runs.

### Changed

- PEB tracking items share a union for in-place retyping during
  state transitions.
- Fault injection routed through the memory abstraction layer.

## [0.20.1] - 2026-04-02

### Changed

- Removed read-write lock from roadmap (per-device mutex is
  sufficient).
- Renamed "User-space tools" to "Shell commands" in roadmap.

### Fixed

- Test API functions now visible in Doxygen output.

## [0.20.0] - 2026-04-01

### Added

- **Single-handle-per-partition guard:** prevents two device handles
  for the same flash partition.
- Concurrency test suite: multi-threaded readers/writers, deinit
  quiescence, double-init guard.

### Fixed

- `ubi_device_deinit()` acquires the mutex before teardown,
  preventing races with in-flight operations.

## [0.19.0] - 2026-04-01

### Added

- Volume config validation, device/volume header semantic checks.
- Invariant checker API for tests.
- Allocation fault injection via Kconfig.
- Shared test fixtures and raw flash write helpers.

### Changed

- **Transactional `ubi_volume_create()` and shrink:** RAM state
  consistent with flash on failure.
- **Copy-on-write `leb_write()`:** old mapping preserved until the
  new PEB is fully written.
- `ubi_leb_unmap()` is idempotent; `ubi_leb_map()` is a no-op when
  already mapped.

### Fixed

- PEB tracking loss on allocation failure during bad-block handling.

## [0.18.0] - 2026-04-01

### Added

- Overview doc with mental model, six-step lifecycle, and Mermaid
  diagrams.
- Architecture guide expanded: core invariants, Mermaid flowcharts,
  degraded-mode table.

### Changed

- README rewritten as a landing page with stack diagram, key
  properties, quality metrics.
- All doc pages restructured with "what this page covers" framing.

## [0.17.1] - 2026-03-31

### Fixed

- Doc source-file table and test counts aligned with file splits.
- Sample app expanded to demonstrate the full lifecycle.

## [0.17.0] - 2026-03-31

### Changed

- Internal source file split (`ubi_core.c` into init + runtime,
  `ubi_io.c` into metadata + data). No functional changes.

## [0.16.0] - 2026-03-31

### Added

- `read_only_degraded` exposed in the device-info struct.
- Cached geometry in the device struct (no flash I/O for
  `get_info()`).
- Thread-safety notes and precise `\retval` docs on all public
  functions.

### Changed

- `allocated_peb_count` renamed to `reserved_peb_count`.
- `-EROFS` propagated from the degraded reserved-PEB scan through
  init and mutators.

## [0.15.0] - 2026-03-31

### Added

- Volume name validation helpers (bounded, NUL-safe).
- Semantic validation of on-flash device headers.
- Flash geometry validation at init.

### Fixed

- Eliminated `strlen()` on raw flash fields (memory safety).
- Sequence number monotonicity: `global_sqnum` set to `max + 1`
  after scan.
- Volume headers always read from the highest-revision reserved PEB.
- Reclaim error paths no longer leak PEBs.

## [0.14.0] - 2026-03-30

### Changed

- Reserved PEB module renamed with `ubi_flash_res_peb_*` prefix.
- Volume module deduplicated with shared helpers.

## [0.13.0] - 2026-03-30

### Added

- Bad-block torture test with configurable cycles and per-PEB erase
  attempts.
- Runtime average erase counter tracking.

### Fixed

- Use-after-free in `leb_write()` write-fail path.
- Bad-block paths now update erase counter averages.

## [0.12.0] - 2026-03-30

### Added

- Configurable write retry count for data PEBs
  (`CONFIG_UBI_PEB_WRITE_RETRY_COUNT`).

### Fixed

- Write failure in `leb_write()` now properly marks the PEB as bad.

## [0.11.0] - 2026-03-27

### Added

- Initial crypto layer design proposal (AES-128-CCM via PSA Crypto
  API).

## [0.10.0] - 2026-03-27

### Added

- Reserved-PEB management extracted into a dedicated module.
- **Configurable reserved-PEB count (2–4)** for cold-spare support
  (`CONFIG_UBI_DEV_HDR_NR_OF_RES_PEBS`).

## [0.9.0] - 2026-03-26

### Changed

- Monolithic source restructured into core, volume, LEB, I/O, and
  cache modules.
- Init scan decomposed into 5 helpers.
- Overhauled Doxygen and Kconfig.

### Fixed

- 11 bugs from a code-quality audit.
- Memory leak in duplicate-LEB resolution.

## [0.8.0] - 2026-03-25

### Added

- **Sphinx documentation** with the Read the Docs theme, deployed to
  GitHub Pages.
- Doxygen + Breathe auto-generated API reference.
- Architecture guide, Getting Started, Configuration reference,
  Contributing guide.

## [0.7.0] - 2026-03-25

### Added

- **`native_sim` board support.**
- Test suites: error handling, boundary, recovery, stress.
- **GitHub Actions CI** with build, test, and Codecov.
- Coverage infrastructure and CI scripts.
- Test strategy documentation.

### Fixed

- `west.yml`: `cmsis_6` renamed to `cmsis` for Zephyr v4.0.0.

## [0.6.0] - 2026-03-24

### Added

- Architecture guide with ASCII diagrams (on-flash layout, PEB
  lifecycle, init flow, wear-leveling).
- Development roadmap and contributor guide.

### Fixed

- EBA table corruption during init when resolving sequence-number
  conflicts.

## [0.5.0] - 2025-09-25

### Added

- **Mutex-based synchronisation** for thread-safe operations.

## [0.4.0] - 2025-09-24

### Added

- Sample application for STM32U5.

### Changed

- Optimised flash I/O and improved logging.

## [0.3.0] - 2025-09-21

### Added

- `.clang-format` configuration.

### Changed

- **Migrated from low-level flash APIs to the Zephyr Flash Map
  (Flash Area API).**

## [0.2.0] - 2025-09-10

### Added

- **Volume support** with static and dynamic types.
- Runtime resizing, write-block alignment, partial dual-bank support.
- Hardware tests on STM32U5.

## [0.1.0] - 2025-07-25

### Added

- **Initial release:** device init/deinit, LEB I/O (map, unmap,
  read, write), PEB statistics.
- Hardware integration tests and sample application for STM32U5.
