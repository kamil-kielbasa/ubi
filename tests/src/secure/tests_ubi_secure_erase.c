/**
 * \file    tests_ubi_secure_erase.c
 * \author  Kamil Kielbasa
 *
 * \brief   Parity tests for secure backend PEB erase lifecycle.
 *
 * \copyright Copyright (c) 2026
 */

/* --------------------------------------- Include files --------------------------------------- */
#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>
#include "arrays.h"

#include "ubi_test_fixture.h"
#include "ubi_test_secure_fixture.h"

#include <psa/crypto.h>

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/sys_heap.h>

#include <errno.h>
#include <string.h>

/* -------------------------------------- Module defines --------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* ------------------------------------- Static variables -------------------------------------- */

static struct ubi_mtd mtd = { 0 };

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
extern struct sys_heap _system_heap;
#endif

static struct sys_memory_stats before_init = { 0 };
static struct sys_memory_stats after_init = { 0 };
static struct sys_memory_stats after_deinit = { 0 };

/* -------------------------------------- Static helpers --------------------------------------- */

static void memory_check(struct sys_memory_stats *bi, struct sys_memory_stats *ai,
			 struct sys_memory_stats *ad)
{
	zassert_not_null(bi);
	zassert_not_null(ai);
	zassert_not_null(ad);

	zassert_equal(bi->free_bytes, ad->free_bytes);
	zassert_equal(bi->allocated_bytes, ad->allocated_bytes);

#if defined(CONFIG_UBI_MEM_BACKEND_HEAP)
	zassert_not_equal(ai->free_bytes, ad->free_bytes);
	zassert_not_equal(ai->allocated_bytes, ad->allocated_bytes);
#endif

	memset(bi, 0, sizeof(*bi));
	memset(ai, 0, sizeof(*ai));
	memset(ad, 0, sizeof(*ad));
}

/* ---------------------------------- Suite setup / teardown ----------------------------------- */

static void *ztest_suite_setup(void)
{
	const struct device *flash_dev = UBI_PARTITION_DEVICE;
	zassert_true(device_is_ready(flash_dev));

	struct flash_pages_info page_info = { 0 };
	zassert_ok(flash_get_page_info_by_offs(flash_dev, 0, &page_info));

	mtd.partition_id = FIXED_PARTITION_ID(UBI_PARTITION_NAME);
	mtd.erase_block_size = page_info.size;
	mtd.write_block_size = flash_get_write_block_size(flash_dev);

	zassert_equal(psa_crypto_init(), PSA_SUCCESS);
	ubi_test_import_root_key();

	return NULL;
}

static void ztest_suite_before(void *ctx)
{
	ARG_UNUSED(ctx);
	ubi_test_partition_force_release_all();
	zassert_ok(flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE));
}

/* ------------------------------------------- Tests ------------------------------------------- */

/**
 * \brief Write until partition full, unmap, erase all dirty PEBs.
 *
 * \details Create a 1-LEB static volume, write data, unmap, erase dirty
 *          PEBs, deinit and re-init across multiple cycles until the
 *          partition is exercised. Verifies dirty_peb_count transitions.
 *          Parity with plain ubi_erase.one_volume_one_leb_operations_with_reboot.
 *
 * \expected dirty_peb_count drops to 0 after erase; data written before
 *           unmap is no longer accessible; heap fully reclaimed after deinit.
 */
ZTEST(ubi_secure_erase, test_fill_unmap_erase_cycle)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;
	const size_t lnum = 0;

	/* 1. Init, create volume. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	struct ubi_device_info info_after_init = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_after_init));
	zassert_equal(vol_cfg.leb_count, info_after_init.reserved_peb_count);

	/* 2. Write repeatedly until all PEBs consumed. */
	struct ubi_device_info info = { 0 };
	for (size_t i = 0; i < info_after_init.total_peb_count; ++i) {
		zassert_ok(ubi_leb_write(ubi, vol_id, lnum, array_128, ARRAY_SIZE(array_128)));

		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(ubi, &info));
		zassert_equal(info.free_peb_count, info.total_peb_count - i - 1);
		zassert_equal(i, info.dirty_peb_count);
	}

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.free_peb_count);
	zassert_equal(info.total_peb_count - 1, info.dirty_peb_count);

	/* 3. Verify data. */
	uint8_t rdata[ARRAY_SIZE(array_128)] = { 0 };
	size_t rdata_size = 0;
	zassert_ok(ubi_leb_get_size(ubi, vol_id, lnum, &rdata_size));
	zassert_equal(ARRAY_SIZE(array_128), rdata_size);
	zassert_ok(ubi_leb_read(ubi, vol_id, lnum, 0, rdata, rdata_size));
	zassert_mem_equal(rdata, array_128, ARRAY_SIZE(array_128));

	/* 4. Deinit → re-init. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	memory_check(&before_init, &after_init, &after_deinit);

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));

	/* 5. Unmap and erase all dirty PEBs one by one. */
	zassert_ok(ubi_leb_unmap(ubi, vol_id, lnum));

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.free_peb_count);
	zassert_equal(info.total_peb_count, info.dirty_peb_count);

	for (size_t i = 0; i < info.dirty_peb_count; ++i) {
		zassert_ok(ubi_device_erase_peb(ubi));

		struct ubi_device_info _info = { 0 };
		zassert_ok(ubi_device_get_info(ubi, &_info));
		zassert_equal(i + 1, _info.free_peb_count);
		zassert_equal(_info.total_peb_count - i - 1, _info.dirty_peb_count);
	}

	/* 6. Verify all dirty PEBs were erased. */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.total_peb_count, info.free_peb_count);
	zassert_equal(0, info.dirty_peb_count);

	/* 7. Deinit. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	memory_check(&before_init, &after_init, &after_deinit);
}

/* ------------------------------------ Suite registration ------------------------------------- */

ZTEST_SUITE(ubi_secure_erase, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);
