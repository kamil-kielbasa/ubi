# Test Strategy

**What this page covers:** Test categories, environments, coverage targets, test patterns, and known gaps.

**Prerequisites:** [Getting Started](getting_started.md) for build and run instructions.

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
| `ubi_fault_injection` | `tests_ubi_fault_injection.c` | 4 | Transactional safety under allocation failures, COW overwrite, invariant checks | native_sim |
| `ubi_io_faults` | `tests_ubi_io_faults.c` | 26 | Malloc/flash-write/flash-erase fault sweeps across init, volume, and I/O paths; commit-order fault injection (VID write failure preserves old mapping) | native_sim |
| `ubi_init_errors` | `tests_ubi_init_errors.c` | 33 | Geometry validation, partition guard, format failures, header corruption at init | native_sim |
| `ubi_stress` | `tests_ubi_stress.c` | 4 | Full utilization, init cycling, wear leveling | native_sim (simulator only) |
| `ubi_stress_longrun` | `tests_ubi_stress_longrun.c` | 4 | Randomized churn with reboots, multi-volume operations, persistence across reinit, EC counter equality after 500 cycles | native_sim (simulator only) |
| `ubi_torture` | `tests_ubi_torture.c` | 5 | Bad-block torture, erase retry, degraded transitions | native_sim (simulator only) |
| `ubi_hil_smoke` | `tests_ubi_hil_smoke.c` | 3 | Basic lifecycle, persistence, stress cycles (board-portable smoke) | native_sim |
| `ubi_concurrency` | `tests_ubi_concurrency.c` | 5 | Multi-threaded readers/writers, deinit quiescence, partition guard | native_sim |
| `ubi_erased_val` | `tests_ubi_erased_val.c` | 6 | Erased-value helper unit tests (`ubi_buf_is_erased` with 0xFF, 0x00, mixed), `ubi_get_erased_val` integration, init regression | native_sim |
| `ubi_mutation_gate` | `tests_ubi_mutation_gate.c` | 5 | Central mutation gate: write-shutdown blocks all mutators, degraded mode blocks reserved-metadata only, runtime PEB corruption recovered transparently, runtime degradation sets flag and blocks mutations, erase_peb recovers reserved bank and clears flag | native_sim |
| `ubi_vol_id_watermark` | `tests_ubi_vol_id_watermark.c` | 4 | Persistent vol_id high-watermark: same-boot reuse prevention, cross-reboot persistence, slot re-indexing stability, overflow fail-closed | native_sim |
| **Total (plain)** | | **251** | | |

### Secure Backend Parity Tests

All secure tests require `CONFIG_UBI_CRYPTO=y` and run with a PSA-imported test root key.

| Suite | File | Tests | Focus | Environment |
|-------|------|------:|-------|-------------|
| `ubi_secure_api` | `tests_ubi_secure_api.c` | 3 | Crypto type sizes, secure format on blank, plain unaffected by secure types | native_sim |
| `ubi_secure_attach` | `tests_ubi_secure_attach.c` | 8 | Format/attach, mode mismatch, freshness rejection, NULL callbacks, allowlist validation | native_sim |
| `ubi_secure_device` | `tests_ubi_secure_device.c` | 2 | Secure init/deinit, info sane, reboot persistence | native_sim |
| `ubi_secure_volumes` | `tests_ubi_secure_volumes.c` | 7 | Create, remove, resize, shrink, multi-volume persistence, vid_counter_floor reconstruction (parity with `ubi_volumes`) | native_sim |
| `ubi_secure_write_read` | `tests_ubi_secure_write_read.c` | 3 | Single/multi LEB write/read, overwrite (parity with `ubi_write_read`) | native_sim |
| `ubi_secure_map` | `tests_ubi_secure_map_unmap.c` | 4 | Map/unmap lifecycle, dirty PEB accounting, unmap→reboot persistence, unmap→erase→reboot (parity with `ubi_map`) | native_sim |
| `ubi_secure_erase` | `tests_ubi_secure_erase.c` | 4 | Fill-unmap-erase cycle, anchor wear-leveling migration, stale-anchor rejection after reboot, reclaim continuity witness preservation (parity with `ubi_erase`) | native_sim |
| `ubi_secure_mixed` | `tests_ubi_secure_mixed.c` | 1 | Multi-volume create/write/remove/resize/map/reboot (parity with `ubi_mixed`) | native_sim |
| `ubi_secure_tamper` | `tests_ubi_secure_tamper.c` | 2 | LEB data tampering smoke, reserved PEB tampering smoke | native_sim |
| `ubi_secure_runtime_policy` | `tests_ubi_secure_runtime_policy.c` | 15 | Sticky crypto read-only, event escalation, freshness sync cadence, sync failure events, reads in read-only, budget SOON/NOW thresholds, KEY_RETIRABLE, allowlist reject, missing key, rollback mismatch, sticky read-only reinit, mixed-key rotation | native_sim |
| **Total (secure)** | | **49** | | |

## What native_sim Proves vs. What Hardware Proves

| Aspect | native_sim (simulator) | Hardware (b_u585i_iot02a, nrf5340dk) |
|--------|----------------------|--------------------------------------|
| Functional correctness | Full — all 300 tests run (30 suites) | Build verification only (CI cross-compiles) |
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

| Metric | Target | Achieved (v0.22.0) |
|--------|--------|--------------------|
| Line coverage | >= 80% | 85.2% (1517/1781) (v0.22.0) |
| Branch coverage | >= 70% | 54.2% (754/1390) (v0.22.0) |

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

static struct ubi_mtd mtd = { 0 };

static void *ztest_suite_setup(void) {
    ubi_test_setup_mtd(&mtd);
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
| No power-loss simulation | Interrupted writes and metadata commits not tested | Recovery logic is tested via corruption injection (corrupt headers, duplicate LEBs) |
| Partial flash write failure coverage | Flash write fault injection covers `flash_write_with_retry`; not all write call-sites are individually swept | `ubi_io_faults` suite validates erase failure -> bad PEB; write retry logic is code-reviewed |
| HIL smoke only (no CI hardware) | `ubi_hil_smoke` suite exists but CI only cross-compiles for STM32U5 and nRF5340 | Manual hardware testing during development; HIL CI planned |
| Non-0xFF erased value end-to-end | Erased-value helpers are unit-tested for 0x00, but the flash simulator only supports 0xFF | Helpers are trivial; integration tests on 0xFF backend cover the full scan path |
| Secure tamper detection precision | Tamper tests verify no-crash and graceful handling but do not assert specific AUTH_FAILURE events (depends on PEB layout) | Full tamper detection with forensic scan planned for PR9 |
| Secure data size limit | Secure LEB read requires scratch memory for decryption (`ct_tag_size + data_size`); data exceeding ~248 bytes may exceed default scratch budget (512 bytes) | Use data within scratch budget in tests; increase `CONFIG_UBI_MAX_NR_OF_VOLUMES` to enlarge scratch, or use chunked mode (PR9) |

## How to Add a New Test

1. Choose the appropriate test file based on test category (see executive summary table)
2. Add a `ZTEST(suite_name, test_name)` function
3. Follow the fixture pattern: init -> operate -> assert -> deinit
4. If testing a new file, add it to `tests/CMakeLists.txt`
5. Build and run: `bash scripts/run_tests.sh`
