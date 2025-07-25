# UBI Hardware Tests Documentation

This document describes the hardware test cases for the Unsorted Block Images (UBI) implementation using Zephyr's Ztest framework.

---

## Test Suite: `ubi`

### Setup and Teardown
- **Suite Setup:** Initializes the memory technology device (`mtd`) using Zephyr flash device information.
- **Testcase Before:** Erases the flash partition before each test.
- **Testcase Teardown:** No special teardown after each test.
- **Suite After:** No special teardown after all tests.

---

## Test Cases

### 1. `init_deinit`
- **Description:** Tests the initialization and deinitialization of the UBI device.
- **Verifications:**
  - Device is successfully initialized.
  - Device info contains valid data:
    - No allocated or dirty physical erase blocks (PEBs).
    - Some free PEBs exist.
    - Logical eraseblock (LEB) count and size are valid.
  - Device is successfully deinitialized.

### 2. `multiple_init_deinit`
- **Description:** Tests repeated initialization and deinitialization cycles.
- **Verifications:**
  - UBI can be initialized and deinitialized multiple times without error.

### 3. `init_deinit_with_heap_checks`
- **Description:** Tests memory heap usage before initialization, after initialization, and after deinitialization.
- **Verifications:**
  - Heap statistics are collected before and after `ubi_init` and `ubi_deinit`.
  - After deinit, heap usage returns to initial state.
  - Heap usage changes during initialization and deinitialization.

### 4. `map_and_unmap`
- **Description:** Tests mapping all logical erase blocks (LEBs) and then unmapping them.
- **Verifications:**
  - All LEBs can be mapped successfully.
  - After mapping, allocated PEBs equal LEB count and no free PEBs remain.
  - After unmapping, dirty PEBs equal LEB count and no allocated or free PEBs remain.
  - Device deinitializes without error.

### 5. `map_unmap_erase_map`
- **Description:** Tests mapping, unmapping, erasing dirty PEBs, then mapping again.
- **Verifications:**
  - After erasing dirty PEBs, all become free PEBs.
  - Mapping after erase works correctly.
  - Device deinitializes without error.

### 6. `write_read_small_buffer`
- **Description:** Tests writing and reading a small buffer to/from a LEB.
- **Verifications:**
  - Buffer is written successfully.
  - Reading returns the exact same data.
  - Device deinitializes without error.

### 7. `write_read_max_buffer`
- **Description:** Tests writing and reading a maximum-sized buffer (full LEB size).
- **Verifications:**
  - Maximum buffer filled with incremental data is written successfully.
  - Reading returns the exact same data.
  - Device deinitializes without error.

### 8. `full_scenario`
- **Description:** Performs a mixed scenario of mapping, writing, reading, and verifying data.
- **Verifications:**
  - Maps several LEBs and verifies they are mapped.
  - Writes data to several LEBs and verifies written data correctness.
  - Reads from mapped but unwritten LEBs return zero length.
  - Device info reflects accurate PEB allocation.
  - Device deinitializes without error.

### 9. `flash_equal_weariness`
- **Description:** Tests wear-leveling and erase cycle consistency across multiple cycles.
- **Verifications:**
  - Average erase count and individual PEB erase counts increase as expected after each cycle.
  - Reserved PEBs remain consistent.
  - After each cycle, all LEBs are mapped, unmapped, and erased.
  - Device deinitializes without error.

---

## Notes
- Each test case uses `zassert` macros to validate expected behavior.
- The tests assume the UBI flash partition is erased before each test to ensure a clean state.
- The test data buffer `rdata` is a static 256-byte array with incremental values.

---

This documentation covers all the test cases provided in the `tests.c` file and explains their purpose and expected outcomes.
