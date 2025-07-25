/**
 * \file    tests.c
 * \author  Kamil Kielbasa
 * \brief   Hardware tests for Unsorted Block Images (UBI) implementation.
 * \version 0.1
 * \date    2025-07-25
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files ----------------------------------------------------------- */

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

/* Module defines ---------------------------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* Module types and type definitiones -------------------------------------- */
/* Module interface variables and constants -------------------------------- */
/* Static variables and constants ------------------------------------------ */

static struct mem_tech_device mtd = { 0 };

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
extern struct sys_heap _system_heap;
#endif

static const uint8_t rdata[256] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
	0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D,
	0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C,
	0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B,
	0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A,
	0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
	0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
	0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
	0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86,
	0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
	0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4,
	0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3,
	0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0, 0xC1, 0xC2,
	0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF, 0xD0, 0xD1,
	0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE0,
	0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF,
	0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE,
	0xFF
};

/* Static function declarations -------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_after(void *ctx);

static void ztest_testcase_before(void *ctx);
static void ztest_testcase_teardown(void *ctx);

/* Static function definitions --------------------------------------------- */

static void *ztest_suite_setup(void)
{
	const struct device *flash_dev = UBI_PARTITION_DEVICE;
	zassert_true(device_is_ready(flash_dev));

	struct flash_pages_info page_info = { 0 };
	zassert_ok(flash_get_page_info_by_offs(flash_dev, 0, &page_info));

	const size_t write_block_size = flash_get_write_block_size(flash_dev);
	const size_t erase_block_size = page_info.size;

	mtd.dev = flash_dev;
	mtd.p_off = UBI_PARTITION_OFFSET;
	mtd.p_size = UBI_PARTITION_SIZE;
	mtd.eb_size = erase_block_size;
	mtd.wb_size = write_block_size;

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

/* Module interface function definitions ----------------------------------- */

ZTEST_SUITE(ubi, NULL, ztest_suite_setup, ztest_testcase_before, ztest_testcase_teardown,
	    ztest_suite_after);

ZTEST(ubi, init_deinit)
{
	zassert_ok(ubi_init(&mtd));

	struct ubi_device_info dev_info = { 0 };
	zassert_ok(ubi_info(&dev_info, NULL));

	zassert_equal(0, dev_info.alloc_pebs);
	zassert_equal(0, dev_info.dirty_pebs);
	zassert_equal(0, dev_info.bad_pebs);
	zassert_not_equal(0, dev_info.free_pebs);

	const size_t nr_of_pebs = mtd.p_size / mtd.eb_size;
	zassert_not_equal(0, dev_info.leb_count);
	zassert_not_equal(nr_of_pebs, dev_info.leb_count);

	zassert_not_equal(0, dev_info.leb_size);
	zassert_not_equal(mtd.eb_size, dev_info.leb_size);

	zassert_ok(ubi_deinit());
}

ZTEST(ubi, multiple_init_deinit)
{
	zassert_ok(ubi_init(&mtd));
	zassert_ok(ubi_deinit());
	zassert_ok(ubi_init(&mtd));
	zassert_ok(ubi_deinit());
	zassert_ok(ubi_init(&mtd));
	zassert_ok(ubi_deinit());
}

ZTEST(ubi, init_deinit_with_heap_checks)
{
	struct sys_memory_stats stat_before_init = { 0 };
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &stat_before_init));

	zassert_ok(ubi_init(&mtd));

	struct sys_memory_stats stat_after_init = { 0 };
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &stat_after_init));

	zassert_ok(ubi_deinit());

	struct sys_memory_stats stat_after_deinit = { 0 };
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &stat_after_deinit));

	zassert_equal(stat_before_init.free_bytes, stat_after_deinit.free_bytes);
	zassert_equal(stat_before_init.allocated_bytes, stat_after_deinit.allocated_bytes);
	zassert_equal(stat_before_init.max_allocated_bytes, stat_after_deinit.max_allocated_bytes);

	zassert_not_equal(stat_after_init.free_bytes, stat_after_deinit.free_bytes);
	zassert_not_equal(stat_after_init.allocated_bytes, stat_after_deinit.allocated_bytes);
	zassert_equal(stat_after_init.max_allocated_bytes, stat_after_deinit.max_allocated_bytes);
}

ZTEST(ubi, map_and_unmap)
{
	zassert_ok(ubi_init(&mtd));

	struct ubi_device_info dev_info = { 0 };
	zassert_ok(ubi_info(&dev_info, NULL));

	for (size_t lnum = 0; lnum < dev_info.leb_count; ++lnum) {
		zassert_ok(ubi_leb_map(lnum));
	}

	zassert_ok(ubi_info(&dev_info, NULL));
	zassert_equal(dev_info.leb_count, dev_info.alloc_pebs);
	zassert_equal(0, dev_info.free_pebs);
	zassert_equal(0, dev_info.dirty_pebs);
	zassert_equal(0, dev_info.bad_pebs);

	for (size_t lnum = 0; lnum < dev_info.leb_count; ++lnum) {
		zassert_ok(ubi_leb_unmap(lnum));
	}

	zassert_ok(ubi_info(&dev_info, NULL));
	zassert_equal(0, dev_info.alloc_pebs);
	zassert_equal(0, dev_info.free_pebs);
	zassert_equal(dev_info.leb_count, dev_info.dirty_pebs);
	zassert_equal(0, dev_info.bad_pebs);

	zassert_ok(ubi_deinit());
}

ZTEST(ubi, map_unmap_erase_map)
{
	zassert_ok(ubi_init(&mtd));

	struct ubi_device_info dev_info = { 0 };
	zassert_ok(ubi_info(&dev_info, NULL));

	for (size_t lnum = 0; lnum < dev_info.leb_count; ++lnum) {
		zassert_ok(ubi_leb_map(lnum));
	}

	zassert_ok(ubi_info(&dev_info, NULL));
	zassert_equal(dev_info.leb_count, dev_info.alloc_pebs);
	zassert_equal(0, dev_info.free_pebs);
	zassert_equal(0, dev_info.dirty_pebs);
	zassert_equal(0, dev_info.bad_pebs);

	for (size_t lnum = 0; lnum < dev_info.leb_count; ++lnum) {
		zassert_ok(ubi_leb_unmap(lnum));
	}

	zassert_ok(ubi_info(&dev_info, NULL));
	zassert_equal(0, dev_info.alloc_pebs);
	zassert_equal(0, dev_info.free_pebs);
	zassert_equal(dev_info.leb_count, dev_info.dirty_pebs);
	zassert_equal(0, dev_info.bad_pebs);

	while (dev_info.dirty_pebs > 0) {
		zassert_ok(ubi_peb_erase());
		zassert_ok(ubi_info(&dev_info, NULL));
	}

	zassert_ok(ubi_info(&dev_info, NULL));
	zassert_equal(0, dev_info.alloc_pebs);
	zassert_equal(dev_info.leb_count, dev_info.free_pebs);
	zassert_equal(0, dev_info.dirty_pebs);
	zassert_equal(0, dev_info.bad_pebs);

	for (size_t lnum = 0; lnum < dev_info.leb_count; ++lnum) {
		zassert_ok(ubi_leb_map(lnum));
	}

	zassert_ok(ubi_info(&dev_info, NULL));
	zassert_equal(dev_info.leb_count, dev_info.alloc_pebs);
	zassert_equal(0, dev_info.free_pebs);
	zassert_equal(0, dev_info.dirty_pebs);
	zassert_equal(0, dev_info.bad_pebs);

	zassert_ok(ubi_deinit());
}

ZTEST(ubi, write_read_small_buffer)
{
	zassert_ok(ubi_init(&mtd));

	const uint8_t buf[32] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA,
				  0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F,
				  0x70, 0x81, 0x92, 0xA3, 0xB4, 0xC5, 0xD6, 0xE7, 0xF8, 0x09 };

	const size_t lnum = 0;
	zassert_ok(ubi_leb_write(lnum, buf, ARRAY_SIZE(buf)));

	const size_t offset = 0;

	size_t len = 0;
	uint8_t output[ARRAY_SIZE(buf)] = { 0 };
	zassert_ok(ubi_leb_read(lnum, output, offset, &len));

	zassert_equal(ARRAY_SIZE(buf), len);
	zassert_mem_equal(buf, output, ARRAY_SIZE(buf), "Memory blocks are not equal");

	zassert_ok(ubi_deinit());
}

ZTEST(ubi, write_read_max_buffer)
{
	zassert_ok(ubi_init(&mtd));

	struct ubi_device_info dev_info = { 0 };
	zassert_ok(ubi_info(&dev_info, NULL));

	uint8_t buf[dev_info.leb_size];
	memset(buf, 0, sizeof(buf));

	for (size_t i = 0; i < ARRAY_SIZE(buf); ++i)
		buf[i] = (uint8_t)i;

	const size_t lnum = dev_info.leb_count - 1;
	zassert_ok(ubi_leb_write(lnum, buf, ARRAY_SIZE(buf)));

	const size_t offset = 0;

	size_t len = 0;
	uint8_t output[dev_info.leb_size];
	memset(output, 0, sizeof(output));
	zassert_ok(ubi_leb_read(lnum, output, offset, &len));

	zassert_equal(ARRAY_SIZE(buf), len);
	zassert_mem_equal(buf, output, ARRAY_SIZE(buf), "Memory blocks are not equal");

	zassert_ok(ubi_deinit());
}

ZTEST(ubi, full_scenario)
{
	zassert_ok(ubi_init(&mtd));

	struct ubi_device_info dev_info = { 0 };
	zassert_ok(ubi_info(&dev_info, NULL));

	const size_t map_lnums[] = { 0, 1, 2, 3, 4, 5 };

	for (size_t i = 0; i < ARRAY_SIZE(map_lnums); ++i) {
		zassert_ok(ubi_leb_map(map_lnums[i]));

		bool is_map = false;
		zassert_ok(ubi_leb_is_mapped(map_lnums[i], &is_map));
		zassert_true(is_map);
	}

	const size_t wr_lnums[] = { dev_info.leb_count / 2 + 0, dev_info.leb_count / 2 + 1,
				    dev_info.leb_count / 2 + 2, dev_info.leb_count / 2 + 3 };
	const size_t wr_sizes[] = { 32, 64, 128, 256 };

	for (size_t i = 0; i < ARRAY_SIZE(wr_lnums); ++i) {
		zassert_ok(ubi_leb_write(wr_lnums[i], rdata, wr_sizes[i]));

		bool is_map = false;
		zassert_ok(ubi_leb_is_mapped(wr_lnums[i], &is_map));
		zassert_true(is_map);
	}

	zassert_ok(ubi_info(&dev_info, NULL));
	zassert_equal(dev_info.alloc_pebs, ARRAY_SIZE(map_lnums) + ARRAY_SIZE(wr_lnums));
	zassert_equal(dev_info.free_pebs,
		      dev_info.leb_count - ARRAY_SIZE(map_lnums) - ARRAY_SIZE(wr_lnums));
	zassert_equal(0, dev_info.dirty_pebs);
	zassert_equal(0, dev_info.bad_pebs);

	for (size_t i = 0; i < ARRAY_SIZE(map_lnums); ++i) {
		const size_t offset = 0;
		uint8_t buf[ARRAY_SIZE(rdata)] = { 0 };
		size_t len = 0;

		zassert_ok(ubi_leb_read(map_lnums[i], buf, offset, &len));
		zassert_equal(0, len);
	}

	for (size_t i = 0; i < ARRAY_SIZE(wr_lnums); ++i) {
		const size_t offset = 0;
		uint8_t buf[ARRAY_SIZE(rdata)] = { 0 };
		size_t len = 0;

		zassert_ok(ubi_leb_read(wr_lnums[i], buf, offset, &len));
		zassert_equal(wr_sizes[i], len);
		zassert_mem_equal(rdata, buf, len, "Memory blocks are not equal");
	}

	zassert_ok(ubi_deinit());
}

ZTEST(ubi, flash_equal_weariness)
{
	size_t reserved_pebs = 0;
	struct ubi_device_info dev_info = { 0 };
	struct ubi_flash_info flash_info = { 0 };

	zassert_ok(ubi_init(&mtd));

	const size_t cycles = 5;
	const size_t cycle_exp_peb_ec[] = { 0, 1, 2, 3, 4 };
	const size_t cycle_exp_ec_avr[] = { 0, 1, 2, 3, 4 };

	/* Cycle 1: */
	for (size_t cycle = 0; cycle < cycles; ++cycle) {
		zassert_ok(ubi_info(&dev_info, &flash_info));
		zassert_equal(cycle_exp_ec_avr[cycle], flash_info.ec_average);
		zassert_not_equal(0, flash_info.peb_count);

		for (size_t pnum = 0; pnum < flash_info.peb_count; ++pnum) {
			if (flash_info.peb_init[pnum]) {
				zassert_equal(cycle_exp_peb_ec[cycle], flash_info.peb_ec[pnum]);
			} else {
				reserved_pebs += 1;
			}
		}
		zassert_equal(1, reserved_pebs);

		k_free(flash_info.peb_init);
		k_free(flash_info.peb_ec);

		for (size_t lnum = 0; lnum < dev_info.leb_count; ++lnum) {
			zassert_ok(ubi_leb_map(lnum));
			zassert_ok(ubi_leb_unmap(lnum));
			zassert_ok(ubi_peb_erase());
		}

		memset(&dev_info, 0, sizeof(dev_info));
		memset(&flash_info, 0, sizeof(flash_info));
		reserved_pebs = 0;
	}

	zassert_ok(ubi_deinit());
}
