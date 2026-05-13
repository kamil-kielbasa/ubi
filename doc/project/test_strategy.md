# Test Strategy

**What this page covers:** How to build and run the test suite, code
coverage, the forensic scan, test categories, environments, coverage
targets, test patterns, and known gaps.

**Prerequisites:** [Quick Start](/getting_started/quick_start.md) for the basic Zephyr +
UBI build.

## Building and running tests

### Prerequisites

| Tool | Purpose | Install |
|------|---------|---------|
| [west](https://docs.zephyrproject.org/latest/develop/west/index.html) | Zephyr meta-tool (build, flash, manage manifests) | `pip install west` |
| [Zephyr SDK](https://docs.zephyrproject.org/latest/develop/toolchains/zephyr_sdk.html) | Cross-compilation toolchain | See Zephyr docs |
| [STM32CubeProgrammer CLI](https://www.st.com/en/development-tools/stm32cubeprog.html) | Flash erase and programming (hardware only) | ST website |
| [picocom](https://github.com/npat-efault/picocom) | Serial terminal for UART output (hardware only) | `sudo apt install picocom` |

### Initialise the workspace

```sh
west init -l .
west update --narrow -o=--depth=1
```

### Build for `native_sim` (simulator)

Build and run the **test** suite:

```sh
west build -p --build-dir build/native_sim/tests -b native_sim ./tests/
./build/native_sim/tests/zephyr/zephyr.exe
```

Build and run the **sample** application:

```sh
west build -p --build-dir build/native_sim/sample -b native_sim ./sample/
./build/native_sim/sample/zephyr/zephyr.exe
```

### Build for STM32U5 (`b_u585i_iot02a`)

```sh
west build -p --build-dir build/stm32u5/tests -b b_u585i_iot02a ./tests/

STM32_Programmer_CLI -c port=SWD -e all
STM32_Programmer_CLI -c port=SWD -d ./build/stm32u5/tests/zephyr/zephyr.hex

picocom -b 115200 /dev/ttyACM0
```

### Build for nRF5340 DK (`nrf5340dk/nrf5340/cpuapp`)

```sh
west build -p --build-dir build/nrf5340dk/tests -b nrf5340dk/nrf5340/cpuapp ./tests/

west flash --build-dir build/nrf5340dk/tests

picocom -b 115200 /dev/ttyACM0
```

### Running the test modes

Tests run on `native_sim` in three modes:

```sh
# Plain tests (default)
bash scripts/run_tests.sh native_sim plain

# Secure tests
bash scripts/run_tests.sh native_sim secure

# Chunked secure tests
bash scripts/run_tests.sh native_sim chunked
```

### Code coverage

Generate separate HTML coverage reports for plain and secure:

```sh
# Plain coverage
bash scripts/coverage.sh plain

# Secure coverage
bash scripts/coverage.sh secure
```

Reports are written to `build/coverage-plain/html/` and
`build/coverage-secure/html/`.

### Forensic scan

After running secure tests, scan the flash image for forbidden plaintext:

```sh
python3 scripts/scan_flash.py flash.bin
```

### Test description check

Every `ZTEST()` must carry the three required Doxygen tags `\brief`,
`\details`, `\expected` in the docblock immediately preceding it.  Three
additional tags are *optional* and reported only as warnings:

| Tag             | Status   | Purpose                                                                          |
|-----------------|----------|----------------------------------------------------------------------------------|
| `\brief`        | required | One-line summary of what the test verifies.                                      |
| `\details`      | required | Step-by-step scenario, including the relevant configuration / fixture state.     |
| `\expected`     | required | Pass/fail criteria phrased as observable behaviour.                              |
| `\oracle`       | optional | The numeric / observational oracle that proves the test (e.g. `mem_equal`, counter monotonicity, exact return code). |
| `\trace`        | optional | Back-link to a spec section or requirement id (e.g. `§9.8.5`).                    |
| `\precondition` | optional | Non-obvious environmental setup the test relies on (Kconfig, fault hooks, geometry). |

Run the checker as part of the local pre-push loop:

```sh
# Default — fail only if a required tag is missing; report optional gaps as warnings.
python3 scripts/check_test_descriptions.py tests/src/

# Strict mode — also fail if any of the three optional tags is missing.
python3 scripts/check_test_descriptions.py tests/src/ --strict
```

The optional tags should be added to new and modified tests; existing
tests that predate the template are not retro-fitted in a single sweep.
The warning channel exists so the pattern can be adopted incrementally
without blocking unrelated changes.

### Code formatting

Apply the project's `.clang-format` rules:

```sh
./scripts/format.sh
```

Dry-run check (mirrors the CI format-check job):

```sh
./scripts/format.sh --check
```

## Executive Summary

| Suite | File | Tests | Focus | Environment |
|-------|------|------:|-------|-------------|
| `ubi_device` | `tests_ubi_device.c` | 2 | Device init, deinit, get_info | native_sim |
| `ubi_volumes` | `tests_ubi_volumes.c` | 6 | Volume create, remove, resize, get_info | native_sim |
| `ubi_map_unmap` | `tests_ubi_map_unmap.c` | 3 | LEB map, unmap, is_mapped | native_sim |
| `ubi_write_read` | `tests_ubi_write_read.c` | 5 | LEB write, read, get_size | native_sim |
| `ubi_erase` | `tests_ubi_erase.c` | 2 | PEB erase, dirty-to-free recycling | native_sim |
| `ubi_mixed` | `tests_ubi_mixed.c` | 1 | Multi-volume cross-functional workflows | native_sim |
| `ubi_error_handling` | `tests_ubi_error_handling.c` | 97 | NULL params, out-of-range, no-space, contract tests, corrupt headers, reserved PEB corruption, degraded recovery, LEB edge cases | native_sim |
| `ubi_boundary` | `tests_ubi_boundary.c` | 6 | Max LEB capacity, alignment, sqnum persistence | native_sim |
| `ubi_recovery` | `tests_ubi_recovery.c` | 30 | Corruption, dual-bank, sqnum conflicts, degraded mode, multi-volume recovery, free vs. uncommitted PEB classification | native_sim |
| `ubi_fault_injection` | `tests_ubi_fault_injection.c` | 6 | Transactional safety under allocation failures, COW overwrite, invariant checks, per-kind allocator fault selector | native_sim |
| `ubi_io_faults` | `tests_ubi_io_faults.c` | 26 | Malloc/flash-write/flash-erase fault sweeps across init, volume, and I/O paths; commit-order fault injection (VID write failure preserves old mapping) | native_sim |
| `ubi_io_defensive` | `tests_ubi_io_defensive.c` | 16 | Plain-backend internal I/O API defensive guards: NULL/range rejection on `ubi_ec_hdr_*`, `ubi_vid_hdr_*`, `ubi_leb_data_*`; metadata helper NULL guards; reserved-PEB helper NULL/range guards plus `find_first_active` synthetic-scan oracle; `flash_area_open` failure propagation through every helper | native_sim |
| `ubi_init_errors` | `tests_ubi_init_errors.c` | 33 | Geometry validation, partition guard, format failures, header corruption at init | native_sim |
| `ubi_stress` | `tests_ubi_stress.c` | 4 | Full utilization, init cycling, wear leveling | native_sim (simulator only) |
| `ubi_stress_longrun` | `tests_ubi_stress_longrun.c` | 4 | Randomized churn with reboots, multi-volume operations, persistence across reinit, EC counter equality after 500 cycles | native_sim (simulator only) |
| `ubi_torture` | `tests_ubi_torture.c` | 5 | Bad-block torture, erase retry, degraded transitions | native_sim (simulator only) |
| `ubi_hil_smoke` | `tests_ubi_hil_smoke.c` | 3 | Basic lifecycle, persistence, stress cycles (board-portable smoke) | native_sim |
| `ubi_concurrency` | `tests_ubi_concurrency.c` | 5 | Multi-threaded readers/writers, deinit quiescence, partition guard | native_sim |
| `ubi_erased_val` | `tests_ubi_erased_val.c` | 6 | Erased-value helper unit tests (`ubi_buf_is_erased` with 0xFF, 0x00, mixed), `ubi_get_erased_val` integration, init regression | native_sim |
| `ubi_mutation_gate` | `tests_ubi_mutation_gate.c` | 5 | Central mutation gate: write-shutdown blocks all mutators, degraded mode blocks reserved-metadata only, runtime PEB corruption recovered transparently, runtime degradation sets flag and blocks mutations, erase_peb recovers reserved bank and clears flag | native_sim |
| `ubi_vol_id_watermark` | `tests_ubi_vol_id_watermark.c` | 4 | Persistent vol_id high-watermark: same-boot reuse prevention, cross-reboot persistence, slot re-indexing stability, overflow fail-closed | native_sim |
| **Total (plain)** | | **269** | | |

### Secure Backend Parity Tests

All secure tests require `CONFIG_UBI_CRYPTO=y` and run with a PSA-imported test root key.

| Suite | File | Tests | Focus | Environment |
|-------|------|------:|-------|-------------|
| `ubi_secure_api` | `tests_ubi_secure_api.c` | 7 | Crypto type sizes, secure format on blank, plain unaffected by secure types, runtime backend selection sanity, getter `write_active_key_version` | native_sim |
| `ubi_secure_attach` | `tests_ubi_secure_attach.c` | 10 | Format/attach, mode mismatch, freshness rejection, NULL callbacks, allowlist validation, allowlist duplicates rejected, requested write_kv downgrade rejected | native_sim |
| `ubi_secure_device` | `tests_ubi_secure_device.c` | 2 | Secure init/deinit, info sane, reboot persistence | native_sim |
| `ubi_secure_volumes` | `tests_ubi_secure_volumes.c` | 8 | Create, remove, resize, shrink, multi-volume persistence, vid_counter_floor reconstruction (parity with `ubi_volumes`) | native_sim |
| `ubi_secure_write_read` | `tests_ubi_secure_write_read.c` | 3 | Single/multi LEB write/read, overwrite (parity with `ubi_write_read`) | native_sim |
| `ubi_secure_map` | `tests_ubi_secure_map_unmap.c` | 4 | Map/unmap lifecycle, dirty PEB accounting, unmap→reboot persistence, unmap→erase→reboot (parity with `ubi_map`) | native_sim |
| `ubi_secure_erase` | `tests_ubi_secure_erase.c` | 4 | Fill-unmap-erase cycle, anchor wear-leveling migration, stale-anchor rejection after reboot, reclaim continuity witness preservation (parity with `ubi_erase`) | native_sim |
| `ubi_secure_mixed` | `tests_ubi_secure_mixed.c` | 1 | Multi-volume create/write/remove/resize/map/reboot (parity with `ubi_mixed`) | native_sim |
| `ubi_secure_tamper` | `tests_ubi_secure_tamper.c` | 2 | LEB data tampering smoke, reserved PEB tampering smoke | native_sim |
| `ubi_secure_replay` | `tests_ubi_secure_replay.c` | 3 | Replay of authentic EC/VID/LEB record to a different physical PEB — parent-child AAD binding (`peb_index` in AAD) rejects forgery: EC/VID replay marks destination PEB bad on reattach, LEB replay raises `AUTH_FAILURE` on read | native_sim |
| `ubi_secure_coexistence` | `tests_ubi_secure_coexistence.c` | 2 | Plain UBI device on `ubi_partition` and secure UBI device on `ubi_partition_2` operate concurrently without cross-talk; partition guard still rejects double attach with `-EBUSY` (requires `ubi_partition_2` in DT, native_sim only) | native_sim |
| `ubi_secure_runtime_policy` | `tests_ubi_secure_runtime_policy.c` | 26 | Sticky crypto read-only, event escalation, freshness sync cadence, sync failure events, reads in read-only, per-domain (LEB / DEV / VOL / EC / VID) budget SOON/NOW thresholds, budget reset on key-rotation reattach, KEY_RETIRABLE, allowlist reject, missing key, rollback mismatch, sticky read-only reinit, mixed-key rotation, forced rekey with stale free/dirty/mapped objects | native_sim |
| `ubi_secure_recovery` | `tests_ubi_secure_recovery.c` | 9 | Interrupted data write COW preservation, interrupted VID commit, first-write-leaves-unmapped, reboot after partial write, interrupted anchor rewrite continuity, reserved generation replay rejection, interrupted reserved PEB commit, interrupted anchor creation during volume create, init-time anchor re-creation for orphaned volumes | native_sim |
| `ubi_secure_chunked` | `tests_ubi_secure_chunked.c` | 12 | Chunked LEB geometry, geometry reject, single/multi-chunk write/read, partial reads within and across chunk boundaries, last-chunk padding, reboot persistence, overwrite, chunk tamper isolation, zero-length map fallback, 48-bit AEAD counter overflow rejection (requires `CONFIG_UBI_CRYPTO_LEB_CHUNKED=y`) | native_sim |
| `ubi_secure_forensic` | `tests_ubi_secure_forensic.c` | 6 | Portable forensic scan: plaintext data absence, volume name absence, key material absence, post-overwrite+erase absence, negative test validates scanner on plain backend, magic-prefix sweep | native_sim |
| `ubi_secure_error_handling` | `tests_ubi_secure_error_handling.c` | 74 | Secure-side parity for plain `ubi_error_handling`: NULL/out-of-range params, no-space, contract tests, corrupt EC/VID auth headers, reserved PEB corruption, degraded recovery, LEB edge cases | native_sim |
| `ubi_secure_fault_injection` | `tests_ubi_secure_fault_injection.c` | 6 | Transactional safety on secure backend under malloc/flash faults during create/write; per-kind allocator fault selector and SCRATCH-allocation faults on `leb_read/write` (requires `CONFIG_UBI_TEST_FAULT_INJECTION=y`) | native_sim |
| `ubi_secure_mutation_gate` | `tests_ubi_secure_mutation_gate.c` | 1 | Central mutation gate sanity on secure backend (write-shutdown blocks mutators with `-EROFS`) | native_sim |
| `ubi_secure_vol_id_watermark` | `tests_ubi_secure_vol_id_watermark.c` | 3 | Persistent vol_id high-watermark on secure backend (parity with plain `ubi_vol_id_watermark`) | native_sim |
| `ubi_secure_coverage` | `tests_ubi_secure_coverage.c` | 16 | Branch-coverage-driven secure-path tests: dirty PEB accounting, LEB overwrite recovery and persistence, leb_get_size, map/unmap lifecycle, volume-remove variants, flash-write fault on EC/VID, erase-all-dirty | native_sim |
| `ubi_secure_crypto_faults` | `tests_ubi_secure_crypto_faults.c` | 18 | Crypto primitive fault hooks (AEAD encrypt/decrypt fail, RNG fail, HKDF fail) inserted via `ubi_secure_test_hook_set` exercise error propagation on init/write/read paths (requires `CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION=y`) | native_sim |
| `ubi_secure_defensive` | `tests_ubi_secure_defensive.c` | 109 | Defensive header-shape validation: `bad_wrapper_version`, `exceeds_ccm_limit`, malformed prefix/AAD/tag fields across EC/VID/LEB records | native_sim |
| **Total (secure)** | | **321** | | |

> Counts above are for the `secure` build (`CONFIG_UBI_CRYPTO=y`,
> `CONFIG_UBI_CRYPTO_LEB_CHUNKED=n`).  The `chunked` build adds the
> `ubi_secure_chunked` suite (12 ZTEST).  Plain tests from the section
> above are also linked into the secure build (so the secure
> binary executes 251 + 311 = 562 ZTEST in total, 41 suites).

## What native_sim Proves vs. What Hardware Proves

| Aspect | native_sim (simulator) | Hardware (b_u585i_iot02a, nrf5340dk) |
|--------|----------------------|--------------------------------------|
| Functional correctness | Full — all 562 tests run (560 pass, 2 skip, 41 suites) in the `secure` config; the `plain` and `chunked` configs run subsets | Build verification only (CI cross-compiles) |
| Flash timing / latency | Not representative | Realistic |
| Power-loss behavior | Not tested (simulator has no power-loss model) | Not currently tested (no HIL power-loss setup) |
| Bad block behavior | Simulated via `CONFIG_FLASH_SIMULATOR` flags | Real flash errors (rare on NOR) |
| Wear-leveling distribution | Verified via erase counter checks | Observable but not systematically tested |
| Memory footprint | Approximate (host allocator) | Precise (Cortex-M33 heap) |

## Test Categories

### 1. Unit / Functional Tests

Core API verification organized by functional area:

| Suite | File | Focus |
|-------|------|-------|
| `ubi_device` | `tests_ubi_device.c` | Device init, deinit, get_info |
| `ubi_volumes` | `tests_ubi_volumes.c` | Volume create, remove, resize, get_info |
| `ubi_map_unmap` | `tests_ubi_map_unmap.c` | LEB map, unmap, is_mapped |
| `ubi_write_read` | `tests_ubi_write_read.c` | LEB write, read, get_size |
| `ubi_erase` | `tests_ubi_erase.c` | PEB erase, dirty-to-free recycling |
| `ubi_mixed` | `tests_ubi_mixed.c` | Multi-volume and cross-functional workflows |

### 2. Error Handling Tests

| Suite | File | Focus |
|-------|------|-------|
| `ubi_error_handling` | `tests_ubi_error_handling.c` | NULL parameters, out-of-range LEB numbers, no-space conditions, resize edge cases, overwrite semantics, no-volumes paths, contract tests (idempotent unmap, no-op map, static volume write, invalid type, zero LEBs, capacity accounting), corrupt EC/VID headers, reserved PEB corruption, degraded recovery, LEB edge cases, wrong vol_id removal |

### 3. Boundary Tests

| Suite | File | Focus |
|-------|------|-------|
| `ubi_boundary` | `tests_ubi_boundary.c` | Max LEB capacity writes, exceeds-capacity errors, exact read offsets, alignment boundaries, sub-alignment writes |

### 4. Recovery / Corruption Tests

| Suite | File | Focus |
|-------|------|-------|
| `ubi_recovery` | `tests_ubi_recovery.c` | Corrupt EC header -> bad PEB, corrupt VID CRC -> bad PEB, valid EC + erased VID + erased data -> free PEB, valid EC + erased VID + non-erased data -> dirty PEB (uncommitted write), orphan vol_id -> dirty PEB, duplicate LEB sqnum conflict resolution, erase_peb no-op when clean, dual-bank recovery during resize, degraded-mode blocking, multi-volume recovery, reinit after interrupted commit preserves old data |

### 5. Fault Injection Tests

| Suite | File | Focus |
|-------|------|-------|
| `ubi_fault_injection` | `tests_ubi_fault_injection.c` | Transactional safety: malloc failure during create (P0.2), COW overwrite preserves old data on failure (P0.7), invariant checker after create/write/remove and resize/shrink cycles. Requires `CONFIG_UBI_TEST_FAULT_INJECTION=y`. |

### 5b. I/O Fault Injection Tests

| Suite | File | Focus |
|-------|------|-------|
| `ubi_io_faults` | `tests_ubi_io_faults.c` | Systematic malloc-failure sweeps (init with no volumes, with volume, with orphans, with duplicates, with bad VID CRC, with bad EC), scratch alloc faults, diag alloc fault, volume create/remove alloc sweeps, flash erase failure -> bad PEB transition, commit-order tests (VID write failure after data write preserves old mapping; unmapped LEB stays unmapped on VID failure). Requires `CONFIG_UBI_TEST_FAULT_INJECTION=y`. |

### 5c. Init Error Tests

| Suite | File | Focus |
|-------|------|-------|
| `ubi_init_errors` | `tests_ubi_init_errors.c` | Geometry validation (erase block size, write block size, partition size), partition guard (`-EBUSY`), format error paths, corrupt EC/VID headers at init time, PEB classification under various corruption scenarios. |

### 6. Stress Tests (simulator only)

| Suite | File | Focus |
|-------|------|-------|
| `ubi_stress` | `tests_ubi_stress.c` | Full volume utilization, init-deinit cycling, PEB wear leveling, multi-volume concurrent usage |
| `ubi_stress_longrun` | `tests_ubi_stress_longrun.c` | Randomized churn with reboots, multi-volume mixed operations, persistence across reinit, EC counter equality after 500 cycles |

### 7. Torture Tests (simulator only)

| Suite | File | Focus |
|-------|------|-------|
| `ubi_torture` | `tests_ubi_torture.c` | Bad-block torture, erase retry logic, degraded-mode transitions |

### 8. HIL Smoke Tests

| Suite | File | Focus |
|-------|------|-------|
| `ubi_hil_smoke` | `tests_ubi_hil_smoke.c` | Board-portable smoke: basic lifecycle, persistence across reinit, stress cycles. Runs on both native_sim and hardware targets. |

### 9. Concurrency Tests

| Suite | File | Focus |
|-------|------|-------|
| `ubi_concurrency` | `tests_ubi_concurrency.c` | Multi-threaded concurrent readers, reader-writer interleave, deinit-after-quiescence, double-init partition guard (`-EBUSY`), init-after-deinit reuse. Uses `k_thread_create` + `k_thread_join`. |

### 10. Mutation Gate Tests

| Suite | File | Focus |
|-------|------|-------|
| `ubi_mutation_gate` | `tests_ubi_mutation_gate.c` | Central mutation gate: write-shutdown flag blocks all 7 public mutators (`-EROFS`) while read-only operations succeed; degraded-mode entry blocks reserved-metadata mutators only (skips on simulator when recovery succeeds); runtime reserved PEB corruption triggers transparent recovery during next volume operation; runtime degradation (corrupt PEB + erase fault) sets `read_only_degraded` flag and blocks subsequent mutations; `erase_peb` recovers degraded reserved PEB bank and clears the flag. Requires `CONFIG_UBI_TEST_API_ENABLE=y`. |

### 11. Volume ID Watermark Tests

| Suite | File | Focus |
|-------|------|-------|
| `ubi_vol_id_watermark` | `tests_ubi_vol_id_watermark.c` | Persistent vol_id high-watermark: same-boot reuse prevention (create, remove, create → new id), cross-reboot persistence (create, remove, reinit, create → watermark survives), slot re-indexing stability (remove middle volume, remaining vol_ids unchanged), overflow fail-closed (vol_id_watermark = UINT32_MAX → `-ENOSPC`). |

## Test Environment

### Primary: `native_sim`

- **Platform**: Zephyr `native_sim` board with flash simulator
- **Config**: `CONFIG_FLASH_SIMULATOR=y`, `CONFIG_FLASH_SIMULATOR_DOUBLE_WRITES=y`, `CONFIG_FLASH_SIMULATOR_EXPLICIT_ERASE=y`
- **Usage**: All test suites run here; stress and torture tests are simulator-only

### Flash Geometry Variants

All functional and stress tests run against three flash geometries to exercise
different erase-block sizes, write-block alignment requirements, and partition
capacities. The geometry is selected via DTC overlay files in `tests/boards/`.

| Variant | Overlay file | Erase block | Write block | Partition | PEB count |
|---------|-------------|------------:|------------:|----------:|----------:|
| Default | `native_sim.overlay` | 8192 B | 1 B | 128 KB | 16 |
| nRF5340 | `native_sim_nrf5340.overlay` | 4096 B | 4 B | 64 KB | 16 |
| STM32U5 | `native_sim_stm32u5.overlay` | 8192 B | 16 B | 128 KB | 16 |

**Running locally with a geometry overlay:**

```bash
# nRF5340 geometry
west build -p always -b native_sim ubi/tests -- \
  -DDTC_OVERLAY_FILE="boards/native_sim_nrf5340.overlay"
./build/zephyr/zephyr.exe

# STM32U5 geometry
west build -p always -b native_sim ubi/tests -- \
  -DDTC_OVERLAY_FILE="boards/native_sim_stm32u5.overlay"
./build/zephyr/zephyr.exe
```

**CI:** The `native-tests` job matrix includes `geometry: [default, nrf5340, stm32u5]`,
producing 6 jobs (2 mem backends × 3 geometries).

**Twister:** `testcase.yaml` contains 16 geometry-specific scenarios using
`extra_dtc_overlay_files` (8 per geometry variant).

### Secondary: `b_u585i_iot02a`

- **Platform**: STM32U585AI hardware target (8 KB erase blocks, 128 KB UBI partition)
- **Usage**: Cross-compilation verification (build only in CI)

### Secondary: `nrf5340dk/nrf5340/cpuapp`

- **Platform**: Nordic nRF5340 DK, application core (4 KB erase blocks, 64 KB UBI partition)
- **Usage**: Cross-compilation verification (build only in CI)

## Coverage

### Targets

| Metric | Target | Achieved (v0.71.0) |
|--------|--------|--------------------|
| Line coverage | >= 80% | 85.0% plain (1670/1964), 77.3% secure (3572/4623) |
| Branch coverage | >= 70% | 51.8% plain (844/1628), 40.7% secure (1554/3817) |

### Tooling

- **Instrumentation**: Zephyr `CONFIG_COVERAGE=y` + `CONFIG_COVERAGE_GCOV=y`
- **Collection**: `lcov --capture` filtered to `lib/src/*`
- **Report**: `genhtml` with `--branch-coverage`

### Running Locally

```bash
bash scripts/coverage.sh
```

HTML report is generated at `build/coverage/html/index.html`.

### CI

The `coverage` job in `.github/workflows/ci.yml` builds with coverage, runs tests,
collects lcov data, and uploads the HTML report as a build artifact.

## Test Patterns

### Fixture Pattern

Each test suite uses the standard ZTest fixture. Shared helpers in `tests/src/` eliminate boilerplate:

- **`ubi_test_fixture.h`** — `ubi_test_setup_mtd()`, `ubi_test_erase_partition()`, `ubi_test_init_device()`, `ubi_test_reinit_device()`
- **`ubi_test_memory.h`** — `ubi_test_memory_snapshot()`, `ubi_test_memory_check_no_leak()` (requires `CONFIG_SYS_HEAP_RUNTIME_STATS`)
- **`ubi_test_raw_flash.h`** — `ubi_test_raw_write_ec_hdr()`, `ubi_test_raw_write_vid_hdr()` for corruption injection

```c
#include "ubi_test_fixture.h"

static struct ubi_flash_desc flash = { 0 };

static void *ztest_suite_setup(void) {
    ubi_test_setup_mtd(&flash);
    return NULL;
}

static void ztest_testcase_before(void *ctx) {
    (void)ctx;
    ubi_test_erase_partition();
}

ZTEST_SUITE(suite_name, NULL, ztest_suite_setup, ztest_testcase_before,
            ztest_testcase_teardown, NULL);
```

Every test starts with a fully erased flash partition, ensuring isolation.

### Corruption Testing Pattern

Recovery tests use raw `flash_area_write()` to inject corrupt headers:

1. Initialize UBI normally (creates device/volume headers on reserved PEBs)
2. Deinit
3. Directly write corrupt EC or VID headers on data PEBs
4. Re-init and verify PEB classification (`bad_peb_count`, `dirty_peb_count`, etc.)

### Compile Flags

Tests build with strict warnings to catch issues at compile time:

```
-Werror -Wextra -Wshadow -Wdouble-promotion -Wformat=2
-Wnull-dereference -Wunused -Wno-unused-parameter
```

## Known Gaps

| Gap | Impact | Mitigation |
|-----|--------|------------|
| Partial flash write failure coverage | Flash write fault injection covers `flash_write_with_retry`; not all write call-sites are individually swept | `ubi_io_faults` suite validates erase failure -> bad PEB; write retry logic is code-reviewed |
| HIL smoke only (no CI hardware) | `ubi_hil_smoke` suite exists but CI only cross-compiles for STM32U5 and nRF5340 | Manual hardware testing during development; HIL CI planned |
| Non-0xFF erased value end-to-end | Erased-value helpers are unit-tested for 0x00, but the flash simulator only supports 0xFF | Helpers are trivial; integration tests on 0xFF backend cover the full scan path |
| Secure tamper detection precision | Tamper tests verify no-crash and graceful handling but do not assert specific AUTH_FAILURE events (depends on PEB layout) | Forensic scan suite (`ubi_secure_forensic`) verifies absence of plaintext |
| Secure data size limit | Secure LEB read requires scratch memory for decryption (`ct_tag_size + data_size`); data exceeding ~248 bytes may exceed default scratch budget (512 bytes) | Use data within scratch budget in tests; increase `CONFIG_UBI_MAX_NR_OF_VOLUMES` to enlarge scratch, or use chunked mode |

## How to Add a New Test

1. Choose the appropriate test file based on test category (see executive summary table)
2. Add a `ZTEST(suite_name, test_name)` function
3. Follow the fixture pattern: init -> operate -> assert -> deinit
4. If testing a new file, add it to `tests/CMakeLists.txt`
5. Build and run: `bash scripts/run_tests.sh`

## ZTEST traceability for Secure UBI

The tables below map each Secure UBI lifecycle step, recovery
scenario, and release-checklist item to the specific ZTESTs that
exercise it. All tests live under `tests/src/secure/` and run as part
of the secure ZTEST suite (`bash scripts/run_tests.sh native_sim
secure`). Items marked **review-only** are design / code-review
constraints with no direct runtime test (intentional; documented for
traceability).

The corresponding normative behaviour is specified in
{doc}`/reference/onflash_format_spec` chapters 19 (Secure volume lifecycle), 20
(Secure recovery scenarios), and 21 (Runtime policy).

### Lifecycle step → ZTEST coverage

| Lifecycle step       | Implementation                                                       | ZTEST(s)                                                                                                                                        |
|----------------------|----------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------|
| Create               | `ubi_secure_volume_create()` (`lib/src/secure/ubi_secure_volume.c`)  | `ubi_secure_volumes::create_one_with_reboot`, `ubi_secure_volumes::create_many_with_reboot`                                           |
| Resize (grow)        | `ubi_secure_volume_resize()` (grow branch)                           | `ubi_secure_volumes::resize_upper_with_reboot`, `ubi_secure_coverage::volume_resize_grow`, `ubi_secure_coverage::volume_resize_persists` |
| Shrink               | `ubi_secure_volume_resize()` (shrink branch)                         | `ubi_secure_volumes::shrink_with_reboot`, `ubi_secure_volumes::shrink_erase_reboot`, `ubi_secure_coverage::volume_resize_shrink` |
| Remove               | `ubi_secure_volume_remove()`                                         | `ubi_secure_coverage::volume_remove_basic`, `ubi_secure_coverage::volume_remove_persists`, `ubi_secure_coverage::volume_remove_with_mapped_lebs`, `ubi_secure_volumes::create_remove_with_reboot` |
| Unmap                | `ubi_secure_leb_unmap()` (`lib/src/secure/ubi_secure_leb.c`)         | `ubi_secure_coverage::leb_unmap`, `ubi_secure_map::unmap_reboot_before_erase`, `ubi_secure_map::unmap_erase_reboot`              |
| Erase and reclaim    | `ubi_secure_device_erase_peb()` + anchor witness (§11.6)            | `ubi_secure_erase::anchor_participates_in_wear_leveling`, `ubi_secure_erase::reclaim_preserves_continuity_witness`, `ubi_secure_erase::fill_unmap_erase_cycle` |
| Reboot recovery      | `ubi_secure_device_init()` scan path (`lib/src/secure/ubi_core_init.c`) | `ubi_secure_map::all_lebs_lifecycle_with_reboot`, `ubi_secure_recovery::interrupted_data_write_survives_reboot`, `ubi_secure_recovery::init_recreates_missing_anchor` |

### Recovery scenario → ZTEST coverage

| Scenario                                   | ZTEST(s)                                                                                                                                                              |
|--------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `unmap → reboot` (before erase)            | `ubi_secure_map::unmap_reboot_before_erase`                                                                                                                      |
| `unmap → erase → reboot`                   | `ubi_secure_map::unmap_erase_reboot`                                                                                                                             |
| `shrink → reboot` (before erase)           | `ubi_secure_volumes::shrink_with_reboot`                                                                                                                         |
| `shrink → erase → reboot`                  | `ubi_secure_volumes::shrink_erase_reboot`                                                                                                                        |
| `remove all volumes → reboot → create`     | `ubi_secure_volumes::vid_counter_floor_remove_create_reboot`                                                                                                     |
| Anchor migration during erase              | `ubi_secure_erase::anchor_participates_in_wear_leveling`, `ubi_secure_erase::reclaim_preserves_continuity_witness`, `ubi_secure_erase::stale_anchor_rejected_after_reboot`, `ubi_secure_recovery::interrupted_anchor_write_preserves_continuity` |
| Per-volume LEB floor continuity at runtime | `ubi_secure_volumes::vid_counter_floor_persists`, `ubi_secure_volumes::vid_counter_floor_remove_create_reboot`, `ubi_secure_coverage::leb_overwrite_counter_recovery` |
| Stale anchor after reboot                  | `ubi_secure_erase::stale_anchor_rejected_after_reboot`, `ubi_secure_recovery::init_recreates_missing_anchor`                                                |
| Emergency reserve refill                   | `ubi_secure_erase::fill_unmap_erase_cycle`                                                                                                                       |
| Dual-bank reserved metadata recovery       | `ubi_secure_recovery::interrupted_reserved_commit_no_ghost_volume`, `ubi_secure_recovery::reserved_generation_replay_rejected`, `ubi_secure_tamper::reserved_peb_tamper_smoke` |

### Release checklist → ZTEST coverage

#### Critical format constraints

| Checklist bullet                                                         | ZTEST(s)                                                                                                                                                            |
|--------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Reserved-generation fit against geometry                                 | `ubi_secure_api::reserved_generation_fit_guard_rejects_small_eb`, `ubi_secure_defensive::init_erase_block_too_small`                                      |
| Single-tag CCM payload limit / require chunked or reject SECURE          | `ubi_secure_chunked::geometry_reject_tiny_erase_block`, `ubi_secure_chunked::geometry_leb_size`, `ubi_secure_chunked::chunked_write_overflow_rejected` |
| Zero-length LEB encoding fixed                                           | `ubi_secure_chunked::zero_length_map`                                                                                                                          |
| Single-tag tail-padding behaviour fixed                                  | `ubi_secure_chunked::partial_last_chunk`, `ubi_secure_chunked::overwrite_chunked`                                                                         |
| Reject cross-mode attach; mixed-mode migration out of scope              | `ubi_secure_attach::plain_then_secure_mismatch`, `ubi_secure_attach::secure_then_plain_mismatch`, `ubi_secure_coexistence::partition_guard_blocks_double_attach` |

#### Important implementation notes

| Checklist bullet                                                         | ZTEST(s)                                                                                                                                                            |
|--------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Operational retirement levels (§13.8)                                    | `ubi_secure_runtime_policy::reserved_metadata_budget_exhausts_blocks_until_rotation`, `ubi_secure_runtime_policy::leb_budget_exhausts_blocks_until_rotation`, `ubi_secure_runtime_policy::leb_budget_rotate_soon_emitted_below_now` |
| Authenticated parent `key_version` in every child AAD binding            | `ubi_secure_defensive::derive_domain_key_rejects_non_allowlisted_kv`, `ubi_secure_defensive::derive_leb_key_rejects_non_allowlisted_kv`                   |
| Zeroize plaintext scratch and software-derived child-key buffers         | review-only (audited in `ubi_secure_crypto.c`; relies on `psa_destroy_key` + scratch slab `memset`)                                                                 |
| `volume_id` as durable cryptographic identity                            | `ubi_secure_vol_id_watermark::volume_id_not_reused_after_remove_and_reinit`, `ubi_secure_vol_id_watermark::volume_id_not_reused_after_remove_same_boot`   |
| Authenticated `write_active_key_version` monotonic                       | `ubi_secure_attach::requested_write_kv_downgrade_rejected`, `ubi_secure_api::get_write_active_kv_after_format_and_rotation`                               |
| `device_revision` widening preserves on-flash numeric ordering           | review-only (locked by `BUILD_ASSERT` on serialized layout in `ubi_secure_ser.c`)                                                                                   |

#### Validation expected before upstream

| Checklist bullet                                                         | ZTEST(s)                                                                                                                                                            |
|--------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Power-cut testing: `DATA → VID`, zero-length, hidden-anchor, reserved-gen | `ubi_secure_recovery::interrupted_data_write_preserves_old_mapping`, `ubi_secure_recovery::interrupted_vid_commit_preserves_old_mapping`, `ubi_secure_recovery::interrupted_anchor_write_preserves_continuity`, `ubi_secure_recovery::interrupted_reserved_commit_no_ghost_volume`, `ubi_secure_recovery::reserved_generation_replay_rejected` |
| PSA-only failure paths (RNG fail, missing key material)                  | `ubi_secure_crypto_faults::rng_fail_on_leb_write`, `ubi_secure_crypto_faults::rng_fail_on_erase`, `ubi_secure_crypto_faults::get_key_id_fail_on_leb_write`, `ubi_secure_crypto_faults::hkdf_fail_on_leb_write`, `ubi_secure_attach::freshness_reject` |
| Zero-length, mixed-key, chunked, alignment, hidden-anchor recovery       | `ubi_secure_chunked::zero_length_map`, `ubi_secure_chunked::multi_chunk_with_reboot`, `ubi_secure_chunked::partial_read_cross_chunk`, `ubi_secure_recovery::interrupted_anchor_create_during_volume_create` |
| `unmap/shrink → erase → reboot` with last-witness PEB                    | `ubi_secure_map::unmap_erase_reboot`, `ubi_secure_volumes::shrink_erase_reboot`, `ubi_secure_erase::reclaim_preserves_continuity_witness`            |
| `volume_remove` including remove-all → reboot → zero-volume state        | `ubi_secure_coverage::volume_remove_basic`, `ubi_secure_coverage::volume_remove_persists`, `ubi_secure_volumes::vid_counter_floor_remove_create_reboot` |
| `vid_next_counter_floor` reconstruction; monotonic write-active-key      | `ubi_secure_volumes::vid_counter_floor_remove_create_reboot`, `ubi_secure_attach::requested_write_kv_downgrade_rejected`                                  |
| Refcount-driven `KEY_RETIRABLE` during reclaim and rotation              | `ubi_secure_runtime_policy::forced_rekey_with_stale_objects`, `ubi_secure_runtime_policy::reserved_refcount_no_spurious_key_retirable`                    |
