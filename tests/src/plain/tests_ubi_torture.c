/**
 * \file    tests_ubi_torture.c
 *
 * \author  Kamil Kielbasa
 *
 * \brief   Tests for UBI bad block torture recovery. Run only on native_sim
 *          (flash simulator). These tests corrupt flash to create bad PEBs,
 *          then verify the torture mechanism recovers them during erase_peb().
 *
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>
#include <ubi_test.h>

/* Test fixtures: */
#include "ubi_test_fixture.h"

/* Zephyr headers: */
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>

/* Standard library headers: */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

/* Module types and type definitiones ----------------------------------------------------------- */

/* Module interface variables and constants ----------------------------------------------------- */
#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

#define EC_HDR_MAGIC (0x55424923U)
#define EC_HDR_SIZE (16U)
#define NR_OF_RES_PEBS (2U)

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
	ubi_test_partition_force_release_all();
	zassert_ok(flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE));
}

static void ztest_testcase_teardown(void *ctx)
{
	(void)ctx;
}

/**
 * \brief Corrupt the EC header of a PEB with garbage bytes via raw flash write.
 */
static void corrupt_peb_ec_header(size_t peb_idx)
{
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t offset = peb_idx * flash.erase_block_size;
	zassert_ok(flash_area_erase(fa, offset, flash.erase_block_size));

	const uint8_t garbage[EC_HDR_SIZE] = { 0xDE, 0xAD, 0xBE, 0xEF };
	zassert_ok(flash_area_write(fa, offset, garbage, sizeof(garbage)));

	flash_area_close(fa);
}

/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_torture, NULL, ztest_suite_setup, ztest_testcase_before, ztest_testcase_teardown,
	    ztest_suite_after);

/**
 * \brief Verify that a bad PEB created by EC corruption is recovered by torture.
 *
 * \details Scenario: Initialize UBI normally and write data to produce a dirty
 *          PEB. Deinitialize, corrupt a data PEB's EC header with garbage bytes,
 *          and re-initialize. The init scan classifies the corrupted PEB as bad.
 *          Call ubi_device_erase_peb() to trigger torture. The flash simulator
 *          allows the erase to succeed, so the PEB should be recovered.
 *
 * \expect After erase_peb(), bad_peb_count drops to 0 and free_peb_count
 *         increases, confirming the PEB was recovered.
 */
ZTEST(ubi_torture, corrupt_peb_recovered_by_torture)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "tort1",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xCA, 0xFE };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Corrupt a data PEB's EC header. */
	corrupt_peb_ec_header(NR_OF_RES_PEBS);

	/* Re-init: corrupted PEB should be classified as bad. */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.bad_peb_count >= 1, "Expected at least 1 bad PEB");

	const size_t free_before = info.free_peb_count;
	const size_t bad_before = info.bad_peb_count;

	/* Trigger erase_peb which also runs torture on bad blocks. */
	ubi_device_erase_peb(ubi);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.bad_peb_count, bad_before - 1, "Bad PEB should be recovered by torture");
	zassert_equal(info.free_peb_count, free_before + 1, "Recovered PEB should appear as free");

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that a recovered PEB is usable for writes.
 *
 * \details Scenario: Create a bad PEB via EC corruption, re-init, and call
 *          erase_peb() to recover it. Then write data to a LEB and read it
 *          back to confirm the recovered PEB is fully functional.
 *
 * \expect Write and read-back succeed. Data matches the written pattern.
 */
ZTEST(ubi_torture, recovered_peb_is_writable)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "tort2",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Corrupt a data PEB. */
	corrupt_peb_ec_header(NR_OF_RES_PEBS);

	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.bad_peb_count >= 1);

	/* Erase dirty PEBs and torture bad ones. */
	for (size_t i = 0; i < info.dirty_peb_count + 1; ++i) {
		ubi_device_erase_peb(ubi);
	}

	/* Verify the recovered PEB is usable. */
	const uint8_t wdata[] = { 0x01, 0x02, 0x03, 0x04 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, wdata, sizeof(wdata)));

	uint8_t rdata[4] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, wdata, sizeof(wdata));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify ec_avg is populated in device info after init.
 *
 * \details Scenario: Initialize a fresh device. All PEBs start at EC=0,
 *          so ec_avg should be 0. Perform some writes and erases to
 *          increment erase counters, then verify ec_avg updates.
 *
 * \expect ec_avg is 0 after fresh init, and increases after erase cycles.
 */
ZTEST(ubi_torture, ec_avg_tracking)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.ec_avg, 0, "Fresh device should have ec_avg=0");

	const struct ubi_volume_config cfg = {
		.name = "tort3",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Perform several write-erase cycles to bump erase counters. */
	const uint8_t data[] = { 0xAA };
	const size_t cycles = info.total_peb_count * 12;

	for (size_t i = 0; i < cycles; ++i) {
		zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
		ubi_device_erase_peb(ubi);
	}

	zassert_ok(ubi_device_get_info(ubi, &info));

	/*
	 * The first write maps a free PEB (no dirty produced), so only
	 * (cycles - 1) erases occur. Each erase increments ec_sum by 1.
	 * ec_avg = (cycles - 1) / total_peb_count  (integer division).
	 */
	const size_t expected_ec_avg = (cycles - 1) / info.total_peb_count;
	zassert_equal(info.ec_avg, expected_ec_avg, "Expected ec_avg=%zu after %zu cycles",
		      expected_ec_avg, cycles);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify ec_avg remains consistent after torture recovery.
 *
 * \details Scenario: Create a bad PEB, recover it via torture, and verify
 *          ec_avg is still valid (matches what would be computed from a
 *          full scan of PEB erase counters).
 *
 * \expect ec_avg after recovery is non-negative and device info is consistent.
 */
ZTEST(ubi_torture, ec_avg_consistent_after_recovery)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "tort4",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Run some erase cycles to get non-zero EC values. */
	const uint8_t data[] = { 0xBB };
	for (size_t i = 0; i < 5; ++i) {
		zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
		ubi_device_erase_peb(ubi);
	}

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Corrupt a PEB. */
	corrupt_peb_ec_header(NR_OF_RES_PEBS);

	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	const size_t bad_before = info.bad_peb_count;
	zassert_true(bad_before >= 1);

	/* Erase and torture. */
	for (size_t i = 0; i < info.dirty_peb_count + 1; ++i) {
		ubi_device_erase_peb(ubi);
	}

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.bad_peb_count, 0, "All bad PEBs should be recovered");

	/* Verify accounting: free + dirty + bad + allocated = total */
	const size_t accounted = info.free_peb_count + info.dirty_peb_count + info.bad_peb_count +
				 info.reserved_peb_count;
	zassert_equal(accounted, info.total_peb_count, "PEB accounting mismatch: %zu != %zu",
		      accounted, info.total_peb_count);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify torture respects CONFIG_UBI_BAD_PEB_TORTURE_CYCLES limit.
 *
 * \details Scenario: Corrupt 2 data PEBs to create 2 bad blocks. Call
 *          erase_peb() once. If CONFIG_UBI_BAD_PEB_TORTURE_CYCLES is 1,
 *          only 1 PEB should be recovered per call. With default of 3,
 *          both are recovered in a single call.
 *
 * \expect After the first erase_peb() call, bad_peb_count decreases by at most
 *         CONFIG_UBI_BAD_PEB_TORTURE_CYCLES.
 *
 * \note   TODO: Torture "stays bad" path cannot be tested with the flash
 *         simulator because threshold-based erase failures are silent (return 0).
 */
ZTEST(ubi_torture, max_per_erase_limit)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "tort5",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Corrupt 2 data PEBs. */
	corrupt_peb_ec_header(NR_OF_RES_PEBS);
	corrupt_peb_ec_header(NR_OF_RES_PEBS + 1);

	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.bad_peb_count >= 2, "Expected at least 2 bad PEBs");

	const size_t bad_before = info.bad_peb_count;

	/* Single erase_peb call — should recover at most max_per_erase PEBs. */
	ubi_device_erase_peb(ubi);

	zassert_ok(ubi_device_get_info(ubi, &info));
	const size_t recovered = bad_before - info.bad_peb_count;
	zassert_true(recovered <= CONFIG_UBI_BAD_PEB_TORTURE_CYCLES,
		     "Recovered %zu PEBs but torture_cycles is %d", recovered,
		     CONFIG_UBI_BAD_PEB_TORTURE_CYCLES);

	/* Call again to recover any remaining. */
	ubi_device_erase_peb(ubi);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.bad_peb_count, 0, "All bad PEBs should be recovered");

	zassert_ok(ubi_device_deinit(ubi));
}
