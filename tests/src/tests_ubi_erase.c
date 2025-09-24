/**
 * \file    tests_ubi_erase.c
 *
 * \author  Kamil Kielbasa
 *
 * \brief   Hardware tests for Unsorted Block Images (UBI) erases.
 *
 * \version 0.4
 * \date    2025-09-24
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files ------------------------------------------------------------------------------- */

/* UBI header: */
#include <ubi.h>
#include "arrays.h"

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

static void memory_check(struct sys_memory_stats *before_init, struct sys_memory_stats *after_init,
			 struct sys_memory_stats *after_deinit);

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

static void memory_check(struct sys_memory_stats *before_init, struct sys_memory_stats *after_init,
			 struct sys_memory_stats *after_deinit)
{
	zassert_not_null(before_init);
	zassert_not_null(after_init);
	zassert_not_null(after_deinit);

	zassert_equal(before_init->free_bytes, after_deinit->free_bytes);
	zassert_equal(before_init->allocated_bytes, after_deinit->allocated_bytes);

	zassert_not_equal(after_init->free_bytes, after_deinit->free_bytes);
	zassert_not_equal(after_init->allocated_bytes, after_deinit->allocated_bytes);

	memset(before_init, 0, sizeof(*before_init));
	memset(after_init, 0, sizeof(*after_init));
	memset(after_deinit, 0, sizeof(*after_deinit));
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

ZTEST_SUITE(ubi_erase, NULL, ztest_suite_setup, ztest_testcase_before, ztest_testcase_teardown,
	    ztest_suite_after);

ZTEST(ubi_erase, one_volume_one_leb_operations_with_reboot)
{
	size_t exp_ec_avr = 0;

	const struct ubi_volume_config vol_cfg_1 = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	int vol_id_1 = -1;
	const size_t lnum = 0;

	struct ubi_device *ubi = NULL;
	struct ubi_device_info info = { 0 };
	struct ubi_device_info info_after_init = { 0 };

	size_t len = 0;
	size_t *peb_ec = NULL;

	/* 1. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	/* 2. Create volume */
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_1, &vol_id_1));

	zassert_ok(ubi_device_get_info(ubi, &info_after_init));
	zassert_equal(vol_cfg_1.leb_count, info_after_init.allocated_leb_count);
	zassert_equal(info_after_init.free_leb_count, info_after_init.leb_total_count);
	zassert_equal(0, info_after_init.dirty_leb_count);
	zassert_equal(1, info_after_init.volumes_count);

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info_after_init.allocated_leb_count, vol_cfg_1.leb_count);
	zassert_equal(1, info_after_init.volumes_count);

	/* 3. Write data to LEB */
	for (size_t i = 0; i < info_after_init.leb_total_count; ++i) {
		zassert_ok(ubi_leb_write(ubi, vol_id_1, lnum, array_256, ARRAY_SIZE(array_256)));

		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(ubi, &info));
		zassert_equal(info.free_leb_count, info.leb_total_count - i - 1);
		zassert_equal(i, info.dirty_leb_count);
	}

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.free_leb_count);
	zassert_equal(info.leb_total_count - 1, info.dirty_leb_count);

	/* 4. Read data from LEB */
	size_t rdata_size = 0;
	uint8_t rdata[ARRAY_SIZE(array_256)] = { 0 };

	zassert_ok(ubi_leb_get_size(ubi, vol_id_1, lnum, &rdata_size));
	zassert_equal(ARRAY_SIZE(array_256), rdata_size);

	zassert_ok(ubi_leb_read(ubi, vol_id_1, lnum, 0, rdata, rdata_size));
	zassert_mem_equal(rdata, array_256, ARRAY_SIZE(array_256), "Memory blocks are not equal");

	/* 5. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	exp_ec_avr = 0;
	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 6. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	/* 7. Get device infos */
	zassert_ok(ubi_device_get_info(ubi, &info_after_init));
	zassert_equal(vol_cfg_1.leb_count, info_after_init.allocated_leb_count);
	zassert_equal(0, info_after_init.free_leb_count);
	zassert_equal(info_after_init.leb_total_count - 1, info_after_init.dirty_leb_count);
	zassert_equal(1, info_after_init.volumes_count);

	/* 8. Unmap LEB and erase all dirty LEBs */
	zassert_ok(ubi_leb_unmap(ubi, vol_id_1, lnum));

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.free_leb_count);
	zassert_equal(info.leb_total_count, info.dirty_leb_count);

	for (size_t i = 0; i < info.dirty_leb_count; ++i) {
		zassert_ok(ubi_device_erase_peb(ubi));

		struct ubi_device_info _info = { 0 };
		zassert_ok(ubi_device_get_info(ubi, &_info));
		zassert_equal(i + 1, _info.free_leb_count);
		zassert_equal(_info.leb_total_count - i - 1, _info.dirty_leb_count);
		zassert_equal(_info.free_leb_count + _info.dirty_leb_count, _info.leb_total_count);
	}

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.leb_total_count, info.free_leb_count);
	zassert_equal(0, info.dirty_leb_count);

	/* 9. Verify internal EC header counters */
	peb_ec = NULL;
	len = 0;
	zassert_ok(ubi_device_get_peb_ec(ubi, &peb_ec, &len));

	zassert_equal(len, info.leb_total_count);
	for (size_t i = 0; i < len; ++i)
		zassert_equal(1, peb_ec[i]);

	k_free(peb_ec);

	/* 5. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	exp_ec_avr = 1;
	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);
}

ZTEST(ubi_erase, many_volumes_many_lebs_operations_with_reboot)
{
	size_t exp_ec_avr[] = { 0, 1, 2, 3, 4, 5 };

	const struct ubi_volume_config vol_cfg_1 = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 7,
	};

	const struct ubi_volume_config vol_cfg_2 = {
		.name = { '/', 'u', 'b', 'i', '_', '1' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 7,
	};

	int vol_id_1 = -1;
	int vol_id_2 = -1;

	struct ubi_device *ubi = NULL;
	struct ubi_device_info info = { 0 };

	size_t len = 0;
	size_t *peb_ec = NULL;

	size_t wdata_idx = 0;
	size_t rdata_idx = 0;
	size_t erase_idx = 0;

	const uint8_t *wdata[] = {
		array_1,   array_2,   array_4,	 array_8,    array_16,	 array_32,   array_64,
		array_128, array_256, array_512, array_1024, array_2048, array_4096, array_8000,
	};

	const size_t wdata_size[] = {
		ARRAY_SIZE(array_1),	ARRAY_SIZE(array_2),	ARRAY_SIZE(array_4),
		ARRAY_SIZE(array_8),	ARRAY_SIZE(array_16),	ARRAY_SIZE(array_32),
		ARRAY_SIZE(array_64),	ARRAY_SIZE(array_128),	ARRAY_SIZE(array_256),
		ARRAY_SIZE(array_512),	ARRAY_SIZE(array_1024), ARRAY_SIZE(array_2048),
		ARRAY_SIZE(array_4096), ARRAY_SIZE(array_8000),
	};

	zassert_equal(ARRAY_SIZE(wdata), ARRAY_SIZE(wdata_size));
	zassert_equal(ARRAY_SIZE(wdata), vol_cfg_1.leb_count + vol_cfg_2.leb_count);
	zassert_equal(ARRAY_SIZE(wdata_size), vol_cfg_1.leb_count + vol_cfg_2.leb_count);

	const struct ubi_volume_config *volumes[] = {
		&vol_cfg_1,
		&vol_cfg_2,
	};

	/* 1. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	/* 2. Create volume */
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_1, &vol_id_1));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_2, &vol_id_2));

	const int volumes_id[] = {
		vol_id_1,
		vol_id_2,
	};

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.allocated_leb_count, vol_cfg_1.leb_count + vol_cfg_2.leb_count);
	zassert_equal(2, info.volumes_count);

	/* 3. Cycles of write volumes LEBs */
	for (size_t cycle = 0; cycle < ARRAY_SIZE(exp_ec_avr); ++cycle) {
		peb_ec = NULL;
		len = 0;
		zassert_ok(ubi_device_get_peb_ec(ubi, &peb_ec, &len));

		zassert_equal(len, info.leb_total_count);
		for (size_t i = 0; i < len; ++i)
			zassert_equal(exp_ec_avr[cycle], peb_ec[i]);

		k_free(peb_ec);

		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(ubi, &info));
		zassert_equal(info.leb_total_count, info.free_leb_count);
		zassert_equal(0, info.dirty_leb_count);

		wdata_idx = 0;
		for (size_t vol_idx = 0; vol_idx < ARRAY_SIZE(volumes_id); ++vol_idx) {
			for (size_t lnum = 0; lnum < volumes[vol_idx]->leb_count; ++lnum) {
				zassert_ok(ubi_leb_write(ubi, volumes_id[vol_idx], lnum,
							 wdata[wdata_idx], wdata_size[wdata_idx]));
				wdata_idx += 1;
			}
		}

		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(ubi, &info));
		zassert_equal(0, info.free_leb_count);
		zassert_equal(0, info.dirty_leb_count);

		rdata_idx = 0;
		for (size_t vol_idx = 0; vol_idx < ARRAY_SIZE(volumes_id); ++vol_idx) {
			for (size_t lnum = 0; lnum < volumes[vol_idx]->leb_count; ++lnum) {
				size_t size = 0;

				zassert_ok(ubi_leb_get_size(ubi, volumes_id[vol_idx], lnum, &size));
				zassert_equal(wdata_size[rdata_idx], size);

				uint8_t rdata[8192] = { 0 };
				zassert_ok(ubi_leb_read(ubi, volumes_id[vol_idx], lnum, 0, rdata,
							size));
				zassert_mem_equal(rdata, wdata[rdata_idx], wdata_size[rdata_idx],
						  "Memory blocks are not equal");

				rdata_idx += 1;
			}
		}

		erase_idx = 0;
		for (size_t vol_idx = 0; vol_idx < ARRAY_SIZE(volumes_id); ++vol_idx) {
			for (size_t lnum = 0; lnum < volumes[vol_idx]->leb_count; ++lnum) {
				zassert_ok(ubi_leb_unmap(ubi, volumes_id[vol_idx], lnum));
				zassert_ok(ubi_device_erase_peb(ubi));

				erase_idx += 1;
			}
		}

		zassert_equal(info.leb_total_count, wdata_idx);
		zassert_equal(wdata_idx, rdata_idx);
		zassert_equal(rdata_idx, erase_idx);
	}

	/* 4. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, (exp_ec_avr[ARRAY_SIZE(exp_ec_avr) - 1]) + 1);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 5. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	/* 6. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, (exp_ec_avr[ARRAY_SIZE(exp_ec_avr) - 1]) + 1);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);
}
