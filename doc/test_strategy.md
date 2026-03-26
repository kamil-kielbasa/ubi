# UBI Test Strategy

## Scope

This document describes the testing strategy for the UBI (Unsorted Block Images) library,
covering test categories, environments, coverage targets, tooling, and patterns used
across the test suite.

## Test Categories

### 1. Unit / Functional Tests

Core API verification organized by functional area:

| Suite | File | Focus |
|-------|------|-------|
| `ubi_device` | `tests_ubi_device.c` | Device init, deinit, get_info |
| `ubi_volumes` | `tests_ubi_volumes.c` | Volume create, remove, resize, get_info |
| `ubi_map_unmap` | `tests_ubi_map_unmap.c` | LEB map, unmap, is_mapped |
| `ubi_write_read` | `tests_ubi_write_read.c` | LEB write, read, get_size |
| `ubi_erase` | `tests_ubi_erase.c` | PEB erase, dirty→free recycling |
| `ubi_mixed` | `tests_ubi_mixed.c` | Multi-volume and cross-functional workflows |

### 2. Error Handling Tests

| Suite | File | Focus |
|-------|------|-------|
| `ubi_error_handling` | `tests_ubi_error_handling.c` | NULL parameters, out-of-range LEB numbers, no-space conditions, resize edge cases, overwrite semantics, no-volumes paths |

### 3. Boundary Tests

| Suite | File | Focus |
|-------|------|-------|
| `ubi_boundary` | `tests_ubi_boundary.c` | Max LEB capacity writes, exceeds-capacity errors, exact read offsets, alignment boundaries, sub-alignment writes |

### 4. Recovery / Corruption Tests

| Suite | File | Focus |
|-------|------|-------|
| `ubi_recovery` | `tests_ubi_recovery.c` | Corrupt EC header → bad PEB, corrupt VID CRC → bad PEB, valid EC + empty VID → free PEB, orphan vol_id → dirty PEB, duplicate LEB sqnum conflict resolution, erase_peb no-op when clean |

### 5. Stress Tests (simulator only)

| Suite | File | Focus |
|-------|------|-------|
| `ubi_stress` | `tests_ubi_stress.c` | Full volume utilization, init-deinit cycling, PEB wear leveling, multi-volume concurrent usage |

## Test Environment

### Primary: `native_sim`

- **Platform**: Zephyr `native_sim` board with flash simulator
- **Erase block**: 8192 bytes (matches STM32U5 geometry)
- **Partition**: 128 KB at offset 0x0 (`ubi_partition`)
- **Config**: `CONFIG_FLASH_SIMULATOR=y`, `CONFIG_FLASH_SIMULATOR_DOUBLE_WRITES=y`, `CONFIG_FLASH_SIMULATOR_EXPLICIT_ERASE=y`
- **Usage**: All test suites run here; stress tests are simulator-only

### Secondary: `b_u585i_iot02a`

- **Platform**: STM32U585AI hardware target
- **Usage**: Cross-compilation verification (build only in CI)

## Coverage

### Targets

| Metric | Target |
|--------|--------|
| Line coverage | ≥ 80% |
| Branch coverage | ≥ 70% |

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

Each test suite uses the standard ZTest fixture:

```c
static struct ubi_mtd mtd = { 0 };

static void *ztest_suite_setup(void) {
    /* Discover flash geometry, populate mtd */
}

static void ztest_testcase_before(void *ctx) {
    /* Erase entire partition for test isolation */
    flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE);
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

## Known Limitations

- **No hardware-in-the-loop**: Tests run only on the flash simulator; actual flash
  wear, timing, and power-loss behavior are not tested.
- **No power-loss simulation**: The simulator does not model power cuts during writes.
- **Dual-bank recovery**: The `ubi_device_erase_peb` function's bad-block torture
  path (`-ENOSYS`) is not exercised because the simulator doesn't produce flash
  erase failures.
- **Heap exhaustion**: Malloc failure paths are not systematically tested because
  Zephyr's heap allocator on `native_sim` does not easily support fault injection.

## How to Add a New Test

1. Choose the appropriate test file based on test category (see table above)
2. Add a `ZTEST(suite_name, test_name)` function
3. Follow the fixture pattern: init → operate → assert → deinit
4. If testing a new file, add it to `tests/CMakeLists.txt`
5. Build and run: `bash scripts/run_tests.sh`
