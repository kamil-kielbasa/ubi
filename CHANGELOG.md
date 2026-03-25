# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
