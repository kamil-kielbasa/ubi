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
| `ubi_error_handling` | `tests_ubi_error_handling.c` | 62 | NULL params, out-of-range, no-space, contract tests (idempotent unmap, no-op map, static write, invalid type, zero LEBs, capacity) | native_sim |
| `ubi_boundary` | `tests_ubi_boundary.c` | 6 | Max LEB capacity, alignment, sqnum persistence | native_sim |
| `ubi_recovery` | `tests_ubi_recovery.c` | 21 | Corruption, dual-bank, sqnum conflicts, degraded mode | native_sim |
| `ubi_fault_injection` | `tests_ubi_fault_injection.c` | 4 | Transactional safety under allocation failures, COW overwrite, invariant checks | native_sim |
| `ubi_stress` | `tests_ubi_stress.c` | 4 | Full utilization, init cycling, wear leveling | native_sim (simulator only) |
| `ubi_stress_longrun` | `tests_ubi_stress_longrun.c` | 3 | Randomized churn with reboots, multi-volume operations, persistence across reinit | native_sim (simulator only) |
| `ubi_torture` | `tests_ubi_torture.c` | 5 | Bad-block torture, erase retry, degraded transitions | native_sim (simulator only) |
| `ubi_hil_smoke` | `tests_ubi_hil_smoke.c` | 3 | Basic lifecycle, persistence, stress cycles (board-portable smoke) | native_sim |
| `ubi_concurrency` | `tests_ubi_concurrency.c` | 5 | Multi-threaded readers/writers, deinit quiescence, partition guard | native_sim |
| **Total** | | **132** | | |

## What native_sim Proves vs. What Hardware Proves

| Aspect | native_sim (simulator) | Hardware (b_u585i_iot02a) |
|--------|----------------------|--------------------------|
| Functional correctness | Full — all 132 tests run (15 suites) | Build verification only (CI cross-compiles) |
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
| `ubi_error_handling` | `tests_ubi_error_handling.c` | NULL parameters, out-of-range LEB numbers, no-space conditions, resize edge cases, overwrite semantics, no-volumes paths, contract tests (idempotent unmap, no-op map, static volume write, invalid type, zero LEBs, capacity accounting) |

### 3. Boundary Tests

| Suite | File | Focus |
|-------|------|-------|
| `ubi_boundary` | `tests_ubi_boundary.c` | Max LEB capacity writes, exceeds-capacity errors, exact read offsets, alignment boundaries, sub-alignment writes |

### 4. Recovery / Corruption Tests

| Suite | File | Focus |
|-------|------|-------|
| `ubi_recovery` | `tests_ubi_recovery.c` | Corrupt EC header -> bad PEB, corrupt VID CRC -> bad PEB, valid EC + empty VID -> free PEB, orphan vol_id -> dirty PEB, duplicate LEB sqnum conflict resolution, erase_peb no-op when clean |

### 5. Fault Injection Tests

| Suite | File | Focus |
|-------|------|-------|
| `ubi_fault_injection` | `tests_ubi_fault_injection.c` | Transactional safety: malloc failure during create (P0.2), COW overwrite preserves old data on failure (P0.7), invariant checker after create/write/remove and resize/shrink cycles. Requires `CONFIG_UBI_TEST_FAULT_INJECTION=y`. |

### 6. Stress Tests (simulator only)

| Suite | File | Focus |
|-------|------|-------|
| `ubi_stress` | `tests_ubi_stress.c` | Full volume utilization, init-deinit cycling, PEB wear leveling, multi-volume concurrent usage |
| `ubi_stress_longrun` | `tests_ubi_stress_longrun.c` | Randomized churn with reboots, multi-volume mixed operations, persistence across reinit |

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

## Test Environment

### Primary: `native_sim`

- **Platform**: Zephyr `native_sim` board with flash simulator
- **Erase block**: 8192 bytes (matches STM32U5 geometry)
- **Partition**: 128 KB at offset 0x0 (`ubi_partition`)
- **Config**: `CONFIG_FLASH_SIMULATOR=y`, `CONFIG_FLASH_SIMULATOR_DOUBLE_WRITES=y`, `CONFIG_FLASH_SIMULATOR_EXPLICIT_ERASE=y`
- **Usage**: All test suites run here; stress and torture tests are simulator-only

### Secondary: `b_u585i_iot02a`

- **Platform**: STM32U585AI hardware target
- **Usage**: Cross-compilation verification (build only in CI)

## Coverage

### Targets

| Metric | Target |
|--------|--------|
| Line coverage | >= 80% |
| Branch coverage | >= 70% |

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
| No real flash erase failures | `ubi_device_erase_peb` bad-block torture path (`-ENOSYS`) not exercised | Torture tests use simulator flags; path is code-reviewed |
| Partial heap exhaustion coverage | `k_malloc` failure hook (`CONFIG_UBI_TEST_FAULT_INJECTION`) covers volume create; not yet wired through all allocation sites | `ubi_fault_injection` suite validates transactional safety for create; remaining paths are code-reviewed |
| HIL smoke only (no CI hardware) | `ubi_hil_smoke` suite exists but CI only cross-compiles for STM32U5 | Manual hardware testing during development; HIL CI planned |

## How to Add a New Test

1. Choose the appropriate test file based on test category (see executive summary table)
2. Add a `ZTEST(suite_name, test_name)` function
3. Follow the fixture pattern: init -> operate -> assert -> deinit
4. If testing a new file, add it to `tests/CMakeLists.txt`
5. Build and run: `bash scripts/run_tests.sh`
