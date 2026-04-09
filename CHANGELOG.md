# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.26.0] - 2026-04-09

### Changed

- **Erased-state detection no longer assumes `0xFF`** (`lib/src/ubi_core_init.c`, `lib/src/ubi_flash_res_peb.c`): All erased-state checks now use the hardware-reported erased byte value obtained via `flash_area_erased_val()`. Two new internal helpers — `ubi_get_erased_val()` and `ubi_buf_is_erased()` — replace hardcoded `0xFF`/`0xFFFFFFFF` comparisons in PEB scan and reserved PEB classification. On-flash layout is unchanged.
- **Architecture documentation** (`doc/architecture.md`): Updated erased-state descriptions to reflect that the erased byte value is platform-dependent, not universally `0xFF`. Added new "Erased-State Detection" section.
- **Test strategy** (`doc/test_strategy.md`): Added `ubi_erased_val` suite (6 tests) covering helper unit tests and init regression. Documented known gap for non-`0xFF` end-to-end testing.

### Fixed

- **Non-portable erased-state detection**: `validate_vid_header()` used `memset(&empty, 0xff, ...)` and reserved PEB scan compared `hdr.magic == 0xFFFFFFFF`. Both are now derived from the actual flash erased value.

## [0.25.0] - 2026-04-08

### Changed

- **Design proposal: crypto layer v3** (`doc/design_proposal_crypto.md`): Major revision of the secure on-flash architecture. Restructured specification into 18 sections plus appendices. Key changes: new secure init classification rules with decision table and recovery flow diagram for data PEBs (section 10); secure write paths covering reserved metadata update, key rotation, data write and erase/reclaim (section 11); secure read paths for metadata, single-tag and chunked LEB reads (section 12); key lifecycle, inventory and retirement with refcount-based runtime retirement detection (section 13); explicit events, policy and read-only transition table (section 14); Kconfig surface (section 15); API shape summary (section 16); cost model with flash overhead tables (section 17). Replaced inline C API with Appendix A containing illustrative Doxygen-documented API surface (PSA key IDs, event types, rollback verdict, policy struct, callbacks, config struct). Added Appendix B with suggested roadmap items outside the spec.
- **Roadmap** (`doc/roadmap.md`): Added "Recovery correctness for data PEB commit order" feature to the overview matrix and detailed description covering the EC → DATA → VID write order change and init classification fix for free versus uncommitted PEBs.

## [0.24.0] - 2026-04-03

### Added

- **Design proposal: crypto layer v2** (`doc/design_proposal_crypto.md`): Full architecture for authenticated encryption of all UBI on-flash structures (device headers, volume headers, EC headers, VID headers, LEB payloads) using AES-128-CCM via PSA Crypto API. Covers ESSIV nonce construction, per-domain key derivation from a versioned root IKM, anti-rollback via persisted global sequence number, crash-safe key rotation, and external AAD callback for application-specific binding. Linked from [roadmap](roadmap.md).

## [0.23.0] - 2026-04-03

### Added

- **nRF5340 DK board support**: DeviceTree overlays for `nrf5340dk/nrf5340/cpuapp` (tests and sample). UBI partition: 64 KB at 0xF0000 (16 PEBs × 4 KB erase blocks). Added `hal_nordic` to `west.yml` module allowlist.
- **nRF5340 in test matrix**: `testcase.yaml` now lists `nrf5340dk/nrf5340/cpuapp` in `ubi.functional` and `ubi.functional.heap` platform_allow.

### Changed

- **CI: split into granular jobs**: Monolithic `build-and-test` job replaced with three parallel jobs: `native-tests` (matrix: static/heap), `cross-build` (matrix: 2 boards × 2 memory backends × 2 apps = 8 variants), and `coverage` (depends on native-tests). Errors are now reported per-variant with faster feedback.
- **CI: concurrency control**: Added `concurrency` group with `cancel-in-progress: true` — new pushes to the same branch cancel older CI runs.
- **CI: least-privilege permissions**: Workflow-level `permissions: { contents: read }` replaces implicit defaults.
- **CI: path filters**: CI skips runs for documentation-only changes (`doc/**`, `*.md`, `LICENSE`).
- **CI: artifact retention**: `flash-usage` and `coverage-report` artifacts now expire after 14 days (was 90 days default).
- **CI: conditional Codecov upload**: Codecov step is skipped for fork PRs and Dependabot PRs where `CODECOV_TOKEN` is unavailable.
- **CI: SHA-pinned actions**: All GitHub Actions (`actions/checkout`, `actions/upload-artifact`, `codecov/codecov-action`) pinned to full commit SHA instead of mutable tags.
- **CI: flash usage per board**: Flash usage measured and uploaded separately for each board (`flash-usage-b_u585i_iot02a`, `flash-usage-nrf5340dk_cpuapp`).

## [0.22.0] - 2026-04-02

### Added

- **Flash I/O fault injection**: `ubi_test_fault_set_flash_write_fail_after()` and `ubi_test_fault_set_flash_erase_fail_after()` enable controllable flash write and erase failures (requires `CONFIG_UBI_TEST_FAULT_INJECTION`). Flash write faults hook into the internal `flash_write_with_retry()` wrapper. Flash erase faults hook into `ubi_device_erase_peb()` via `ubi_test_flash_erase_check_fail()` (declared in `ubi_io.h`).
- **Test suite: `ubi_io_faults`** (`tests_ubi_io_faults.c`, 24 tests): Flash I/O and malloc fault injection sweep tests — allocation failure sweeps during init with various flash states (empty, volumes, orphans, duplicates, bad VID CRC, bad EC), scratch allocation faults during volume operations, diagnostic allocation faults, and flash erase failure handling.
- **Test suite: `ubi_init_errors`** (`tests_ubi_init_errors.c`, 33 tests): Device initialization error paths — invalid geometry (zero erase/write block size, unaligned partition, oversized write block, too-small partition, erase block smaller than headers), partition guard (`-EBUSY` on double init), format failure propagation, device header corruption, volume header corruption, and `CONFIG_UBI_MEM_BACKEND_STATIC` limit checks.
- **34 new error-handling tests** in `ubi_error_handling` (62 → 96 tests): Corrupt EC/VID header paths in LEB and PEB operations, reserved PEB corruption during create/remove/resize, degraded-mode bank recovery, orphan PEB classification, write-retry exhaustion, `get_peb_ec` with corrupt PEB, invariant checks after bad PEB erase, LEB map/unmap edge cases, volume remove with wrong vol_id, and re-index with corrupt vol headers.
- **6 new recovery tests** in `ubi_recovery` (21 → 27 tests): Dual-bank recovery during resize, degraded-mode mutation blocking, multi-volume bank recovery, corrupt EC with valid VID classification, multiple corrupt PEB classification, and fresh partition spare PEB formatting.
- **Fault reset covers all counters**: `ubi_test_fault_reset()` now resets flash write and erase fault counters in addition to the malloc counter.
- **Long-term EC counter equality test** (`tests_ubi_stress_longrun.c`): 500 write-erase cycle test verifying that erase counters across all PEBs remain balanced (max deviation ≤ 2). Runs on native_sim only. Requires `CONFIG_UBI_TEST_API_ENABLE`.

### Changed

- **Test configuration**: `CONFIG_UBI_TEST_FAULT_INJECTION=y` enabled in `tests/prj.conf` by default for all test builds.
- **Fault injection declarations**: `ubi_test_fault_set_flash_write_fail_after()` and `ubi_test_fault_set_flash_erase_fail_after()` moved from internal scope to public API in `ubi.h` (with no-op stubs when `CONFIG_UBI_TEST_FAULT_INJECTION` is disabled).
- **Source module roles**: `ubi_io_data.c` now also hosts flash write/erase fault injection counters and check functions.
- **Coverage**: Line coverage increased from ~80% to 85.2% (1517/1781 lines). 17 test suites, 228 tests total.

### Fixed

- **Test description ordering**: All test Doxygen comments now consistently use `\brief` → `\details` → `\expect` order (previously some used `\brief` → `\expect` → `\details`).
- **Test descriptions: removed source line references**: Removed direct references to source file line numbers (e.g. "Covers ubi_leb.c lines 237-238") from test Doxygen comments — line numbers change across refactors and become stale.

## [0.21.1] - 2026-04-02

### Added

- **Docs: `-W` flag**: `docs.yml` now runs `sphinx-build -W` so Sphinx warnings fail the build.

### Changed

- **Doxygen invocation**: `conf.py` uses `subprocess.check_call` instead of `subprocess.call` — Doxygen failures now break the build.

### Fixed

- **Sphinx: `design_proposal_crypto` orphan warning**: added document to `index.rst` toctree.
- **Docs: Breathe + Doxygen 1.9.8 compatibility**: switched `api.rst` from `doxygenfunction`/`doxygenstruct` directives to `doxygengroup`, matching the approach used in libedhoc. Breathe's function finder filter excludes group compounds, so when Doxygen ≥ 1.9.8 places functions only in group XML files (not file XML), `doxygenfunction` fails. Using `doxygengroup` reads group XML directly and works with both Doxygen 1.9.1 and 1.9.8.

## [0.21.0] - 2026-04-02

### Added

- **Static memory backend** (`CONFIG_UBI_MEM_BACKEND_STATIC`, default): all UBI runtime allocations use `k_mem_slab` pools (device, volume, leaf, scratch) instead of the global Zephyr heap.
- **Memory abstraction layer** (`ubi_mem.h` / `ubi_mem.c`): encapsulates all UBI memory operations behind a single API, supporting heap and static backends via Kconfig.
- **Kconfig options**: `UBI_MEM_BACKEND` (STATIC/HEAP), `UBI_MAX_NR_OF_DEVICES`, `UBI_MAX_NR_OF_DATA_PEBS`, `UBI_MEM_STATS`.
- **Init-time validation**: static backend verifies flash geometry fits within configured pool limits.
- **Dual-backend CI**: `testcase.yaml` runs all suites under both backends.

### Changed

- **PEB tracking**: `ubi_rbt_item` and `ubi_list_item` share `union ubi_leaf_item` (16 B), enabling in-place retyping during state transitions.
- **Fault injection**: operates through `ubi_mem` layer; declarations moved from deleted `ubi_test_hooks.h` to `ubi.h`.
- **Partition guard ordering**: `ubi_device_init()` acquires partition before device allocation.
- **Error logging**: all allocation and metadata error paths now emit `LOG_ERR`.

### Removed

- `ubi_test_hooks.h` / `ubi_test_hooks.c` — fault injection API moved to `ubi.h`, implementation to `ubi_mem.c`.

### Fixed

- **Fault injection broken**: all allocations now route through `ubi_mem`, making the fault counter functional.
- **Runtime RAM example**: corrected from ~432 B to ~464 B (missing volume tree nodes).

## [0.20.1] - 2026-04-02

### Fixed

- **Doxygen: test API functions missing**: Added `PREDEFINED = CONFIG_UBI_TEST_API_ENABLE` to `doc/Doxyfile` so that `ubi_device_check_invariants()` and `ubi_device_get_peb_ec()` (guarded by `#if defined(CONFIG_UBI_TEST_API_ENABLE)`) are extracted into the Doxygen XML and rendered by Breathe.

### Changed

- **Roadmap: removed read-write locking**: Dropped the planned read-write lock feature from the roadmap, README, introduction, and architecture docs. The current per-device mutex is sufficient.
- **Roadmap: renamed user-space tools to shell commands**: Replaced "User-space tools" with "Shell commands" across roadmap and README to better reflect the Zephyr shell integration.

## [0.20.0] - 2026-04-01

### Added

- **Single-handle-per-partition guard**: `ubi_partition_acquire()` / `ubi_partition_release()` in `lib/src/ubi_partition_guard.h` / `ubi_partition_guard.c` — static bitfield registry that prevents two `ubi_device` handles for the same flash partition. `ubi_device_init()` returns `-EBUSY` if the partition is already in use.
- **Concurrency test suite**: `tests/src/tests_ubi_concurrency.c` — multi-threaded tests using `k_thread_create` / `k_thread_join`: concurrent metadata readers, reader-writer interleave, deinit-after-quiescence, double-init guard (`-EBUSY`), init-after-deinit reuse.

### Fixed

- **`ubi_device_deinit()` thread safety**: Now acquires the device mutex before freeing resources. In-flight operations that hold the mutex complete before teardown proceeds.

### Changed

- **`ubi_device_deinit()` contract** (`lib/include/ubi.h`): Added `\pre` clause — caller must ensure no new operations start after calling deinit.
- **`ubi_device_init()` contract** (`lib/include/ubi.h`): Documents `-EBUSY` and the single-handle-per-partition invariant.
- **Documentation**: Thread Safety section in `doc/architecture.md` now documents the deinit contract and single-handle-per-partition rule. Source file table includes `ubi_partition_guard` module. Test counts updated across `doc/test_strategy.md` and `doc/getting_started.md`.

## [0.19.0] - 2026-04-01

### Added

- **Volume configuration validation**: `ubi_volume_config_is_valid()` in `lib/src/ubi_internal.h` — enforces valid name, `UBI_VOLUME_TYPE_STATIC` / `DYNAMIC`, and `leb_count > 0` for `ubi_volume_create()`.
- **Metadata semantic checks**: `ubi_dev_hdr_semantically_valid()` and `ubi_vol_hdr_semantically_valid()` — reject CRC-valid but invalid on-flash fields; used in reserved PEB scan (`lib/src/ubi_flash_res_peb.c`) and volume collection (`lib/src/ubi_core_init.c`).
- **Test API**: `ubi_device_check_invariants()` when `CONFIG_UBI_TEST_API_ENABLE` — verifies PEB accounting, tree sizes vs counters, and reserved PEB sum (`lib/src/ubi_core_runtime.c`, `lib/include/ubi.h`).
- **Fault injection (Kconfig)**: `UBI_TEST_FAULT_INJECTION` (requires `UBI_TEST_API_ENABLE`) — controllable allocation hook API (`ubi_test_fault_reset()`, `ubi_test_fault_set_malloc_fail_after()`) in `lib/include/ubi.h`, implemented in `lib/src/ubi_mem.c`.
- **Shared test headers**: `tests/src/ubi_test_fixture.h`, `ubi_test_memory.h`, `ubi_test_raw_flash.h` for MTD setup, partition erase, heap snapshots, and raw EC/VID writes.
- **New test suites**: `tests/src/tests_ubi_fault_injection.c`, `tests_ubi_stress_longrun.c` (with `CONFIG_FLASH_SIMULATOR`), `tests_ubi_hil_smoke.c`; contract tests in `tests_ubi_error_handling.c` (invalid type, zero LEBs, idempotent unmap, no-op map, static volume write).

### Changed

- **Transactional `ubi_volume_create()`**: Allocate `struct ubi_volume` and rbt item before flash append; on failure, no persistent volume is written without matching RAM state (`lib/src/ubi_volume.c`).
- **Transactional shrink in `ubi_volume_resize()`**: Flash metadata update (`ubi_vol_hdr_update`) completes before trimming EBA entries and reclaiming PEBs to dirty.
- **`ubi_volume_remove()`**: After successful flash remove, reclaim and vol_idx re-index are best-effort (errors logged, operation still completes with success when metadata removal succeeded).
- **Capacity accounting**: `ubi_volume_create()` and resize-grow path subtract `bad_peb_count` from usable PEBs before comparing to requested `leb_count`.
- **`ubi_volume_resize()`**: Rejects `vol_cfg->leb_count == 0` with `-EINVAL`; shrink loop uses `lnum` from new count upward (removed dead `diff == 0` branch).
- **Copy-on-write `leb_write()`**: New PEB is written before the old EBA mapping is removed; on write failure the previous mapping and data remain (`lib/src/ubi_leb.c`).
- **`reclaim_peb_to_dirty()`**: If EC read fails and bad-block list allocation fails, PEB is kept in the dirty pool with average EC key instead of being dropped from tracking.
- **`resolve_duplicate_leb()`**: When the existing mapping’s headers are unreadable, replace EBA with the current PEB after marking the old PEB bad (`lib/src/ubi_core_init.c`).
- **`ubi_leb_unmap()`**: Idempotent — unmapped LEB returns `0`.
- **`ubi_leb_map()`**: No-op when already mapped; otherwise uses `leb_prepare_new_mapping()` + `leb_commit_mapping_swap()` (no longer delegates through `leb_write()`).
- **Refactor**: `leb_prepare_new_mapping()`, `leb_commit_mapping_swap()`, `leb_mark_peb_bad()` extracted in `ubi_leb.c`.
- **Documentation** (`lib/include/ubi.h`): `ubi_volume_create` / `resize` `-EINVAL` details; `ubi_leb_write` for static and dynamic volumes; `ubi_volume_get_info` documents `-ENOENT` for missing volume.

### Fixed

- **PEB tracking**: Eliminated loss of PEB from all trees when `reclaim_peb_to_dirty()` hit `-ENOMEM` on bad-block allocation after EC read failure.

## [0.18.0] - 2026-04-01

### Added

- **Documentation**: New `doc/overview.md` — mental model (PEB/LEB/EC/VID/EBA), six-step lifecycle, stack Mermaid diagram, links to deeper docs.
- **Sphinx**: `sphinxcontrib-mermaid` in `doc/requirements.txt` and `myst_fence_as_directive` for Mermaid in MyST (`doc/conf.py`).
- **Architecture guide**: 30-second summary, Core Invariants table, Mermaid flowcharts (write, read, erase/reclaim), PEB lifecycle state diagram, degraded-mode operation table.

### Changed

- **README**: Expanded landing page — stack diagram, key properties, when to use / not, documentation map, project quality, design trade-offs.
- **Sphinx index** (`doc/index.rst`): “Start here” guidance, grouped toctree (Understanding / Using / Quality / Project), aligned flash footprint wording with introduction (~6.7 KB).
- **Contributing**: Root `CONTRIBUTING.md` is a short pointer to the full guide; `doc/contributing.md` rewritten with current source layout, dev loop, testing and documentation expectations, PR checklist.
- **Configuration** (`doc/configuration.md`): Impact analysis table for Kconfig options, sizing example, “what this page covers” framing.
- **Test strategy** (`doc/test_strategy.md`): Executive summary table (all suites and counts), native_sim vs hardware comparison, known gaps as a structured table.
- **API docs** (`doc/api.rst`): Usage notes — lifecycle, thread safety, error model table, typical call sequence.
- **Introduction** (`doc/introduction.md`): Non-goals table, tighter opening, resource profile notes.
- **Getting started** (`doc/getting_started.md`): “Before you start” paths (evaluation vs integration vs hardware).

### Fixed

- **Documentation**: Degraded read-only mode now documents `-EROFS` (not `-EIO`) for `ubi_volume_create` / `resize` / `remove` in `architecture.md`.
- **On-flash layout docs**: Corrected data-PEB range wording and init scan phases to use PEB indices N..total-1 (reserved count N) instead of hardcoded “2..N-1”.
- **Public API docs** (`ubi.h`): `ubi_leb_write` — document internal padding for unaligned lengths and align `\retval` with implementation; copyright year 2026.

## [0.17.1] - 2026-03-31

### Fixed

- **Documentation truthfulness**: Updated `architecture.md` source file table to reflect Phase 3 splits (`ubi_core_init.c`, `ubi_core_runtime.c`, `ubi_io_metadata.c`, `ubi_io_data.c`, `ubi_flash_res_peb.*`). Fixed test count in `getting_started.md` (89 → 111). Added `ubi_torture` suite to `test_strategy.md`.
- **Sample**: Expanded `sample/src/main.c` to demonstrate full lifecycle (init → create → write → read → get_info → deinit). Fixed `definitiones` typo, removed stale `\version`/`\date` header, replaced Yoda conditions with idiomatic style.

### Changed

- **Style normalization**: Removed per-file `\version`/`\date` Doxygen tags from all library headers and sources — version is tracked via CHANGELOG and git tags only.
- Replaced Yoda conditions (`0 == len`, `false == is_mounted`) with idiomatic C style (`len == 0`, `!is_mounted`) across `ubi_leb.c`, `ubi_io_data.c`, and `ubi_core_init.c`.
- Improved log message in `ubi_volume.c`: "Lack of available for allocation LEBs" → "Not enough free PEBs to allocate requested LEBs".

## [0.17.0] - 2026-03-31

### Changed

- **Split `ubi_core.c`** into `ubi_core_init.c` (device init, format, scan, volume collection) and `ubi_core_runtime.c` (get_info, erase_peb, deinit, test API). No functional changes.
- **Split `ubi_io.c`** into `ubi_io_metadata.c` (device/volume header read/write/append/remove/update) and `ubi_io_data.c` (EC/VID header and LEB data read/write). No functional changes.
- Updated `CMakeLists.txt` to reference the new source files.

### Removed

- `ubi_core.c` — replaced by `ubi_core_init.c` + `ubi_core_runtime.c`.
- `ubi_io.c` — replaced by `ubi_io_metadata.c` + `ubi_io_data.c`.

## [0.16.0] - 2026-03-31

### Added

- `read_only_degraded` field in `struct ubi_device_info` — exposes whether the device lost reserved PEB redundancy and is operating in degraded read-only mode for metadata operations.
- Cached `total_data_peb_count` and `leb_size` in the internal device struct, eliminating `flash_area_open()` from `ubi_device_get_info()`.
- `ubi_reserved_peb_count()` internal helper for computing reserved PEB sum without acquiring mutex or performing flash I/O.
- Thread-safety notes on all public API groups (`\note` blocks in `ubi.h`): all functions use a per-device mutex and must not be called from ISR context.
- Precise `\retval` documentation for every public function, including `-EROFS`, `-EIO`, `-ENOENT`, and `-ECANCELED` where applicable.
- `-EROFS` documented as a return code for `ubi_volume_create()`, `ubi_volume_resize()`, and `ubi_volume_remove()` when the device is in degraded mode.

### Changed

- **Renamed** `ubi_device_info.allocated_peb_count` → `reserved_peb_count` to accurately reflect the semantics (sum of `leb_count` across all volumes, not physically mapped PEBs).
- `ubi_dev_hdr_read()` now propagates `-EROFS` from `ubi_flash_res_peb_validate()` instead of silently swallowing it; callers can detect degraded mode at the I/O layer.
- `ubi_device_init()` handles `-EROFS` from the device header read: sets the degraded flag and continues initialization (previously would have hidden the condition).
- `ubi_device_get_info()` is now a lightweight in-memory operation — uses cached geometry instead of opening the flash area on every call.
- `ubi_volume_create()` and `ubi_volume_resize()` no longer call the public `ubi_device_get_info()` under the already-held mutex; replaced with direct internal computation via `ubi_reserved_peb_count()`.
- `dev_hdr_read_and_bump()` explicitly returns `-EROFS` with a descriptive log message when the device is in degraded mode, failing fast before attempting metadata writes.

## [0.15.0] - 2026-03-31

### Added

- `ubi_validate_volume_name()`, `ubi_copy_name_to_hdr()`, `ubi_copy_name_from_hdr()` — safe volume name helpers in `ubi_internal.h`.
- Semantic validation of device headers in reserved PEB scan (`vol_count`, header version, header-vs-erase-block size check).
- `canonical_peb_idx` field in `struct ubi_flash_res_peb_scan` for deterministic canonical copy selection.
- Flash geometry validation in `ubi_device_init()` — rejects zero sizes, misaligned partitions, partitions too small for reserved PEBs, and unsupported `write_block_size`.
- Tests: duplicate-name with different config (`-EEXIST`), empty name, name without NUL, max-length name, sqnum monotonicity across remount.

### Changed

- `ubi_volume_create()` contract: duplicate name with identical config returns existing `vol_id` (idempotent); duplicate name with different config returns `-EEXIST`. Updated `ubi.h` documentation accordingly.
- `ubi_vol_hdr_read()` now reads from the canonical (highest-revision) reserved PEB instead of the first active one.
- `ubi_vol_hdr_append()` reads existing content from the canonical reserved PEB.
- `ubi_flash_res_peb_validate()` performs recovery from the canonical PEB.
- `ubi_leb_data_write()` uses `mtd->write_block_size` for alignment instead of the hardcoded `WRITE_BLOCK_SIZE_ALIGNMENT` constant.

### Fixed

- **Memory safety**: eliminated all `strlen()` calls on raw on-flash fixed-size name fields; replaced with bounded `strnlen()` and safe copy helpers ensuring NUL-termination.
- **Sequence number monotonicity**: `global_sqnum` is now set to `max + 1` after PEB scan, preventing reuse of existing sequence numbers after device re-init.
- **Mixed-revision metadata**: volume headers are now always read from the highest-revision reserved PEB, preventing inconsistent state after interrupted metadata commits.
- **Reclaim error paths**: `reclaim_peb_to_dirty()` now always consumes its item (moves to dirty pool or marks as bad), preventing orphaned PEBs and potential double-free in `ubi_volume_remove()`.
- **Boundary conditions**: `ubi_vol_hdr_read()` index check changed from `>` to `>=`; removed incorrect `vol_count >= MAX` guards from `ubi_vol_hdr_remove()` and `ubi_vol_hdr_update()` that prevented operations at maximum volume count.

## [0.14.0] - 2026-03-30

### Changed

- Renamed `ubi_res_peb.h` / `ubi_res_peb.c` to `ubi_flash_res_peb.h` / `ubi_flash_res_peb.c` with `ubi_flash_res_peb_*` prefix on all symbols.
- `ubi_volume.c` deduplicated with `dev_hdr_read_and_bump()` (3 call sites) and `reclaim_peb_to_dirty()` (2 call sites).

### Removed

- Volume module simplification entry from `doc/roadmap.md` (implemented).

## [0.13.0] - 2026-03-30

### Added

- Bad block torture test: erase-only recovery controlled by `CONFIG_UBI_BAD_PEB_TORTURE_CYCLES` (bad PEBs per call, range 1–10, default 3) and `CONFIG_UBI_BAD_PEB_TORTURE_MAX_PER_ERASE` (erase attempts per PEB, range 1–10, default 1).
- Runtime `ec_avg` tracking via `ec_sum`/`ec_count` in device struct; exposed in `ubi_device_info.ec_avg`.
- 5 torture recovery tests (`tests_ubi_torture.c`).

### Fixed

- Use-after-free in `leb_write()` write-fail path: PEB metadata is now saved before `k_free()`.
- `leb_write()` write-fail now passes correct erase count (was 0) and updates `ec_sum`/`ec_count`.
- `erase_peb()` bad-block paths now update `ec_sum`/`ec_count` for consistent average tracking.

### Removed

- Bad block torture test and permanent bad block tracking entries from `doc/roadmap.md` (implemented/obsoleted).

## [0.12.0] - 2026-03-30

### Added

- `CONFIG_UBI_PEB_WRITE_RETRY_COUNT` Kconfig option (range 1–5, default 3) for data-PEB write retry.

### Fixed

- `leb_write()` now marks PEB bad when VID header or LEB data write fails (previously leaked the PEB).

### Removed

- Write retry mechanism entry from `doc/roadmap.md` (implemented).

## [0.11.0] - 2026-03-27

### Added

- `doc/design_proposal_crypto.md`: design proposal for authenticated encryption layer (AES-128-CCM via PSA Crypto API).
- Crypto layer entry in `doc/roadmap.md` (Priority: High, Status: Design).

## [0.10.0] - 2026-03-27

### Added

- `ubi_flash_res_peb.h` / `ubi_flash_res_peb.c`: reserved PEB management module extracted from `ubi_io.c`.
- `CONFIG_UBI_DEV_HDR_NR_OF_RES_PEBS` Kconfig option (range 2–4, default 2) for cold spare support.
- Volume header validation in reserved PEB scan.
- `data_size` boundary check in `ubi_leb_read()`.
- 12 new tests (recovery, data_size boundary). Test count: 89 → 101.

### Changed

- Reserved PEB functions use `enum ubi_flash_res_peb_state` and unified `ubi_flash_res_peb_*` naming.
- `ubi_flash_res_peb_overwrite()` seeks immediate replacement on active PEB failure instead of batching.
- `ubi_dev_is_mounted()` treats corrupt PEBs as evidence of a previously mounted device.

### Fixed

- Impossible validate condition (`active < 2 && spare == 0 && corrupt == 0` with N=2).

## [0.9.0] - 2026-03-26

### Added

- `ubi_cache.h` / `ubi_cache.c`: extracted red-black tree cache module.
- `ubi_internal.h`: shared internal types and helpers.
- Coverage tests: `leb_write_all_pebs_exhausted`, `leb_map_all_pebs_exhausted`.
- CI step to measure ARM flash usage on `b_u585i_iot02a` with artifact upload.
- Roadmap entries: bad block torture test, volume module simplification.

### Changed

- Restructured monolithic `ubi.c` into `ubi_core.c`, `ubi_volume.c`, `ubi_leb.c`.
- Renamed `ubi_utils.h` / `ubi_utils.c` to `ubi_io.h` / `ubi_io.c`.
- Renamed `ubi_device_info` fields from `leb_*` to `peb_*` naming (tracks physical erase blocks).
- Renamed cache functions `ubi_rbt_cmp` / `ubi_rbt_search` to `ubi_cache_cmp` / `ubi_cache_search`.
- Decomposed `ubi_device_init` into 4 sub-functions.
- Decomposed `init_scan_pebs` into 5 helpers (`validate_ec_header`, `validate_vid_header`, `classify_orphan_peb`, `map_leb_first_occurrence`, `resolve_duplicate_leb`).
- Scoped `flash_area_open` / `close` per PEB operation.
- Deduplicated volume header ops with `validate_dual_bank()` and `commit_dual_bank()`.
- Extracted `find_volume()` helper.
- Replaced numbered comments with descriptive ones.
- Overhauled Doxygen on all public headers.
- Updated Kconfig: Zephyr logging template, `depends on FLASH && FLASH_MAP && CRC`, range constraint.
- Unified condition style from Yoda (`0 != ret`) to standard (`ret != 0`).
- Updated all file dates to 2026-03-26.
- Removed Kconfig copyright header.

### Fixed

- 11 bugs from code quality audit.
- Volume type Doxygen: static = fixed LEB count, dynamic = resizable.
- Typo in `ubi_volume_remove()`: "readd" → "read".
- `init_scan_pebs` L237: bare `return` on VID read failure converted to `continue` with bad block classification (consistent with EC failure handling).
- Memory leak in `init_scan_pebs` duplicate-LEB path when existing PEB header read fails.

### Removed

- Dead code eliminated during file restructure.

## [0.8.0] - 2026-03-25

### Added

- Sphinx documentation with Read the Docs theme, deployed to GitHub Pages.
- Doxygen + Breathe auto-generated API reference from `ubi.h`.
- Introduction page: what is UBI, why UBI on Zephyr, resource usage, feature summary.
- Getting started guide: quick start, native_sim build, STM32U5 build, tests, coverage.
- Configuration reference: Kconfig options, DeviceTree overlays, sizing guidelines.
- Contributing guide in Sphinx (mirrored from root `CONTRIBUTING.md`).
- Changelog included in Sphinx via MyST `{include}` directive.
- `doc/Makefile` for local documentation builds.
- Docs badge in `README.md`.
- `docs` CI job: builds Sphinx docs and deploys to GitHub Pages on push to `main`.

### Changed

- Slimmed `README.md` to a gateway page (badges, quick start, link to full docs).
- Moved resource usage tables, features list, and documentation links from `README.md` to Sphinx.
- Moved "What is UBI?" and API Reference sections from `architecture.md` to dedicated Sphinx pages.
- Updated `CONTRIBUTING.md` to reference native_sim build instructions.

### Removed

- `doc/environment_setup.md` content superseded by `doc/getting_started.md`.
- Manual API reference table from `architecture.md` (replaced by auto-generated Breathe docs).

## [0.7.0] - 2026-03-25

### Added

- native_sim board support for tests and sample.
- Test suites: error handling (50), boundary (5), recovery (10), stress (4) — 87 tests total.
- GitHub Actions CI workflow with build, test, and Codecov coverage upload.
- Code coverage infrastructure (`native_sim_coverage.conf`, `scripts/coverage.sh`).
- CI/test runner scripts (`scripts/run_tests.sh`, `scripts/ci.sh`).
- Twister test metadata (`tests/testcase.yaml`).
- Test strategy documentation (`doc/test_strategy.md`).
- CI, Codecov, and license badges in `README.md`.
- Doxygen-style documentation for all test functions.

### Changed

- Pinned Zephyr to `v4.0.0` in `west.yml` (was `main`).
- Enabled strict compile flags (`-Werror -Wextra -Wshadow` etc.) for library and tests.
- Portable `BUILD_ASSERT` and `device_is_ready()` for cross-platform builds.
- native_sim erase-block-size set to 8192 to match STM32U5 geometry.
- CI uploads line-only coverage to Codecov (eliminates phantom branches from
  Zephyr LOG macros). Branch-coverage HTML report kept as build artifact.

### Fixed

- `west.yml`: `cmsis_6` → `cmsis` for Zephyr v4.0.0.
- `scripts/format.sh`: sample path pointed to `tests/src` instead of `sample/src`.
- Recovery test: erase PEB before writing garbage (hardware compatibility).

## [0.6.0] - 2026-03-24

### Added

- Architecture guide with ASCII diagrams covering on-flash layout, in-RAM data structures, PEB lifecycle, device initialization flow, and wear-leveling strategy (`doc/architecture.md`).
- Development roadmap with prioritized feature list (`doc/roadmap.md`).
- Contributor guide with code style, build/test, and PR workflow (`CONTRIBUTING.md`).
- Thread safety section in architecture documentation.

### Changed

- Rewrote `README.md` with quick-start example, structured documentation links, and updated resource usage table.
- Adopted [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) format for `CHANGELOG.md`.
- Refactored `doc/environment_setup.md` with prerequisites table, numbered workflow, and troubleshooting notes.
- Renamed `doc/features_candidates.md` to `doc/roadmap.md` with status and priority tracking.

### Fixed

- EBA table corruption during init when resolving sequence number conflicts (`ubi.c`: `item->key` was overwritten with PEB index instead of setting `item->value.pnum`).

### Removed

- `doc/features_candidates.md` (replaced by `doc/roadmap.md`).

## [0.5.0] - 2025-09-25

### Added

- Mutex-based synchronization for thread-safe device and volume operations.

## [0.4.0] - 2025-09-24

### Added

- Sample application demonstrating UBI initialization on STM32U5.

### Changed

- Optimized flash read/write operations to reduce unnecessary flash area open/close cycles.
- Improved logging messages across all modules.
- Deduplicated common code paths in volume and LEB operations.
- Reorganized source file structure for clarity.
- Updated Doxygen documentation for all public API functions.

## [0.3.0] - 2025-09-21

### Added

- `.clang-format` configuration file for consistent code style.

### Changed

- Replaced low-level Zephyr flash APIs with the Zephyr Flash Map (Flash Area API) for better portability and abstraction.

## [0.2.0] - 2025-09-10

### Added

- Volume support with static and dynamic volume types.
- Runtime resizing for dynamic volumes.
- Write block alignment handling (transparent to the caller).
- Partial dual-bank support for device and volume headers on reserved PEBs.
- Hardware integration tests on `b_u585i_iot02a`.

### Removed

- Hardware test documentation (superseded by updated environment setup guide).
- Temporary sample application (reintroduced in v0.4.0).

## [0.1.0] - 2025-07-25

### Added

- UBI device initialization and deinitialization routines.
- LEB and PEB statistics reporting, including per-PEB erase counters.
- LEB I/O operations: map, unmap, read, and write.
- Hardware integration tests on `b_u585i_iot02a`.
- Example application for `b_u585i_iot02a`.
- Environment setup documentation.
  - Hardware testing procedures.  
  - Candidate features for future development.  
- Clang-format script for consistent code formatting.  

**Changed**  
- _No changes in this release._  

**Removed**  
- _No removals in this release._  

**Fixed**  
- _No fixes in this release._  

**Contributors**  
- [@kamil-kielbasa](https://github.com/kamil-kielbasa)  
