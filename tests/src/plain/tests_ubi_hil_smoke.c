/**
 * \file    tests_ubi_hil_smoke.c
 *
 * \author Kamil Kielbasa
 *
 * \brief   Hardware-in-the-loop smoke tests for STM32U585 (or any real flash).
 *
 * These tests exercise UBI on actual hardware flash. They should be run
 * on a board target (not native_sim).
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>

/* Test fixtures: */
#include "ubi_test_fixture.h"

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

ZTEST_SUITE(ubi_hil_smoke, NULL, ztest_suite_setup, ztest_testcase_before, ztest_testcase_teardown,
	    ztest_suite_after);

/**
 * \brief HIL smoke: init, create volume, write, read, remove on real flash.
 *
 * \details Scenario: Initialize the device on real flash, create dynamic volume "hil_v"
 *          with 2 LEBs, write 4 bytes {0xCA, 0xFE, 0xBA, 0xBE} to LEB 0, read back, remove
 *          the volume, check invariants where available, deinit.
 *
 * \expect All operations return 0; read data matches written data; invariants pass.
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
 *
 * \details Scenario: Initialize device, create static volume "hil_p" with 1 LEB, write
 *          8 bytes {0x11, 0x22, ..., 0x88} to LEB 0, deinit. Reinit and read LEB 0.
 *
 * \expect Reinit returns 0; the read data matches the originally written data.
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
 *
 * \details Scenario: Initialize device, create dynamic volume "hil_s" with 2 LEBs,
 *          perform 100 cycles of: write a 16-byte pattern to LEB(cycle%2), read back,
 *          erase dirty PEB. Check invariants where available and deinit.
 *
 * \expect All 100 cycles succeed; read data matches the written pattern; invariants pass.
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
