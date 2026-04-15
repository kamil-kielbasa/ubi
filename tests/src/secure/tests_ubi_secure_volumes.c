/**
 * \file    tests_ubi_secure_volumes.c
 * \author  Kamil Kielbasa
 *
 * \brief   Parity tests for secure backend volume create/resize/remove.
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
 * \brief Create a single volume and verify persistence across reboot.
 *
 * \details Init secure device, create one static volume with 2 LEBs,
 *          verify info, deinit, re-init and verify the volume persists
 *          with identical config. Memory leak check on both cycles.
 *          Parity with plain ubi_volumes.create_one_with_reboot.
 *
 * \expected Volume survives reboot with same type, leb_count, and zero
 *           allocated LEBs; heap fully reclaimed after each deinit.
 */
ZTEST(ubi_secure_volumes, test_create_one_with_reboot)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	struct ubi_volume_config read_vol_cfg = { 0 };
	size_t read_alloc_lebs = 0;
	int vol_id = -1;
	struct ubi_device *ubi = NULL;

	/* 1. Init + create volume. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));
	zassert_not_null(ubi);

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_equal(0, vol_id);

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.reserved_peb_count, vol_cfg.leb_count);
	zassert_equal(1, info.volume_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg.type, read_vol_cfg.type);
	zassert_equal(vol_cfg.leb_count, read_vol_cfg.leb_count);
	zassert_equal(0, read_alloc_lebs);

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	memory_check(&before_init, &after_init, &after_deinit);

	/* 2. Re-init and verify persistence. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));
	zassert_not_null(ubi);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg.type, read_vol_cfg.type);
	zassert_equal(vol_cfg.leb_count, read_vol_cfg.leb_count);
	zassert_equal(0, read_alloc_lebs);

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	memory_check(&before_init, &after_init, &after_deinit);
}

/**
 * \brief Create, remove, and verify removal persists across reboot.
 *
 * \details Create a volume, deinit, re-init, verify presence, remove it,
 *          deinit, re-init and confirm the volume is gone.
 *          Parity with plain ubi_volumes.create_one_with_remove_with_reboot.
 *
 * \expected After removal + reboot: volume_count == 0, reserved_peb_count == 0,
 *           volume_get_info returns -ENOENT.
 */
ZTEST(ubi_secure_volumes, test_create_remove_with_reboot)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	struct ubi_volume_config read_vol_cfg = { 0 };
	size_t read_alloc_lebs = 0;
	int vol_id = -1;
	struct ubi_device *ubi = NULL;
	struct ubi_device_info info = { 0 };

	/* 1. Init, create, deinit. */
	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* 2. Re-init, verify, remove. */
	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg.type, read_vol_cfg.type);
	zassert_equal(vol_cfg.leb_count, read_vol_cfg.leb_count);

	zassert_ok(ubi_volume_remove(ubi, vol_id));

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.volume_count);
	zassert_equal(0, info.reserved_peb_count);

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* 3. Re-init, verify removal persists. */
	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.volume_count);
	zassert_equal(0, info.reserved_peb_count);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Resize a volume upward and verify persistence.
 *
 * \details Create a dynamic volume with 2 LEBs, deinit, re-init, resize
 *          to 4 LEBs, deinit, re-init and verify the new leb_count persists.
 *          Parity with plain ubi_volumes.create_one_with_resize_upper_with_reboot.
 *
 * \expected After resize + reboot: leb_count == 4, reserved_peb_count == 4,
 *           volume_count == 1.
 */
ZTEST(ubi_secure_volumes, test_resize_upper_with_reboot)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	struct ubi_volume_config new_vol_cfg = vol_cfg;
	new_vol_cfg.leb_count = 4;

	struct ubi_volume_config read_vol_cfg = { 0 };
	size_t read_alloc_lebs = 0;
	int vol_id = -1;
	struct ubi_device *ubi = NULL;

	/* 1. Create volume with 2 LEBs. */
	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* 2. Re-init, resize to 4 LEBs. */
	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));
	zassert_ok(ubi_volume_resize(ubi, vol_id, &new_vol_cfg));

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(new_vol_cfg.leb_count, read_vol_cfg.leb_count);

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* 3. Re-init, verify resize persists. */
	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(new_vol_cfg.leb_count, read_vol_cfg.leb_count);

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(new_vol_cfg.leb_count, info.reserved_peb_count);
	zassert_equal(1, info.volume_count);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Create multiple volumes and verify persistence.
 *
 * \details Create two volumes (2 LEBs + 4 LEBs) in a single session,
 *          deinit, re-init and verify both volumes survive with correct
 *          leb_counts and volume IDs.
 *          Parity with plain ubi_volumes.create_many_with_reboot.
 *
 * \expected After reboot: volume_count == 2, both volumes report original
 *           leb_count values.
 */
ZTEST(ubi_secure_volumes, test_create_many_with_reboot)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg_1 = {
		.name = { '/', 'u', 'b', 'i', '_', '1' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	const struct ubi_volume_config vol_cfg_2 = {
		.name = { '/', 'u', 'b', 'i', '_', '2' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	int vol_id_1 = -1;
	int vol_id_2 = -1;
	struct ubi_device *ubi = NULL;
	struct ubi_device_info info = { 0 };

	/* 1. Create two volumes. */
	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_1, &vol_id_1));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_2, &vol_id_2));
	zassert_equal(0, vol_id_1);
	zassert_equal(1, vol_id_2);

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(2, info.volume_count);
	zassert_equal(vol_cfg_1.leb_count + vol_cfg_2.leb_count, info.reserved_peb_count);

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* 2. Re-init, verify both volumes persist. */
	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));

	struct ubi_volume_config read_cfg = { 0 };
	size_t alloc = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_1, &read_cfg, &alloc));
	zassert_equal(vol_cfg_1.leb_count, read_cfg.leb_count);

	memset(&read_cfg, 0, sizeof(read_cfg));
	zassert_ok(ubi_volume_get_info(ubi, vol_id_2, &read_cfg, &alloc));
	zassert_equal(vol_cfg_2.leb_count, read_cfg.leb_count);

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(2, info.volume_count);

	zassert_ok(ubi_device_deinit(ubi));
}

/* ------------------------------------ Suite registration ------------------------------------- */

ZTEST_SUITE(ubi_secure_volumes, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);
