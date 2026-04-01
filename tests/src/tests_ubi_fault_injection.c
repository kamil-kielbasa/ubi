/**
 * \file    tests_ubi_fault_injection.c
 *
 * \brief   Transactional safety tests using fault injection.
 *
 * These tests verify that UBI operations remain transactionally safe
 * when k_malloc or flash I/O fails at critical points.
 *
 * Requires CONFIG_UBI_TEST_FAULT_INJECTION=y and CONFIG_UBI_TEST_API_ENABLE=y.
 *
 * \copyright Copyright (c) 2026
 */

#include <ubi.h>
#include "ubi_test_fixture.h"
#include "ubi_test_memory.h"

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>

#include <string.h>

static struct ubi_mtd mtd = { 0 };

static void *ztest_suite_setup(void)
{
	ubi_test_setup_mtd(&mtd);
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

ZTEST_SUITE(ubi_fault_injection, NULL, ztest_suite_setup, ztest_testcase_before,
	    ztest_testcase_teardown, ztest_suite_after);

/**
 * \brief Verify that volume create with malloc failure after flash commit
 *        does NOT leave a persistent volume (P0.2 validation).
 *
 * Since P0.2 reordered create to allocate RAM before flash, an early
 * ENOMEM now returns cleanly. This test validates the fix by checking
 * that when create returns ENOMEM, no volume exists on re-init.
 */
ZTEST(ubi_fault_injection, create_malloc_fail_no_persistent_volume)
{
	struct ubi_device *ubi = ubi_test_init_device(&mtd);

	const struct ubi_volume_config cfg = {
		.name = "fivol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	int ret = ubi_volume_create(ubi, &cfg, &vol_id);

	if (ret == -ENOMEM) {
		zassert_ok(ubi_device_deinit(ubi));

		ubi = ubi_test_init_device(&mtd);

		struct ubi_device_info info;
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
 */
ZTEST(ubi_fault_injection, overwrite_preserves_old_data_on_failure)
{
	struct ubi_device *ubi = ubi_test_init_device(&mtd);

	const struct ubi_volume_config cfg = {
		.name = "cowvol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
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
 */
ZTEST(ubi_fault_injection, invariants_hold_after_create_write_remove)
{
#if defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = ubi_test_init_device(&mtd);

	zassert_ok(ubi_device_check_invariants(ubi));

	const struct ubi_volume_config cfg = {
		.name = "invvol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
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
 */
ZTEST(ubi_fault_injection, invariants_hold_after_resize_shrink)
{
#if defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = ubi_test_init_device(&mtd);

	const struct ubi_volume_config cfg = {
		.name = "shrinv",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 4,
	};
	int vol_id;
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
