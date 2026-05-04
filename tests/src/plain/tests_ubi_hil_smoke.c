/**
 * \file    tests_ubi_hil_smoke.c
 *
 * \brief   Hardware-in-the-loop smoke tests for STM32U585 (or any real flash).
 *
 * These tests exercise UBI on actual hardware flash. They should be run
 * on a board target (not native_sim).
 *
 * \copyright Copyright (c) 2026
 */

#include <ubi.h>
#include "ubi_test_fixture.h"

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>

#include <string.h>

static struct ubi_flash_desc flash = { 0 };

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

ZTEST_SUITE(ubi_hil_smoke, NULL, ztest_suite_setup, ztest_testcase_before, ztest_testcase_teardown,
	    ztest_suite_after);

/**
 * \brief HIL smoke: init, create volume, write, read, remove on real flash.
 */
ZTEST(ubi_hil_smoke, hil_basic_lifecycle)
{
	struct ubi_device *ubi = ubi_test_init_device(&flash);

	const struct ubi_volume_config cfg = {
		.name = "hil_v",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xCA, 0xFE, 0xBA, 0xBE };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t rbuf[4] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rbuf, sizeof(rbuf)));
	zassert_mem_equal(rbuf, data, sizeof(data));

	zassert_ok(ubi_volume_remove(ubi, vol_id));

#if defined(CONFIG_UBI_TEST_API_ENABLE)
	zassert_ok(ubi_device_check_invariants(ubi));
#endif

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief HIL persistence: write, deinit, reinit, verify data survives.
 */
ZTEST(ubi_hil_smoke, hil_persistence)
{
	struct ubi_device *ubi = ubi_test_init_device(&flash);

	const struct ubi_volume_config cfg = {
		.name = "hil_p",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_device_deinit(ubi));

	ubi = ubi_test_init_device(&flash);

	uint8_t rbuf[8] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rbuf, sizeof(rbuf)));
	zassert_mem_equal(rbuf, data, sizeof(data));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief HIL stress: repeated write/erase cycles on real flash.
 */
ZTEST(ubi_hil_smoke, hil_stress_cycles)
{
	struct ubi_device *ubi = ubi_test_init_device(&flash);

	const struct ubi_volume_config cfg = {
		.name = "hil_s",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	uint8_t wbuf[16];
	uint8_t rbuf[16];

	for (int cycle = 0; cycle < 100; cycle++) {
		memset(wbuf, (uint8_t)cycle, sizeof(wbuf));

		zassert_ok(ubi_leb_write(ubi, vol_id, cycle % 2, wbuf, sizeof(wbuf)));

		zassert_ok(ubi_leb_read(ubi, vol_id, cycle % 2, 0, rbuf, sizeof(rbuf)));
		zassert_mem_equal(rbuf, wbuf, sizeof(wbuf));

		zassert_ok(ubi_device_erase_peb(ubi));
	}

#if defined(CONFIG_UBI_TEST_API_ENABLE)
	zassert_ok(ubi_device_check_invariants(ubi));
#endif

	zassert_ok(ubi_device_deinit(ubi));
}
