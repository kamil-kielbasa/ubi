/**
 * \file    tests_ubi_map_unmap.c
 *
 * \author  Kamil Kielbasa
 *
 * \brief   Hardware tests for Unsorted Block Images (UBI) map and unmap operations.
 *
 * \version 0.5
 * \date    2025-09-25
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

ZTEST_SUITE(ubi_map, NULL, ztest_suite_setup, ztest_testcase_before, ztest_testcase_teardown,
	    ztest_suite_after);

ZTEST(ubi_map, one_volume_with_one_leb_operation_with_reboot)
{
	const size_t exp_ec_avr = 0;

	const struct ubi_volume_config vol_cfg_1 = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	struct ubi_device_info info_after_init = { 0 };
	struct ubi_device_info info_after_map = { 0 };
	struct ubi_device_info info_after_unmap = { 0 };

	struct ubi_device *ubi = NULL;
	int vol_id_1 = -1;

	/* 1. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	zassert_ok(ubi_device_get_info(ubi, &info_after_init));

	/* 2. Create volume */
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_1, &vol_id_1));

	/* 3. Verify if LEBs are not mapped */
	for (size_t lnum = 0; lnum < vol_cfg_1.leb_count; ++lnum) {
		bool is_mapped = true;
		zassert_ok(ubi_leb_is_mapped(ubi, vol_id_1, lnum, &is_mapped));
		zassert_false(is_mapped);
	}

	/* 4. Map LEB with extra checks */
	size_t lnum = 0;
	zassert_ok(ubi_leb_map(ubi, vol_id_1, lnum));

	bool is_mapped = false;
	zassert_ok(ubi_leb_is_mapped(ubi, vol_id_1, lnum, &is_mapped));
	zassert_true(is_mapped);

	size_t size = 1;
	zassert_ok(ubi_leb_get_size(ubi, vol_id_1, lnum, &size));
	zassert_equal(0, size);

	zassert_ok(ubi_device_get_info(ubi, &info_after_map));
	zassert_equal(info_after_map.free_leb_count, info_after_init.free_leb_count - 1);

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

	/* 7. Unmap LEB */
	zassert_ok(ubi_leb_unmap(ubi, vol_id_1, lnum));

	/* 8. Verify device infos */
	zassert_ok(ubi_device_get_info(ubi, &info_after_unmap));
	zassert_equal(info_after_unmap.free_leb_count, info_after_init.free_leb_count - 1);
	zassert_equal(0, info_after_map.dirty_leb_count);
	zassert_equal(1, info_after_unmap.dirty_leb_count);

	/* 9. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 10. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	/* 11. Verify device infos */
	zassert_ok(ubi_device_get_info(ubi, &info_after_unmap));
	zassert_equal(info_after_unmap.free_leb_count, info_after_init.free_leb_count - 1);
	zassert_equal(0, info_after_map.dirty_leb_count);
	zassert_equal(0, info_after_unmap.dirty_leb_count);

	/* 12. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);
}

ZTEST(ubi_map, one_volume_with_many_lebs_operations_with_reboot)
{
	const size_t exp_ec_avr = 0;

	const struct ubi_volume_config vol_cfg_1 = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	struct ubi_device_info info_after_init = { 0 };
	struct ubi_device_info info_after_map = { 0 };
	struct ubi_device_info info_after_unmap = { 0 };

	struct ubi_device *ubi = NULL;
	int vol_id_1 = -1;

	/* 1. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	zassert_ok(ubi_device_get_info(ubi, &info_after_init));

	/* 2. Create volume */
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_1, &vol_id_1));

	/* 3. Verify if LEBs are not mapped */
	for (size_t lnum = 0; lnum < vol_cfg_1.leb_count; ++lnum) {
		bool is_mapped = true;
		zassert_ok(ubi_leb_is_mapped(ubi, vol_id_1, lnum, &is_mapped));
		zassert_false(is_mapped);
	}

	/* 4. Map LEB with extra checks */
	for (size_t lnum = 0; lnum < vol_cfg_1.leb_count; ++lnum) {
		bool is_mapped = true;
		zassert_ok(ubi_leb_is_mapped(ubi, vol_id_1, lnum, &is_mapped));
		zassert_false(is_mapped);
	}

	const size_t lnum[] = { 0, 1, 2, 3 };
	for (size_t i = 0; i < ARRAY_SIZE(lnum); ++i)
		zassert_ok(ubi_leb_map(ubi, vol_id_1, lnum[i]));

	for (size_t i = 0; i < ARRAY_SIZE(lnum); ++i) {
		bool is_mapped = false;
		zassert_ok(ubi_leb_is_mapped(ubi, vol_id_1, lnum[i], &is_mapped));
		zassert_true(is_mapped);
	}

	for (size_t i = 0; i < ARRAY_SIZE(lnum); ++i) {
		size_t size = 1;
		zassert_ok(ubi_leb_get_size(ubi, vol_id_1, lnum[i], &size));
		zassert_equal(0, size);
	}

	zassert_ok(ubi_device_get_info(ubi, &info_after_map));
	zassert_equal(info_after_map.free_leb_count,
		      info_after_init.free_leb_count - ARRAY_SIZE(lnum));

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

	/* 7. Unmap LEBs */
	for (size_t i = 0; i < ARRAY_SIZE(lnum); ++i)
		zassert_ok(ubi_leb_unmap(ubi, vol_id_1, lnum[i]));

	/* 8. Verify device infos */
	zassert_ok(ubi_device_get_info(ubi, &info_after_unmap));
	zassert_equal(info_after_unmap.free_leb_count,
		      info_after_init.free_leb_count - ARRAY_SIZE(lnum));
	zassert_equal(0, info_after_map.dirty_leb_count);
	zassert_equal(ARRAY_SIZE(lnum), info_after_unmap.dirty_leb_count);

	/* 9. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 10. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	/* 11. Verify device infos */
	zassert_ok(ubi_device_get_info(ubi, &info_after_unmap));
	zassert_equal(info_after_unmap.free_leb_count,
		      info_after_init.free_leb_count - ARRAY_SIZE(lnum));
	zassert_equal(0, info_after_map.dirty_leb_count);
	zassert_equal(0, info_after_unmap.dirty_leb_count);

	/* 12. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);
}

ZTEST(ubi_map, many_volumes_with_many_lebs_operations_with_reboot)
{
	const size_t exp_ec_avr = 0;

	const struct ubi_volume_config vol_cfg_1 = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	const struct ubi_volume_config vol_cfg_2 = {
		.name = { '/', 'u', 'b', 'i', '_', '1' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 8,
	};

	struct ubi_device_info info_after_init = { 0 };
	struct ubi_device_info info_after_map = { 0 };
	struct ubi_device_info info_after_unmap = { 0 };

	struct ubi_device *ubi = NULL;
	int vol_id_1 = -1;
	int vol_id_2 = -1;

	/* 1. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	zassert_ok(ubi_device_get_info(ubi, &info_after_init));

	/* 2. Create volume */
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_1, &vol_id_1));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_2, &vol_id_2));

	/* 3. Verify if LEBs are not mapped */
	for (size_t lnum = 0; lnum < vol_cfg_1.leb_count; ++lnum) {
		bool is_mapped = true;
		zassert_ok(ubi_leb_is_mapped(ubi, vol_id_1, lnum, &is_mapped));
		zassert_false(is_mapped);
	}

	for (size_t lnum = 0; lnum < vol_cfg_2.leb_count; ++lnum) {
		bool is_mapped = true;
		zassert_ok(ubi_leb_is_mapped(ubi, vol_id_2, lnum, &is_mapped));
		zassert_false(is_mapped);
	}

	/* 4. Map LEB with extra checks */
	const size_t lnum_1[] = { 0, 1, 2, 3 };
	const size_t lnum_2[] = { 0, 1, 2, 3, 4, 6 };

	for (size_t i = 0; i < ARRAY_SIZE(lnum_1); ++i)
		zassert_ok(ubi_leb_map(ubi, vol_id_1, lnum_1[i]));

	for (size_t i = 0; i < ARRAY_SIZE(lnum_2); ++i)
		zassert_ok(ubi_leb_map(ubi, vol_id_2, lnum_2[i]));

	for (size_t i = 0; i < ARRAY_SIZE(lnum_1); ++i) {
		bool is_mapped = false;
		zassert_ok(ubi_leb_is_mapped(ubi, vol_id_1, lnum_1[i], &is_mapped));
		zassert_true(is_mapped);
	}

	for (size_t i = 0; i < ARRAY_SIZE(lnum_2); ++i) {
		bool is_mapped = false;
		zassert_ok(ubi_leb_is_mapped(ubi, vol_id_2, lnum_2[i], &is_mapped));
		zassert_true(is_mapped);
	}

	for (size_t i = 0; i < ARRAY_SIZE(lnum_1); ++i) {
		size_t size = 1;
		zassert_ok(ubi_leb_get_size(ubi, vol_id_1, lnum_1[i], &size));
		zassert_equal(0, size);
	}

	for (size_t i = 0; i < ARRAY_SIZE(lnum_2); ++i) {
		size_t size = 1;
		zassert_ok(ubi_leb_get_size(ubi, vol_id_2, lnum_2[i], &size));
		zassert_equal(0, size);
	}

	zassert_ok(ubi_device_get_info(ubi, &info_after_map));
	zassert_equal(info_after_map.free_leb_count,
		      info_after_init.free_leb_count - ARRAY_SIZE(lnum_1) - ARRAY_SIZE(lnum_2));

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

	/* 7. Unmap LEBs */
	for (size_t i = 0; i < ARRAY_SIZE(lnum_1); ++i)
		zassert_ok(ubi_leb_unmap(ubi, vol_id_1, lnum_1[i]));

	for (size_t i = 0; i < ARRAY_SIZE(lnum_2); ++i)
		zassert_ok(ubi_leb_unmap(ubi, vol_id_2, lnum_2[i]));

	/* 8. Verify device infos */
	zassert_ok(ubi_device_get_info(ubi, &info_after_unmap));
	zassert_equal(info_after_map.free_leb_count,
		      info_after_init.free_leb_count - ARRAY_SIZE(lnum_1) - ARRAY_SIZE(lnum_2));
	zassert_equal(0, info_after_map.dirty_leb_count);
	zassert_equal(ARRAY_SIZE(lnum_1) + ARRAY_SIZE(lnum_2), info_after_unmap.dirty_leb_count);

	/* 9. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 10. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));
	zassert_not_null(ubi);

	/* 11. Verify device infos */
	zassert_ok(ubi_device_get_info(ubi, &info_after_unmap));
	zassert_equal(info_after_map.free_leb_count,
		      info_after_init.free_leb_count - ARRAY_SIZE(lnum_1) - ARRAY_SIZE(lnum_2));
	zassert_equal(0, info_after_map.dirty_leb_count);
	zassert_equal(0, info_after_unmap.dirty_leb_count);

	/* 12. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);
}
