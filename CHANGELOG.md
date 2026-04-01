# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.19.0] - 2026-04-01

### Added

- **Volume configuration validation**: `ubi_volume_config_is_valid()` in `lib/src/ubi_internal.h` — enforces valid name, `UBI_VOLUME_TYPE_STATIC` / `DYNAMIC`, and `leb_count > 0` for `ubi_volume_create()`.
- **Metadata semantic checks**: `ubi_dev_hdr_semantically_valid()` and `ubi_vol_hdr_semantically_valid()` — reject CRC-valid but invalid on-flash fields; used in reserved PEB scan (`lib/src/ubi_flash_res_peb.c`) and volume collection (`lib/src/ubi_core_init.c`).
- **Test API**: `ubi_device_check_invariants()` when `CONFIG_UBI_TEST_API_ENABLE` — verifies PEB accounting, tree sizes vs counters, and reserved PEB sum (`lib/src/ubi_core_runtime.c`, `lib/include/ubi.h`).
- **Fault injection (Kconfig)**: `UBI_TEST_FAULT_INJECTION` (requires `UBI_TEST_API_ENABLE`) — controllable `k_malloc` hook API (`ubi_test_malloc()`, `ubi_test_fault_reset()`, `ubi_test_fault_set_malloc_fail_after()`) in `lib/src/ubi_test_hooks.h` / `ubi_test_hooks.c`.
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
