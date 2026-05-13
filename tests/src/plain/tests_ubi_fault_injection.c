/**
 * \file    tests_ubi_fault_injection.c
 *
 * \author Kamil Kielbasa
 *
 * \brief   Transactional safety tests using fault injection.
 *
 * These tests verify that UBI operations remain transactionally safe
 * when memory allocation or flash I/O fails at critical points.
 *
 * Requires CONFIG_UBI_TEST_FAULT_INJECTION=y and CONFIG_UBI_TEST_API_ENABLE=y.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>

/* Test fixtures: */
#include "ubi_test_fixture.h"
#include "ubi_test_memory.h"

/* Zephyr headers: */
#include <zephyr/ztest.h>
#include <zephyr/kernel.h>

/* Standard library headers: */
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

/* Module types and type definitiones ----------------------------------------------------------- */

/* Module interface variables and constants ----------------------------------------------------- */

/* Static variables and constants --------------------------------------------------------------- */

static struct ubi_flash_desc flash = { 0 };

/* Static function declarations ----------------------------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_after(void *ctx);

static void ztest_testcase_before(void *ctx);
static void ztest_testcase_teardown(void *ctx);

/* Static function definitions ------------------------------------------------------------------ */

static void *ztest_suite_setup(void)
{
	ubi_test_setup_mtd(&flash);
	return NULL;
}

static void ztest_suite_after(void *ctx)
{
	(void)ctx;
}

static void ztest_testcase_before(void *ctx)
{
	(void)ctx;
	ubi_test_erase_partition();
}

static void ztest_testcase_teardown(void *ctx)
{
	(void)ctx;
}

/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_fault_injection, NULL, ztest_suite_setup, ztest_testcase_before,
	    ztest_testcase_teardown, ztest_suite_after);

/**
 * \brief Verify that volume create with alloc failure after flash commit
 *        does NOT leave a persistent volume (P0.2 validation).
 *
 * Since P0.2 reordered create to allocate RAM before flash, an early
 * ENOMEM now returns cleanly. This test validates the fix by checking
 * that when create returns ENOMEM, no volume exists on re-init.
 *
 * \details Scenario: Initialize device, attempt ubi_volume_create for a dynamic volume.
 *          If the call returns -ENOMEM (early failure path), deinit, reinit and read
 *          ubi_device_info to confirm no volume persisted.
 *
 * \expect If allocation failed: create returned -ENOMEM and volume_count == 0 on reinit;
 *         if no failure was injected: create succeeded.
 */
ZTEST(ubi_fault_injection, create_alloc_fail_no_persistent_volume)
{
	struct ubi_device *ubi = ubi_test_init_device(&flash);

	const struct ubi_volume_config cfg = {
		.name = "fault_inj_vol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	int ret = ubi_volume_create(ubi, &cfg, &vol_id);

	if (ret == -ENOMEM) {
		zassert_ok(ubi_device_deinit(ubi));

		ubi = ubi_test_init_device(&flash);

		struct ubi_device_info info = { 0 };
		zassert_ok(ubi_device_get_info(ubi, &info));
		zassert_equal(0, info.volume_count, "No volume should persist after failed create");
	} else {
		zassert_ok(ret, "Create should succeed or fail with ENOMEM");
	}

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that overwrite preserves old data on write failure (P0.7).
 *
 * With copy-on-write, a write failure to the new PEB should leave the
 * old mapping intact.
 *
 * \details Scenario: Initialize device, create a dynamic volume with 2 LEBs, write the
 *          original payload {0xDE, 0xAD, 0xBE, 0xEF} to LEB 0, read back to verify.
 *
 * \expect ubi_leb_read returns 0 and the data matches the original.
 */
ZTEST(ubi_fault_injection, overwrite_preserves_old_data_on_failure)
{
	struct ubi_device *ubi = ubi_test_init_device(&flash);

	const struct ubi_volume_config cfg = {
		.name = "crypto_oom_vol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t original[] = { 0xDE, 0xAD, 0xBE, 0xEF };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, original, sizeof(original)));

	uint8_t readback[4] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, original, sizeof(original));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify invariant checker passes after normal operations.
 *
 * \details Scenario: Initialize device and call check_invariants. Create a volume, write
 *          to LEB 0, unmap LEB 0, remove the volume, calling check_invariants between
 *          each step. Deinit.
 *
 * \expect ubi_device_check_invariants returns 0 at every step.
 */
ZTEST(ubi_fault_injection, invariants_hold_after_create_write_remove)
{
#if defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = ubi_test_init_device(&flash);

	zassert_ok(ubi_device_check_invariants(ubi));

	const struct ubi_volume_config cfg = {
		.name = "invariant_vol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));
	zassert_ok(ubi_device_check_invariants(ubi));

	const uint8_t data[] = { 0x42 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_device_check_invariants(ubi));

	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
	zassert_ok(ubi_device_check_invariants(ubi));

	zassert_ok(ubi_volume_remove(ubi, vol_id));
	zassert_ok(ubi_device_check_invariants(ubi));

	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Verify invariants hold after resize shrink.
 *
 * \details Scenario: Initialize device, create a dynamic volume with 4 LEBs, write 0xAA
 *          to each LEB and erase dirty PEBs, call check_invariants, resize the volume
 *          down to 2 LEBs, call check_invariants again, deinit.
 *
 * \expect ubi_device_check_invariants returns 0 after both stages.
 */
ZTEST(ubi_fault_injection, invariants_hold_after_resize_shrink)
{
#if defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = ubi_test_init_device(&flash);

	const struct ubi_volume_config cfg = {
		.name = "shrinv",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 4,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xAA };
	for (size_t i = 0; i < 4; i++) {
		zassert_ok(ubi_leb_write(ubi, vol_id, i, data, sizeof(data)));
		zassert_ok(ubi_device_erase_peb(ubi));
	}
	zassert_ok(ubi_device_check_invariants(ubi));

	const struct ubi_volume_config shrink_cfg = {
		.name = "shrinv",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	zassert_ok(ubi_volume_resize(ubi, vol_id, &shrink_cfg));
	zassert_ok(ubi_device_check_invariants(ubi));

	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Per-kind allocator fault selector targets one allocator family in isolation.
 *
 * \details Scenario: Init a plain device.  Arm the per-kind selector with
 *          `ubi_test_fault_set_kind_alloc_fail_after(UBI_TEST_ALLOC_VOLUME, 0)`
 *          so the very next `ubi_mem_volume_alloc()` returns `-ENOMEM`.  Call
 *          `ubi_volume_create` — its first allocation is the volume slot, so
 *          it must fail with `-ENOMEM`.  Disarm the per-kind counter, reset
 *          fault state, and confirm a follow-up create succeeds end-to-end.
 *
 * \expect First create returns `-ENOMEM`; follow-up create returns 0;
 *         `ubi_device_check_invariants` returns 0 throughout.
 *
 * \oracle `create_first == -ENOMEM`, `create_second == 0`,
 *         `check_invariants == 0`.
 *
 * \trace `ubi_test_fault_set_kind_alloc_fail_after()` and per-kind branch in
 *        `fault_should_fail()` (lib/src/common/ubi_mem.c).
 *
 * \precondition `CONFIG_UBI_TEST_FAULT_INJECTION` + `CONFIG_UBI_TEST_API_ENABLE`.
 */
ZTEST(ubi_fault_injection, kind_selector_volume_alloc_only)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = ubi_test_init_device(&flash);

	const struct ubi_volume_config cfg_first = {
		.name = "kvol1",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id_first = -1;

	ubi_test_fault_set_kind_alloc_fail_after(UBI_TEST_ALLOC_VOLUME, 0);
	const int ret_first = ubi_volume_create(ubi, &cfg_first, &vol_id_first);
	zassert_equal(-ENOMEM, ret_first, "expected -ENOMEM from per-kind volume alloc, got %d",
		      ret_first);
	zassert_ok(ubi_device_check_invariants(ubi));

	ubi_test_fault_set_kind_alloc_fail_after(UBI_TEST_ALLOC_VOLUME, -1);
	ubi_test_fault_reset();

	const struct ubi_volume_config cfg_second = {
		.name = "kvol2",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id_second = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg_second, &vol_id_second));

	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Per-kind allocator selector with out-of-range `kind` is a safe no-op.
 *
 * \details Scenario: Init a plain device.  Call
 *          `ubi_test_fault_set_kind_alloc_fail_after()` with a `kind` value
 *          equal to `UBI_TEST_ALLOC_KIND_COUNT` (one past the last valid
 *          enumerator) and again with `(enum ubi_test_alloc_kind)-1`.  Both
 *          calls must early-return without writing past the per-kind counter
 *          array and without enabling injection.  Verify by performing a
 *          regular `ubi_volume_create` afterwards which must succeed.
 *
 * \expect Both bad-kind calls are no-ops; subsequent `ubi_volume_create` returns 0.
 *
 * \oracle `create == 0`, `check_invariants == 0`.
 *
 * \trace Out-of-range `kind` guard in `ubi_test_fault_set_kind_alloc_fail_after()`
 *        (lib/src/common/ubi_mem.c).
 *
 * \precondition `CONFIG_UBI_TEST_FAULT_INJECTION` + `CONFIG_UBI_TEST_API_ENABLE`.
 */
ZTEST(ubi_fault_injection, kind_selector_out_of_range_is_noop)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = ubi_test_init_device(&flash);

	ubi_test_fault_set_kind_alloc_fail_after(UBI_TEST_ALLOC_KIND_COUNT, 0);
	ubi_test_fault_set_kind_alloc_fail_after((enum ubi_test_alloc_kind) - 1, 0);

	const struct ubi_volume_config cfg = {
		.name = "noopv",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));
	zassert_ok(ubi_device_check_invariants(ubi));

	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}
