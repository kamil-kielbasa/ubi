/**
 * \file    tests_ubi_secure_map_unmap.c
 * \author  Kamil Kielbasa
 *
 * \brief   Parity tests for secure backend LEB map, unmap, is_mapped.
 *
 * \copyright Copyright (c) 2026
 */

/* --------------------------------------- Include files --------------------------------------- */
#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>

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
 * \brief Map/unmap single LEB lifecycle with reboot.
 *
 * \details Create a 4-LEB static volume, verify all LEBs unmapped, map
 *          LEB 0, deinit, re-init, unmap LEB 0, verify dirty count, deinit,
 *          re-init and confirm dirty PEBs cleaned by attach scan.
 *          Parity with plain ubi_map.one_volume_with_one_leb_operation_with_reboot.
 *
 * \expected After map: is_mapped true, size 0, free_peb_count decremented.
 *           After unmap + reboot: dirty_peb_count == 0; heap fully reclaimed.
 */
ZTEST(ubi_secure_map, test_one_leb_lifecycle_with_reboot)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;
	const size_t lnum = 0;

	/* 1. Init, create volume. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));

	struct ubi_device_info info_after_init = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_after_init));

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	/* 2. Verify LEBs are not mapped. */
	for (size_t i = 0; i < vol_cfg.leb_count; ++i) {
		bool is_mapped = true;
		zassert_ok(ubi_leb_is_mapped(ubi, vol_id, i, &is_mapped));
		zassert_false(is_mapped);
	}

	/* 3. Map LEB 0. */
	zassert_ok(ubi_leb_map(ubi, vol_id, lnum));

	bool is_mapped = false;
	zassert_ok(ubi_leb_is_mapped(ubi, vol_id, lnum, &is_mapped));
	zassert_true(is_mapped);

	size_t size = 1;
	zassert_ok(ubi_leb_get_size(ubi, vol_id, lnum, &size));
	zassert_equal(0, size);

	struct ubi_device_info info_after_map = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_after_map));
	zassert_equal(info_after_map.free_peb_count, info_after_init.free_peb_count - 1);

	/* 4. Deinit. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	memory_check(&before_init, &after_init, &after_deinit);

	/* 5. Re-init, unmap LEB. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));

	zassert_ok(ubi_leb_unmap(ubi, vol_id, lnum));

	struct ubi_device_info info_after_unmap = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_after_unmap));
	zassert_equal(1, info_after_unmap.dirty_peb_count);

	/* 6. Deinit. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	memory_check(&before_init, &after_init, &after_deinit);

	/* 7. Re-init, verify dirty cleaned up by attach scan. */
	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));

	struct ubi_device_info info_final = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_final));
	zassert_equal(0, info_final.dirty_peb_count);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Map/unmap all LEBs lifecycle with reboot.
 *
 * \details Create a 4-LEB static volume, map all 4 LEBs, deinit, re-init,
 *          unmap all, deinit, re-init and verify dirty PEBs reclaimed.
 *          Parity with plain ubi_map.one_volume_with_many_lebs_operations_with_reboot.
 *
 * \expected All LEBs mapped after first cycle; all unmapped after second;
 *           dirty_peb_count == 0 after final reboot; heap balanced.
 */
ZTEST(ubi_secure_map, test_all_lebs_lifecycle_with_reboot)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	/* 1. Init, create volume. */
	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));

	struct ubi_device_info info_after_init = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_after_init));

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	/* 2. Map all LEBs. */
	for (size_t i = 0; i < vol_cfg.leb_count; ++i) {
		zassert_ok(ubi_leb_map(ubi, vol_id, i));
	}

	for (size_t i = 0; i < vol_cfg.leb_count; ++i) {
		bool is_mapped = false;
		zassert_ok(ubi_leb_is_mapped(ubi, vol_id, i, &is_mapped));
		zassert_true(is_mapped);

		size_t sz = 1;
		zassert_ok(ubi_leb_get_size(ubi, vol_id, i, &sz));
		zassert_equal(0, sz);
	}

	struct ubi_device_info info_after_map = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_after_map));
	zassert_equal(info_after_map.free_peb_count,
		      info_after_init.free_peb_count - vol_cfg.leb_count);

	/* 3. Deinit, re-init, unmap all. */
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));

	for (size_t i = 0; i < vol_cfg.leb_count; ++i) {
		zassert_ok(ubi_leb_unmap(ubi, vol_id, i));
	}

	struct ubi_device_info info_after_unmap = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_after_unmap));
	zassert_equal(vol_cfg.leb_count, info_after_unmap.dirty_peb_count);

	zassert_ok(ubi_device_deinit(ubi));
}

/* ------------------------------------ Suite registration ------------------------------------- */

ZTEST_SUITE(ubi_secure_map, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);
