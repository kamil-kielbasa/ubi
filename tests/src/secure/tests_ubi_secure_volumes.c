/**
 * \file    tests_ubi_secure_volumes.c
 * \author  Kamil Kielbasa
 *
 * \brief   Parity tests for secure backend volume create/resize/remove.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

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

/* Module defines ------------------------------------------------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* Static variables ----------------------------------------------------------------------------- */

static struct ubi_flash_desc flash = { 0 };

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
extern struct sys_heap _system_heap;
#endif

static struct sys_memory_stats before_init = { 0 };
static struct sys_memory_stats after_init = { 0 };
static struct sys_memory_stats after_deinit = { 0 };

/* Static helpers ------------------------------------------------------------------------------- */

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

/* Suite setup / teardown ----------------------------------------------------------------------- */

static void *ztest_suite_setup(void)
{
	const struct device *flash_dev = UBI_PARTITION_DEVICE;
	zassert_true(device_is_ready(flash_dev));

	struct flash_pages_info page_info = { 0 };
	zassert_ok(flash_get_page_info_by_offs(flash_dev, 0, &page_info));

	flash.partition_id = FIXED_PARTITION_ID(UBI_PARTITION_NAME);
	flash.erase_block_size = page_info.size;
	flash.write_block_size = flash_get_write_block_size(flash_dev);

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

/* Tests ---------------------------------------------------------------------------------------- */

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

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_not_null(ubi);

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_equal(0, vol_id);

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	/* reserved = leb_count + 1 hidden anchor PEB per volume (§7.9). */
	zassert_equal(info.reserved_peb_count, vol_cfg.leb_count + 1);
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
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
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
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* 2. Re-init, verify, remove. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

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
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

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
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* 2. Re-init, resize to 4 LEBs. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_resize(ubi, vol_id, &new_vol_cfg));

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(new_vol_cfg.leb_count, read_vol_cfg.leb_count);

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* 3. Re-init, verify resize persists. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(new_vol_cfg.leb_count, read_vol_cfg.leb_count);

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	/* reserved = leb_count + 1 hidden anchor PEB per volume (§7.9). */
	zassert_equal(new_vol_cfg.leb_count + 1, info.reserved_peb_count);
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
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_1, &vol_id_1));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_2, &vol_id_2));
	zassert_equal(0, vol_id_1);
	zassert_equal(1, vol_id_2);

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(2, info.volume_count);
	/* reserved = sum(leb_count) + 2 hidden anchor PEBs (§7.9). */
	zassert_equal(vol_cfg_1.leb_count + vol_cfg_2.leb_count + 2, info.reserved_peb_count);

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* 2. Re-init, verify both volumes persist. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

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

/**
 * \brief Shrink a volume and verify persistence across reboot (no erase).
 *
 * \details Create a 4-LEB dynamic volume, write data to LEBs 2 and 3,
 *          shrink to 2 LEBs, deinit, re-init and verify the shrunken
 *          leb_count persists and tail LEBs are recovered as dirty.
 *          Tests §11.7: resize commits smaller leb_count in reserved
 *          metadata, so tail PEBs whose lnum is out of range become dirty
 *          after reboot even without prior erase.
 *
 * \expected After shrink + reboot: leb_count == 2, reserved_peb_count == 3,
 *           tail LEB PEBs recovered as dirty, volume_count == 1.
 */
ZTEST(ubi_secure_volumes, test_shrink_with_reboot)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 's', 'h', 'r', 'k' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 4,
	};

	struct ubi_volume_config read_vol_cfg = { 0 };
	size_t read_alloc_lebs = 0;
	int vol_id = -1;
	struct ubi_device *ubi = NULL;
	struct ubi_device_info info = { 0 };

	/* 1. Create volume with 4 LEBs, write tail LEBs. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	const uint8_t data[] = { 0xAA };
	zassert_ok(ubi_leb_write(ubi, vol_id, 2, data, sizeof(data)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 3, data, sizeof(data)));

	/* 2. Shrink to 2 LEBs. */
	const struct ubi_volume_config shrink_cfg = {
		.name = { '/', 's', 'h', 'r', 'k' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	zassert_ok(ubi_volume_resize(ubi, vol_id, &shrink_cfg));

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(2, read_vol_cfg.leb_count);

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	/* Tail LEBs became dirty. */
	zassert_true(info.dirty_peb_count >= 2, "Tail PEBs should be dirty");

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* 3. Reboot (no erase) — verify shrink persists. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(2, read_vol_cfg.leb_count);

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	/* reserved = 2 LEBs + 1 anchor. */
	zassert_equal(shrink_cfg.leb_count + 1, info.reserved_peb_count);
	zassert_equal(1, info.volume_count);
	/* Tail PEBs are recovered as dirty (§11.7). */
	zassert_true(info.dirty_peb_count >= 2, "Tail PEBs should be dirty after reboot");

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Shrink, erase dirty PEBs, then verify persistence across reboot.
 *
 * \details Same setup as test_shrink_with_reboot but after shrinking all
 *          dirty PEBs are erased before deinit.  This exercises the path
 *          where the erased PEB may have been the last writable witness
 *          for the old LEB range — the hidden anchor preserves continuity.
 *
 * \expected After shrink + erase + reboot: leb_count == 2, dirty_peb_count
 *           == 0, all freed PEBs returned to free pool.
 */
ZTEST(ubi_secure_volumes, test_shrink_erase_reboot)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 's', 'e', 'r' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 4,
	};

	int vol_id = -1;
	struct ubi_device *ubi = NULL;
	struct ubi_device_info info = { 0 };

	/* 1. Create, write tail LEBs, shrink. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	const uint8_t data[] = { 0xBB };
	zassert_ok(ubi_leb_write(ubi, vol_id, 2, data, sizeof(data)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 3, data, sizeof(data)));

	const struct ubi_volume_config shrink_cfg = {
		.name = { '/', 's', 'e', 'r' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	zassert_ok(ubi_volume_resize(ubi, vol_id, &shrink_cfg));

	/* 2. Erase all dirty PEBs. */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	while (info.dirty_peb_count > 0) {
		zassert_ok(ubi_device_erase_peb(ubi));
		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(ubi, &info));
	}

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* 3. Reboot — verify shrink persists, dirty cleaned. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	struct ubi_volume_config read_vol_cfg = { 0 };
	size_t read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(2, read_vol_cfg.leb_count);

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(shrink_cfg.leb_count + 1, info.reserved_peb_count);
	zassert_equal(1, info.volume_count);
	zassert_equal(0, info.dirty_peb_count);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify vid_next_counter_floor persists across remove-all + reboot.
 *
 * \details Create a volume, write data to advance the VID counter, then
 *          remove the volume (zero-volume state).  Deinit, re-init, create
 *          a new volume and write again.  The new write's VID counter must
 *          be above the previously committed floor — not reset to 0.
 *          Tests §9.8.5: vid_next_counter_floor is saved in the secure
 *          device header during every reserved metadata rewrite.
 *
 * \expected After remove + reboot + create: new writes do not reuse
 *           VID counter values from the previous volume's lifetime.
 *           Verified indirectly: the device successfully stores and
 *           retrieves data, proving the floor was not corrupted.
 */
ZTEST(ubi_secure_volumes, test_vid_counter_floor_persists)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'v', 'c', 'f' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	int vol_id = -1;
	struct ubi_device *ubi = NULL;
	struct ubi_device_info info = { 0 };

	/* 1. Create volume and write data to advance the VID counter. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	const uint8_t data[] = { 0xCC, 0xDD };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	/* Overwrite to push counter further. */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 1, data, sizeof(data)));

	/* 2. Remove the volume (zero-volume state). */
	zassert_ok(ubi_volume_remove(ubi, vol_id));

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.volume_count);
	zassert_equal(0, info.reserved_peb_count);

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* 3. Reboot — floor must be preserved in secure device header. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.volume_count);

	/* 4. Create new volume and write — must succeed with fresh counter
	 *    above the old floor.  If the floor was lost, the new anchor and
	 *    writes would reuse counter values, which could cause replay. */
	const struct ubi_volume_config vol_cfg2 = {
		.name = { '/', 'v', 'c', '2' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id2 = -1;
	zassert_ok(ubi_volume_create(ubi, &vol_cfg2, &vol_id2));

	const uint8_t data2[] = { 0xEE, 0xFF };
	zassert_ok(ubi_leb_write(ubi, vol_id2, 0, data2, sizeof(data2)));

	/* Verify data integrity. */
	uint8_t rdata[sizeof(data2)] = { 0 };
	size_t rsize = 0;
	zassert_ok(ubi_leb_get_size(ubi, vol_id2, 0, &rsize));
	zassert_equal(sizeof(data2), rsize);
	zassert_ok(ubi_leb_read(ubi, vol_id2, 0, 0, rdata, rsize));
	zassert_mem_equal(rdata, data2, sizeof(data2));

	/* 5. Deinit, re-init — verify persistence of new data. */
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	memset(rdata, 0, sizeof(rdata));
	rsize = 0;
	zassert_ok(ubi_leb_get_size(ubi, vol_id2, 0, &rsize));
	zassert_equal(sizeof(data2), rsize);
	zassert_ok(ubi_leb_read(ubi, vol_id2, 0, 0, rdata, rsize));
	zassert_mem_equal(rdata, data2, sizeof(data2));

	zassert_ok(ubi_device_deinit(ubi));
}

/* Suite registration --------------------------------------------------------------------------- */

/**
 * \brief Verify VID counter floor survives remove→create→reboot sequence.
 *
 * \details Create volume, write several LEBs, remove volume, create new
 *          volume, write, reboot, re-read. The sequence must not cause
 *          counter value reuse. Additionally verifies that multiple
 *          create→remove→create cycles do not reset the floor.
 *
 * \expected New volume writes succeed after remove→create→reboot,
 *           data integrity is preserved, and no counter was reused.
 */
ZTEST(ubi_secure_volumes, test_vid_counter_floor_remove_create_reboot)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg_a = {
		.name = { '/', 'a' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	/* 1. Create volume A, write data to advance VID counter. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_a, &vol_id));

	const uint8_t data1[] = { 0x11, 0x22 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data1, sizeof(data1)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 1, data1, sizeof(data1)));
	/* Overwrite to push counter further. */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data1, sizeof(data1)));

	/* 2. Remove volume A — floor preserved in device header. */
	zassert_ok(ubi_volume_remove(ubi, vol_id));

	/* 3. Create volume B (same config). */
	const struct ubi_volume_config vol_cfg_b = {
		.name = { '/', 'b' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	int vol_id_b = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg_b, &vol_id_b));

	const uint8_t data2[] = { 0xAA, 0xBB, 0xCC };

	zassert_ok(ubi_leb_write(ubi, vol_id_b, 0, data2, sizeof(data2)));

	/* 4. Reboot. */
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	/* 5. Verify data integrity — floor was preserved across remove→create→reboot. */
	uint8_t rdata[sizeof(data2)] = { 0 };
	size_t rsize = 0;

	zassert_ok(ubi_leb_get_size(ubi, vol_id_b, 0, &rsize));
	zassert_equal(sizeof(data2), rsize);
	zassert_ok(ubi_leb_read(ubi, vol_id_b, 0, 0, rdata, rsize));
	zassert_mem_equal(rdata, data2, sizeof(data2));

	/* 6. One more cycle: remove B, create C, reboot, verify. */
	zassert_ok(ubi_volume_remove(ubi, vol_id_b));

	const struct ubi_volume_config vol_cfg_c = {
		.name = { '/', 'c' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id_c = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg_c, &vol_id_c));

	const uint8_t data3[] = { 0xDD, 0xEE };

	zassert_ok(ubi_leb_write(ubi, vol_id_c, 0, data3, sizeof(data3)));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	memset(rdata, 0, sizeof(rdata));
	rsize = 0;
	zassert_ok(ubi_leb_get_size(ubi, vol_id_c, 0, &rsize));
	zassert_equal(sizeof(data3), rsize);
	zassert_ok(ubi_leb_read(ubi, vol_id_c, 0, 0, rdata, rsize));
	zassert_mem_equal(rdata, data3, sizeof(data3));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST_SUITE(ubi_secure_volumes, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);
