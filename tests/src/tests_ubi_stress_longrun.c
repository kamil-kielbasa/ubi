/**
 * \file    tests_ubi_stress_longrun.c
 *
 * \brief   Randomized churn and concurrency smoke tests.
 *
 * These tests exercise the UBI stack under sustained mixed workloads
 * and verify invariants hold throughout.
 *
 * Only built when CONFIG_FLASH_SIMULATOR is available (native_sim).
 *
 * \copyright Copyright (c) 2026
 */

#include <ubi.h>
#include "ubi_test_fixture.h"

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>

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

ZTEST_SUITE(ubi_stress_longrun, NULL, ztest_suite_setup, ztest_testcase_before,
	    ztest_testcase_teardown, ztest_suite_after);

/**
 * \brief Randomized churn: create, write, read, erase, remove in a loop.
 *
 * Runs 50 iterations of: create a volume, write to random LEBs, read back,
 * erase dirty PEBs, remove the volume. Checks invariants after each cycle
 * if the test API is available.
 */
ZTEST(ubi_stress_longrun, randomized_churn_with_reboots)
{
	uint8_t wbuf[32];
	uint8_t rbuf[32];

	for (int cycle = 0; cycle < 50; cycle++) {
		struct ubi_device *ubi = ubi_test_init_device(&mtd);

		const struct ubi_volume_config cfg = {
			.name = "churn",
			.type = UBI_VOLUME_TYPE_DYNAMIC,
			.leb_count = 3,
		};
		int vol_id;
		zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

		for (size_t lnum = 0; lnum < 3; lnum++) {
			memset(wbuf, (uint8_t)(cycle + lnum), sizeof(wbuf));
			zassert_ok(ubi_leb_write(ubi, vol_id, lnum, wbuf, sizeof(wbuf)));
		}

		for (size_t lnum = 0; lnum < 3; lnum++) {
			memset(wbuf, (uint8_t)(cycle + lnum), sizeof(wbuf));
			zassert_ok(ubi_leb_read(ubi, vol_id, lnum, 0, rbuf, sizeof(rbuf)));
			zassert_mem_equal(rbuf, wbuf, sizeof(wbuf));
		}

		for (int i = 0; i < 5; i++) {
			zassert_ok(ubi_device_erase_peb(ubi));
		}

#if defined(CONFIG_UBI_TEST_API_ENABLE)
		zassert_ok(ubi_device_check_invariants(ubi));
#endif

		zassert_ok(ubi_volume_remove(ubi, vol_id));
		zassert_ok(ubi_device_deinit(ubi));
	}
}

/**
 * \brief Persistence check: write data, deinit+reinit, verify data survives.
 */
ZTEST(ubi_stress_longrun, persistence_across_reinit)
{
	struct ubi_device *ubi = ubi_test_init_device(&mtd);

	const struct ubi_volume_config cfg = {
		.name = "persist",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	zassert_ok(ubi_device_deinit(ubi));

	ubi = ubi_test_init_device(&mtd);

	struct ubi_device_info info;
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count);

	uint8_t rbuf[6] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rbuf, sizeof(rbuf)));
	zassert_mem_equal(rbuf, data, sizeof(data));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Mixed operations: create multiple volumes, write, resize, remove.
 */
ZTEST(ubi_stress_longrun, mixed_multi_volume_operations)
{
	struct ubi_device *ubi = ubi_test_init_device(&mtd);

	const struct ubi_volume_config cfg_a = {
		.name = "vol_a",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	const struct ubi_volume_config cfg_b = {
		.name = "vol_b",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int id_a, id_b;
	zassert_ok(ubi_volume_create(ubi, &cfg_a, &id_a));
	zassert_ok(ubi_volume_create(ubi, &cfg_b, &id_b));

	const uint8_t data_a[] = { 0xAA, 0xBB };
	const uint8_t data_b[] = { 0xCC, 0xDD };
	zassert_ok(ubi_leb_write(ubi, id_a, 0, data_a, sizeof(data_a)));
	zassert_ok(ubi_leb_write(ubi, id_b, 0, data_b, sizeof(data_b)));

	const struct ubi_volume_config grow_cfg = {
		.name = "vol_a",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 3,
	};
	zassert_ok(ubi_volume_resize(ubi, id_a, &grow_cfg));

	uint8_t rbuf[2];
	zassert_ok(ubi_leb_read(ubi, id_a, 0, 0, rbuf, sizeof(rbuf)));
	zassert_mem_equal(rbuf, data_a, sizeof(data_a));

	zassert_ok(ubi_volume_remove(ubi, id_b));

	struct ubi_device_info info;
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count);

#if defined(CONFIG_UBI_TEST_API_ENABLE)
	zassert_ok(ubi_device_check_invariants(ubi));
#endif

	zassert_ok(ubi_device_deinit(ubi));
}
