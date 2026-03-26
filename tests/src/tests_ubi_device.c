/**
 * \file    tests_ubi_device.c
 *
 * \author  Kamil Kielbasa
 *
 * \brief   Hardware tests for Unsorted Block Images (UBI) device operations.
 *
 * \version 0.5
 * \date    2026-03-26
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files ------------------------------------------------------------------------------- */

/* UBI header: */
#include <ubi.h>

/* Zephyr headers: */
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <zephyr/toolchain/common.h>
#include <zephyr/sys/sys_heap.h>

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

/* Module defines ------------------------------------------------------------------------------ */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* Module types and type definitiones ---------------------------------------------------------- */
/* Module interface variables and constants ---------------------------------------------------- */
/* Static variables and constants -------------------------------------------------------------- */

static struct ubi_mtd mtd = { 0 };

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
extern struct sys_heap _system_heap;
#endif

static struct sys_memory_stats before_init = { 0 };
static struct sys_memory_stats after_init = { 0 };
static struct sys_memory_stats after_deinit = { 0 };

/* Static function declarations ---------------------------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_after(void *ctx);

static void ztest_testcase_before(void *ctx);
static void ztest_testcase_teardown(void *ctx);

static void memory_check(struct sys_memory_stats *bi, struct sys_memory_stats *ai,
			 struct sys_memory_stats *ad);

static void erase_counters_check(struct ubi_device *ubi, size_t exp_ec);

/* Static function definitions ----------------------------------------------------------------- */

static void *ztest_suite_setup(void)
{
	const struct device *flash_dev = UBI_PARTITION_DEVICE;
	zassert_true(device_is_ready(flash_dev));

	struct flash_pages_info page_info = { 0 };
	zassert_ok(flash_get_page_info_by_offs(flash_dev, 0, &page_info));

	const size_t write_block_size = flash_get_write_block_size(flash_dev);
	const size_t erase_block_size = page_info.size;

	mtd.partition_id = FIXED_PARTITION_ID(UBI_PARTITION_NAME);
	mtd.erase_block_size = erase_block_size;
	mtd.write_block_size = write_block_size;

	return NULL;
}

static void ztest_suite_after(void *ctx)
{
	(void)ctx;

	return;
}

static void ztest_testcase_before(void *ctx)
{
	(void)ctx;

	zassert_ok(flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE));

	return;
}

static void ztest_testcase_teardown(void *ctx)
{
	(void)ctx;
	return;
}

static void memory_check(struct sys_memory_stats *bi, struct sys_memory_stats *ai,
			 struct sys_memory_stats *ad)
{
	zassert_not_null(bi);
	zassert_not_null(ai);
	zassert_not_null(ad);

	zassert_equal(bi->free_bytes, ad->free_bytes);
	zassert_equal(bi->allocated_bytes, ad->allocated_bytes);

	zassert_not_equal(ai->free_bytes, ad->free_bytes);
	zassert_not_equal(ai->allocated_bytes, ad->allocated_bytes);

	memset(bi, 0, sizeof(*bi));
	memset(ai, 0, sizeof(*ai));
	memset(ad, 0, sizeof(*ad));
}

static void erase_counters_check(struct ubi_device *ubi, size_t exp_ec)
{
	size_t peb_ec_len = 0;
	size_t *peb_ec = NULL;
	zassert_ok(ubi_device_get_peb_ec(ubi, &peb_ec, &peb_ec_len));
	zassert_not_null(peb_ec);
	zassert_not_equal(0, peb_ec_len);

	size_t ec_avr = 0;
	for (size_t pnum = 0; pnum < peb_ec_len; ++pnum)
		ec_avr += peb_ec[pnum];

	ec_avr /= peb_ec_len;

	zassert_equal(exp_ec, ec_avr);

	k_free(peb_ec);
}

/* Module interface function definitions ------------------------------------------------------- */

ZTEST_SUITE(ubi_device, NULL, ztest_suite_setup, ztest_testcase_before, ztest_testcase_teardown,
	    ztest_suite_after);

/**
 * \brief Verify a single init/deinit cycle leaves the device in a clean state.
 *
 * \details Scenario: Initialize a freshly erased UBI device. Query device info
 *          and verify all counters (allocated, free, dirty, bad, volumes). Check
 *          that all PEB erase counters are 0. Deinitialize.
 *
 * \expect allocated_peb_count=0, free_peb_count=total PEBs, dirty=0, bad=0,
 *         volumes=0, leb_size within valid range. All erase counters are 0.
 *         Heap memory is fully reclaimed after deinit.
 */
ZTEST(ubi_device, init_deinit)
{
	const size_t exp_ec_avr = 0;
	const size_t total_nr_of_pebs = (UBI_PARTITION_SIZE / mtd.erase_block_size) - 2;

	struct ubi_device *ubi = NULL;
	struct ubi_device_info info = { 0 };

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(0, info.allocated_peb_count);
	zassert_equal(total_nr_of_pebs, info.free_peb_count);
	zassert_equal(0, info.dirty_peb_count);
	zassert_equal(0, info.bad_peb_count);
	zassert_equal(total_nr_of_pebs, info.total_peb_count);
	zassert_between_inclusive(info.leb_size, 1, mtd.erase_block_size - 1);
	zassert_equal(0, info.volume_count);

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);
}

/**
 * \brief Verify device state is consistent across two consecutive init/deinit
 *        cycles (simulated reboot).
 *
 * \details Scenario: Perform two full init-verify-deinit cycles on a freshly
 *          erased partition. Each cycle validates all device info counters
 *          and erase counter averages.
 *
 * \expect Both cycles produce identical device info. Heap memory is fully
 *         reclaimed after each deinit.
 */
ZTEST(ubi_device, init_deinit_reboot)
{
	const size_t exp_ec_avr = 0;
	const size_t total_nr_of_pebs = (UBI_PARTITION_SIZE / mtd.erase_block_size) - 2;

	struct ubi_device *ubi = NULL;
	struct ubi_device_info info = { 0 };

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(0, info.allocated_peb_count);
	zassert_equal(total_nr_of_pebs, info.free_peb_count);
	zassert_equal(0, info.dirty_peb_count);
	zassert_equal(0, info.bad_peb_count);
	zassert_equal(total_nr_of_pebs, info.total_peb_count);
	zassert_between_inclusive(info.leb_size, 1, mtd.erase_block_size - 1);
	zassert_equal(0, info.volume_count);

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(0, info.allocated_peb_count);
	zassert_equal(total_nr_of_pebs, info.free_peb_count);
	zassert_equal(0, info.dirty_peb_count);
	zassert_equal(0, info.bad_peb_count);
	zassert_equal(total_nr_of_pebs, info.total_peb_count);
	zassert_between_inclusive(info.leb_size, 1, mtd.erase_block_size - 1);
	zassert_equal(0, info.volume_count);

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);
}
