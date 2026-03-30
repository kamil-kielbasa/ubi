/**
 * \file    tests_ubi_stress.c
 *
 * \author  Kamil Kielbasa
 *
 * \brief   Stress tests for UBI. Run only on native_sim (flash simulator).
 *          These tests perform heavy flash I/O and must NOT run on real hardware
 *          to avoid premature flash wear.
 *
 * \version 0.9
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
#include <string.h>

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

/* Static function declarations ---------------------------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_after(void *ctx);

static void ztest_testcase_before(void *ctx);
static void ztest_testcase_teardown(void *ctx);

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

/* Module interface function definitions ------------------------------------------------------- */

ZTEST_SUITE(ubi_stress, NULL, ztest_suite_setup, ztest_testcase_before, ztest_testcase_teardown,
	    ztest_suite_after);

/**
 * \brief Verify that wear-leveling distributes erase cycles evenly across PEBs.
 *
 * \details Scenario: Create a static volume with 1 LEB. Perform 3×total_peb_count
 *          write-then-erase cycles to stress the greedy wear-leveling algorithm.
 *          Retrieve per-PEB erase counters via ubi_device_get_peb_ec().
 *
 * \expect The maximum erase counter across all PEBs deviates by at most 2 from
 *         the minimum, confirming even wear distribution.
 */
ZTEST(ubi_stress, wear_leveling_distribution)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "wl",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));

	const uint8_t data[] = { 0xCA, 0xFE };
	const size_t cycles = info.total_peb_count * 3;

	for (size_t i = 0; i < cycles; i++) {
		zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

		if (ubi_device_erase_peb(ubi) != 0) {
			break;
		}
	}

	/* Verify EC distribution: no PEB should deviate more than 2 from the min */
	size_t *peb_ec = NULL;
	size_t peb_ec_len = 0;
	zassert_ok(ubi_device_get_peb_ec(ubi, &peb_ec, &peb_ec_len));

	size_t min_ec = SIZE_MAX;
	size_t max_ec = 0;

	for (size_t i = 0; i < peb_ec_len; i++) {
		if (peb_ec[i] < min_ec) {
			min_ec = peb_ec[i];
		}
		if (peb_ec[i] > max_ec) {
			max_ec = peb_ec[i];
		}
	}

	zassert_true(max_ec <= min_ec + 2, "Wear-leveling imbalance: min_ec=%zu, max_ec=%zu",
		     min_ec, max_ec);

	/* Cross-check ec_avg: 41 erases / 14 data PEBs = 2. */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.ec_avg, 2, "ec_avg mismatch: got %zu, expected 2", info.ec_avg);

	k_free(peb_ec);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify no memory leaks during 50 write-read-unmap-erase cycles.
 *
 * \details Scenario: Create a static volume with 2 LEBs. Execute 50 iterations
 *          of: write to both LEBs, read back and verify, unmap both, erase all
 *          dirty PEBs. Measure heap free bytes before the first init and after
 *          the final deinit.
 *
 * \expect All write/read operations succeed in every cycle. Read-back data
 *         matches the written pattern. Heap free bytes are identical before
 *         and after, confirming no memory leak.
 */
ZTEST(ubi_stress, repeated_write_erase_cycles)
{
	struct sys_memory_stats mem_before = { 0 };
	struct sys_memory_stats mem_after = { 0 };

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &mem_before));

	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "stress",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t wdata[] = { 0x01, 0x02, 0x03, 0x04 };
	uint8_t rdata[4];

	for (size_t cycle = 0; cycle < 50; cycle++) {
		/* Write to both LEBs */
		zassert_ok(ubi_leb_write(ubi, vol_id, 0, wdata, sizeof(wdata)));
		zassert_ok(ubi_leb_write(ubi, vol_id, 1, wdata, sizeof(wdata)));

		/* Read back and verify */
		zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
		zassert_mem_equal(rdata, wdata, sizeof(wdata));
		zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, rdata, sizeof(rdata)));
		zassert_mem_equal(rdata, wdata, sizeof(wdata));

		/* Unmap and erase */
		zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
		zassert_ok(ubi_leb_unmap(ubi, vol_id, 1));

		/* Erase all dirty PEBs */
		struct ubi_device_info info;
		zassert_ok(ubi_device_get_info(ubi, &info));

		for (size_t d = 0; d < info.dirty_peb_count; d++) {
			zassert_ok(ubi_device_erase_peb(ubi));
		}
	}

	/* 50 cycles × 2 erases per cycle = 100 erases / 14 data PEBs = 7. */
	struct ubi_device_info final_info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &final_info));
	zassert_equal(final_info.ec_avg, 7, "ec_avg mismatch: got %zu, expected 7",
		      final_info.ec_avg);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &mem_after));
	zassert_equal(mem_before.free_bytes, mem_after.free_bytes, "Memory leak detected");
}

/**
 * \brief Verify behavior when the entire flash partition is full.
 *
 * \details Scenario: Create a static volume whose leb_count equals the total
 *          number of available PEBs (total_peb_count). Write the same 4-byte
 *          pattern to every LEB, consuming all free PEBs. Attempt one more
 *          write (overwrite LEB 0), which requires a free PEB that no longer
 *          exists. Read all LEBs back.
 *
 * \expect All initial writes succeed. The overwrite returns -ENOSPC.
 *         All previously written LEBs remain readable with correct data.
 */
ZTEST(ubi_stress, fill_entire_partition)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	struct ubi_device_info info;
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.ec_avg, 0, "Fresh device should have ec_avg=0");

	const struct ubi_volume_config cfg = {
		.name = "fill",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = info.total_peb_count,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xFF, 0x00, 0xAA, 0x55 };

	/* Fill all LEBs */
	for (size_t lnum = 0; lnum < info.total_peb_count; lnum++) {
		zassert_ok(ubi_leb_write(ubi, vol_id, lnum, data, sizeof(data)));
	}

	/* Next write should fail with -ENOSPC (no free PEBs for overwrite) */
	zassert_equal(-ENOSPC, ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)),
		      "Should fail when partition is full");

	/* Verify all data is intact */
	uint8_t rdata[4];

	for (size_t lnum = 0; lnum < info.total_peb_count; lnum++) {
		zassert_ok(ubi_leb_read(ubi, vol_id, lnum, 0, rdata, sizeof(rdata)));
		zassert_mem_equal(rdata, data, sizeof(data));
	}

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify data persistence and no memory leaks across 20 init/deinit cycles.
 *
 * \details Scenario: In the first cycle, create a static volume with 2 LEBs
 *          and write a 2-byte marker {0xAB, 0xCD} to LEB 0. In cycles 2..19,
 *          re-initialize the device and verify the marker persists: volume count
 *          is 1, and reading LEB 0 returns the original bytes. Measure heap
 *          usage before and after.
 *
 * \expect Every re-init cycle finds the volume and data intact. Heap free
 *         bytes are identical before the first init and after the last deinit.
 */
ZTEST(ubi_stress, multiple_init_deinit_cycles)
{
	struct sys_memory_stats mem_before = { 0 };
	struct sys_memory_stats mem_after = { 0 };

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &mem_before));

	for (size_t cycle = 0; cycle < 20; cycle++) {
		struct ubi_device *ubi = NULL;
		zassert_ok(ubi_device_init(&mtd, &ubi));

		if (cycle == 0) {
			const struct ubi_volume_config cfg = {
				.name = "persist",
				.type = UBI_VOLUME_TYPE_STATIC,
				.leb_count = 2,
			};
			int vol_id;
			zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

			const uint8_t data[] = { 0xAB, 0xCD };
			zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
		} else {
			/* Verify data persists across init/deinit cycles */
			struct ubi_device_info info;
			zassert_ok(ubi_device_get_info(ubi, &info));
			zassert_equal(1, info.volume_count);
			zassert_equal(0, info.ec_avg, "No erases performed, ec_avg should be 0");

			uint8_t rdata[2];
			zassert_ok(ubi_leb_read(ubi, 0, 0, 0, rdata, sizeof(rdata)));
			zassert_equal(0xAB, rdata[0]);
			zassert_equal(0xCD, rdata[1]);
		}

		zassert_ok(ubi_device_deinit(ubi));
	}

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &mem_after));
	zassert_equal(mem_before.free_bytes, mem_after.free_bytes, "Memory leak detected");
}
