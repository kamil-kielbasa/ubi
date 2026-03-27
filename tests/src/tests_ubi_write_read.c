/**
 * \file    tests_ubi_write_read.c
 *
 * \author  Kamil Kielbasa
 *
 * \brief   Hardware tests for Unsorted Block Images (UBI) write and read operations.
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

ZTEST_SUITE(ubi_write_read, NULL, ztest_suite_setup, ztest_testcase_before, ztest_testcase_teardown,
	    ztest_suite_after);

/**
 * \brief Verify that a single LEB write persists across a reboot.
 *
 * \details Scenario: Create a static volume with 4 LEBs. Write a 256-byte
 *          array to LEB 2. Read back and verify. Deinitialize, re-initialize,
 *          and read LEB 2 again.
 *
 * \expect Data is intact after reboot. free_peb_count decreases by 1 after
 *         the write. All erase counters are 0. Heap memory is reclaimed.
 */
ZTEST(ubi_write_read, one_volume_one_leb_operation_with_reboot)
{
	const size_t exp_ec_avr = 0;

	const struct ubi_volume_config vol_cfg_1 = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	struct ubi_device *ubi = NULL;
	struct ubi_device_info info_after_init = { 0 };
	struct ubi_device_info info_after_write = { 0 };

	int vol_id_1 = -1;

	/* 1. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	/* 2. Create volume */
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_1, &vol_id_1));

	zassert_ok(ubi_device_get_info(ubi, &info_after_init));
	zassert_equal(info_after_init.allocated_peb_count, vol_cfg_1.leb_count);
	zassert_equal(1, info_after_init.volume_count);

	/* 3. Write data to LEB */
	int lnum = 2;
	zassert_ok(ubi_leb_write(ubi, vol_id_1, lnum, array_256, ARRAY_SIZE(array_256)));

	/* 4. Read data from LEB */
	size_t rdata_size = 0;
	uint8_t rdata[ARRAY_SIZE(array_256)] = { 0 };

	zassert_ok(ubi_leb_get_size(ubi, vol_id_1, lnum, &rdata_size));
	zassert_equal(ARRAY_SIZE(array_256), rdata_size);

	zassert_ok(ubi_leb_read(ubi, vol_id_1, lnum, 0, rdata, rdata_size));
	zassert_mem_equal(rdata, array_256, ARRAY_SIZE(array_256), "Memory blocks are not equal");

	/* 5. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 6. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	/* 7. Read device info */
	zassert_ok(ubi_device_get_info(ubi, &info_after_write));
	zassert_equal(vol_cfg_1.leb_count, info_after_write.allocated_peb_count);
	zassert_equal(1, info_after_write.volume_count);
	zassert_equal(info_after_init.free_peb_count - 1, info_after_write.free_peb_count);

	/* 8. Read data from LEB */
	rdata_size = 0;
	memset(rdata, 0, sizeof(rdata));

	zassert_ok(ubi_leb_get_size(ubi, vol_id_1, lnum, &rdata_size));
	zassert_equal(ARRAY_SIZE(array_256), rdata_size);

	zassert_ok(ubi_leb_read(ubi, vol_id_1, lnum, 0, rdata, rdata_size));
	zassert_mem_equal(rdata, array_256, ARRAY_SIZE(array_256), "Memory blocks are not equal");

	/* 9. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);
}

/**
 * \brief Verify that multiple LEBs with varying data sizes persist across a
 *        reboot.
 *
 * \details Scenario: Create a static volume with 4 LEBs. Write different-sized
 *          patterns (10, 6, 4, 9 bytes) to each of the 4 LEBs. Deinitialize,
 *          re-initialize, and read all LEBs.
 *
 * \expect All four LEBs contain correct data after reboot. Each stored size
 *         matches the original write length.
 */
ZTEST(ubi_write_read, one_volume_many_leb_operations_with_reboot)
{
	const size_t exp_ec_avr = 0;

	const struct ubi_volume_config vol_cfg_1 = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	struct ubi_device *ubi = NULL;
	struct ubi_device_info info_after_init = { 0 };
	struct ubi_device_info info_after_write = { 0 };

	int vol_id_1 = -1;

	/* 1. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	/* 2. Create volume */
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_1, &vol_id_1));
	zassert_ok(ubi_device_get_info(ubi, &info_after_init));

	/* 3. Write data to LEB */
	const int leb[] = { 0, 1, 2, 3 };
	const uint8_t wdata[ARRAY_SIZE(leb)][16] = {
		{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 },
		{ 100, 101, 102, 103, 104, 105 },
		{ 197, 198, 199, 200 },
		{ 255, 254, 253, 252, 251, 250, 249, 248, 247 },
	};

	for (size_t i = 0; i < ARRAY_SIZE(leb); ++i)
		zassert_ok(ubi_leb_write(ubi, vol_id_1, leb[i], wdata[i], ARRAY_SIZE(wdata[i])));

	/* 4. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 5. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	/* 6. Read device info */
	zassert_ok(ubi_device_get_info(ubi, &info_after_write));
	zassert_equal(vol_cfg_1.leb_count, info_after_write.allocated_peb_count);
	zassert_equal(1, info_after_write.volume_count);
	zassert_equal(info_after_init.free_peb_count - ARRAY_SIZE(leb),
		      info_after_write.free_peb_count);

	/* 7. Read data from LEB */
	for (size_t i = 0; i < ARRAY_SIZE(leb); ++i) {
		size_t rdata_size = 0;
		zassert_ok(ubi_leb_get_size(ubi, vol_id_1, leb[i], &rdata_size));
		zassert_equal(ARRAY_SIZE(wdata[i]), rdata_size);

		uint8_t rdata[256] = { 0 };
		zassert_ok(ubi_leb_read(ubi, vol_id_1, leb[i], 0, rdata, rdata_size));
		zassert_mem_equal(rdata, wdata[i], ARRAY_SIZE(wdata[i]),
				  "Memory blocks are not equal");
	}

	/* 8. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);
}

/**
 * \brief Verify large-scale I/O across multiple volumes with varying payload
 *        sizes and reboot persistence.
 *
 * \details Scenario: Create three volumes (2, 4, 8 LEBs). Write 14 different
 *          arrays (sizes: 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048,
 *          4096, 8000 bytes) sequentially across all LEBs. Read all data back.
 *          Deinitialize, re-initialize, and read everything again.
 *
 * \expect All 14 data patterns are correct after initial write and after
 *         reboot. Heap memory is fully reclaimed after deinit.
 */
ZTEST(ubi_write_read, many_volumes_many_leb_operations_with_reboot)
{
	const size_t exp_ec_avr = 0;

	const struct ubi_volume_config vol_cfg_1 = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	const struct ubi_volume_config vol_cfg_2 = {
		.name = { '/', 'u', 'b', 'i', '_', '1' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	const struct ubi_volume_config vol_cfg_3 = {
		.name = { '/', 'u', 'b', 'i', '_', '2' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 8,
	};

	struct ubi_device *ubi = NULL;
	struct ubi_device_info info_after_init = { 0 };
	struct ubi_device_info info_after_write = { 0 };

	int vol_id_1 = -1;
	int vol_id_2 = -1;
	int vol_id_3 = -1;

	size_t wdata_idx = 0;
	size_t rdata_idx = 0;

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
	zassert_equal(ARRAY_SIZE(wdata),
		      vol_cfg_1.leb_count + vol_cfg_2.leb_count + vol_cfg_3.leb_count);
	zassert_equal(ARRAY_SIZE(wdata_size),
		      vol_cfg_1.leb_count + vol_cfg_2.leb_count + vol_cfg_3.leb_count);

	/* 1. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	/* 2. Create volume */
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_1, &vol_id_1));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_2, &vol_id_2));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_3, &vol_id_3));
	zassert_ok(ubi_device_get_info(ubi, &info_after_init));

	const int volumes_id[] = {
		vol_id_1,
		vol_id_2,
		vol_id_3,
	};

	const struct ubi_volume_config *volumes[] = {
		&vol_cfg_1,
		&vol_cfg_2,
		&vol_cfg_3,
	};

	/* 3. Write data to volumes LEBs */
	wdata_idx = 0;

	for (size_t vol_idx = 0; vol_idx < ARRAY_SIZE(volumes_id); ++vol_idx) {
		for (size_t lnum = 0; lnum < volumes[vol_idx]->leb_count; ++lnum) {
			zassert_ok(ubi_leb_write(ubi, volumes_id[vol_idx], lnum, wdata[wdata_idx],
						 wdata_size[wdata_idx]));
			wdata_idx += 1;
		}
	}

	/* 4. Read data from volumes LEBs */
	rdata_idx = 0;

	for (size_t vol_idx = 0; vol_idx < ARRAY_SIZE(volumes_id); ++vol_idx) {
		for (size_t lnum = 0; lnum < volumes[vol_idx]->leb_count; ++lnum) {
			size_t size = 0;

			zassert_ok(ubi_leb_get_size(ubi, volumes_id[vol_idx], lnum, &size));
			zassert_equal(wdata_size[rdata_idx], size);

			uint8_t rdata[8192] = { 0 };
			zassert_ok(ubi_leb_read(ubi, volumes_id[vol_idx], lnum, 0, rdata, size));
			zassert_mem_equal(rdata, wdata[rdata_idx], wdata_size[rdata_idx],
					  "Memory blocks are not equal");

			rdata_idx += 1;
		}
	}

	/* 5. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 6. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	/* 7. Read device info */
	zassert_ok(ubi_device_get_info(ubi, &info_after_write));
	zassert_equal(vol_cfg_1.leb_count + vol_cfg_2.leb_count + vol_cfg_3.leb_count,
		      info_after_write.allocated_peb_count);
	zassert_equal(3, info_after_write.volume_count);
	zassert_equal(info_after_init.free_peb_count - vol_cfg_1.leb_count - vol_cfg_2.leb_count -
			      vol_cfg_3.leb_count,
		      info_after_write.free_peb_count);

	/* 8. Read data from volumes LEBs */
	rdata_idx = 0;

	for (size_t vol_idx = 0; vol_idx < ARRAY_SIZE(volumes_id); ++vol_idx) {
		for (size_t lnum = 0; lnum < volumes[vol_idx]->leb_count; ++lnum) {
			size_t size = 0;

			zassert_ok(ubi_leb_get_size(ubi, volumes_id[vol_idx], lnum, &size));
			zassert_equal(wdata_size[rdata_idx], size);

			uint8_t rdata[8192] = { 0 };
			zassert_ok(ubi_leb_read(ubi, volumes_id[vol_idx], lnum, 0, rdata, size));
			zassert_mem_equal(rdata, wdata[rdata_idx], wdata_size[rdata_idx],
					  "Memory blocks are not equal");

			rdata_idx += 1;
		}
	}

	/* 9. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);
}

/**
 * \brief Verify non-aligned write sizes exercise the padding path and persist
 *        across a reboot.
 *
 * \details Scenario: Create a static volume with 4 LEBs. Write non-aligned
 *          sizes (5, 97, 271, 3907 bytes) to each LEB. These sizes are not
 *          multiples of WRITE_BLOCK_SIZE_ALIGNMENT (16 bytes), exercising the
 *          partial-block padding logic in ubi_leb_data_write(). Read back
 *          and verify. Deinitialize, re-initialize, and verify persistence.
 *
 * \expect All non-aligned writes succeed. Read-back data matches. Data
 *         persists across reboot with correct stored sizes.
 */
ZTEST(ubi_write_read, one_volume_many_lebs_io_operations_not_aligned_with_reboot)
{
	const size_t exp_ec_avr = 0;

	const struct ubi_volume_config vol_cfg_1 = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	struct ubi_device *ubi = NULL;
	struct ubi_device_info info_after_init = { 0 };
	struct ubi_device_info info_after_write = { 0 };

	int vol_id_1 = -1;

	const int leb[] = { 0, 1, 2, 3 };

	const uint8_t *wdata[] = {
		array_5,
		array_97,
		array_271,
		array_3907,
	};

	const size_t wdata_size[] = {
		ARRAY_SIZE(array_5),
		ARRAY_SIZE(array_97),
		ARRAY_SIZE(array_271),
		ARRAY_SIZE(array_3907),
	};

	zassert_equal(ARRAY_SIZE(wdata), ARRAY_SIZE(wdata_size));
	zassert_equal(ARRAY_SIZE(wdata), vol_cfg_1.leb_count);
	zassert_equal(ARRAY_SIZE(wdata_size), vol_cfg_1.leb_count);

	/* 1. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	/* 2. Create volume */
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_1, &vol_id_1));
	zassert_ok(ubi_device_get_info(ubi, &info_after_init));

	/* 3. Write data to volume LEBs */
	for (size_t i = 0; i < ARRAY_SIZE(leb); ++i)
		zassert_ok(ubi_leb_write(ubi, vol_id_1, leb[i], wdata[i], wdata_size[i]));

	/* 4. Read data from volume LEBs */
	for (size_t i = 0; i < ARRAY_SIZE(leb); ++i) {
		size_t rdata_size = 0;
		zassert_ok(ubi_leb_get_size(ubi, vol_id_1, leb[i], &rdata_size));
		zassert_equal(wdata_size[i], rdata_size);

		uint8_t rdata[8000] = { 0 };
		zassert_ok(ubi_leb_read(ubi, vol_id_1, leb[i], 0, rdata, rdata_size));
		zassert_mem_equal(rdata, wdata[i], wdata_size[i], "Memory blocks are not equal");
	}

	/* 5. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 6. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	/* 7. Read device info */
	zassert_ok(ubi_device_get_info(ubi, &info_after_write));
	zassert_equal(vol_cfg_1.leb_count, info_after_write.allocated_peb_count);
	zassert_equal(1, info_after_write.volume_count);
	zassert_equal(info_after_init.free_peb_count - ARRAY_SIZE(leb),
		      info_after_write.free_peb_count);

	/* 8. Read data from volume LEBs */
	for (size_t i = 0; i < ARRAY_SIZE(leb); ++i) {
		size_t rdata_size = 0;
		zassert_ok(ubi_leb_get_size(ubi, vol_id_1, leb[i], &rdata_size));
		zassert_equal(wdata_size[i], rdata_size);

		uint8_t rdata[8000] = { 0 };
		zassert_ok(ubi_leb_read(ubi, vol_id_1, leb[i], 0, rdata, rdata_size));
		zassert_mem_equal(rdata, wdata[i], wdata_size[i], "Memory blocks are not equal");
	}

	/* 9. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);
}

/**
 * \brief Verify that ubi_leb_read rejects reads beyond data_size.
 *
 * \details Write 4 bytes to a LEB, then attempt to read 8 bytes starting
 *          from offset 0. The VID header stores data_size=4, so the read
 *          should fail because offset + len > data_size.
 *
 * \expect ubi_leb_read returns -EINVAL for the oversized read.
 *         A read within bounds succeeds.
 */
ZTEST(ubi_write_read, leb_read_rejects_beyond_data_size)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "dsize",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xAA, 0xBB, 0xCC, 0xDD };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	/* Read within bounds should succeed */
	uint8_t rdata[4] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, data, sizeof(data));

	/* Read beyond data_size should fail */
	uint8_t oversized[8] = { 0 };
	const int ret = ubi_leb_read(ubi, vol_id, 0, 0, oversized, sizeof(oversized));
	zassert_equal(-EINVAL, ret, "Read beyond data_size should return -EINVAL (got %d)", ret);

	/* Read with offset beyond data_size should also fail */
	uint8_t small[1] = { 0 };
	const int ret2 = ubi_leb_read(ubi, vol_id, 0, 4, small, sizeof(small));
	zassert_equal(-EINVAL, ret2, "Read at offset=data_size should return -EINVAL (got %d)",
		      ret2);

	zassert_ok(ubi_device_deinit(ubi));
}
