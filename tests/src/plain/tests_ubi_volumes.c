/**
 * \file    tests_ubi_volumes.c
 *
 * \author  Kamil Kielbasa
 *
 * \brief   Hardware tests for Unsorted Block Images (UBI) volumes operations.
 *
 * \version 0.5
 * \date    2026-03-26
 *
 * \copyright Copyright (c) 2025
 *
 */

/* --------------------------------------- Include files --------------------------------------- */

/* UBI header: */
#include <ubi.h>
#include <ubi_test.h>

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

/* -------------------------------------- Module defines --------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* ---------------------------- Module types and type definitiones ----------------------------- */
/* ------------------------- Module interface variables and constants -------------------------- */
/* ------------------------------ Static variables and constants ------------------------------- */

static struct ubi_mtd mtd = { 0 };

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
extern struct sys_heap _system_heap;
#endif

static struct sys_memory_stats before_init = { 0 };
static struct sys_memory_stats after_init = { 0 };
static struct sys_memory_stats after_deinit = { 0 };

/* ------------------------------- Static function declarations -------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_after(void *ctx);

static void ztest_testcase_before(void *ctx);
static void ztest_testcase_teardown(void *ctx);

static void memory_check(struct sys_memory_stats *bi, struct sys_memory_stats *ai,
			 struct sys_memory_stats *ad);

static void erase_counters_check(struct ubi_device *ubi, size_t exp_ec);

/* -------------------------------- Static function definitions -------------------------------- */

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

	ubi_test_partition_force_release_all();
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

#if defined(CONFIG_UBI_MEM_BACKEND_HEAP)
	/* Under the heap backend, init must consume heap memory. */
	zassert_not_equal(ai->free_bytes, ad->free_bytes);
	zassert_not_equal(ai->allocated_bytes, ad->allocated_bytes);
#endif

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

/* --------------------------- Module interface function definitions --------------------------- */

ZTEST_SUITE(ubi_volumes, NULL, ztest_suite_setup, ztest_testcase_before, ztest_testcase_teardown,
	    ztest_suite_after);

/**
 * \brief Verify that a single volume persists across a simulated reboot.
 *
 * \details Scenario: Create a static volume with 2 LEBs. Verify the device
 *          info reflects the allocated LEBs and volume count. Deinitialize,
 *          re-initialize (simulated reboot), and verify the volume persists
 *          with the same name, type, LEB count, and allocation.
 *
 * \expect Volume is recoverable after reboot. Device info matches across
 *         both init cycles. Heap memory is fully reclaimed after deinit.
 */
ZTEST(ubi_volumes, create_one_with_reboot)
{
	const size_t exp_ec_avr = 0;

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

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_equal(0, vol_id);

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(info.reserved_peb_count, vol_cfg.leb_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg.type, read_vol_cfg.type);
	zassert_equal(vol_cfg.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg.type, read_vol_cfg.type);
	zassert_equal(vol_cfg.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);
}

/**
 * \brief Verify that creating and removing a volume persists correctly across
 *        reboots.
 *
 * \details Scenario: Create a static volume. Deinitialize and re-initialize to
 *          confirm persistence. Remove the volume, deinitialize, re-initialize,
 *          and verify the volume no longer exists.
 *
 * \expect After removal and reboot, volume_count=0 and reserved_peb_count=0.
 *         Heap memory is fully reclaimed after each deinit.
 */
ZTEST(ubi_volumes, create_one_with_remove_with_reboot)
{
	const size_t exp_ec_avr = 0;

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

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	/* 1. Initialize device */
	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	/* 2. Create volume */
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_equal(0, vol_id);

	/* 3. Verify created volume */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(info.reserved_peb_count, vol_cfg.leb_count);
	zassert_equal(1, info.volume_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg.type, read_vol_cfg.type);
	zassert_equal(vol_cfg.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	/* 4. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	/* 5. Initialize device */
	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	/* 6. Verify created volume */
	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg.type, read_vol_cfg.type);
	zassert_equal(vol_cfg.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	/* 7. Remove volume */
	zassert_ok(ubi_volume_remove(ubi, vol_id));

	/* 8. Verify after removed volume */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(0, info.reserved_peb_count);
	zassert_equal(0, info.volume_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, vol_id, &read_vol_cfg, &read_alloc_lebs));

	/* 9. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	/* 10. Initialize device */
	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	/* 11. Verify removed volume */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(0, info.reserved_peb_count);
	zassert_equal(0, info.volume_count);

	/* 12. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);
}

/**
 * \brief Verify that resizing a dynamic volume upward persists across a reboot.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs. Resize it upward
 *          to 4 LEBs. Verify the new allocation. Deinitialize, re-initialize,
 *          and confirm the resized configuration persists.
 *
 * \expect After reboot, the volume reports leb_count=4 with the correct
 *         reserved_peb_count. Heap memory is fully reclaimed after deinit.
 */
ZTEST(ubi_volumes, create_one_with_resize_upper_with_reboot)
{
	const size_t exp_ec_avr = 0;

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
	struct ubi_device_info info = { 0 };

	/* 1. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	/* 2. Create volume */
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_equal(0, vol_id);

	/* 3. Verify created volume */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(info.reserved_peb_count, vol_cfg.leb_count);
	zassert_equal(1, info.volume_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg.type, read_vol_cfg.type);
	zassert_equal(vol_cfg.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	/* 4. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 5. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	/* 6. Verify created volume */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(info.reserved_peb_count, vol_cfg.leb_count);
	zassert_equal(1, info.volume_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg.type, read_vol_cfg.type);
	zassert_equal(vol_cfg.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	/* 7. Resize volume */
	zassert_ok(ubi_volume_resize(ubi, vol_id, &new_vol_cfg));

	/* 8. Verify resized volume */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(new_vol_cfg.leb_count, info.reserved_peb_count);
	zassert_equal(1, info.volume_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(new_vol_cfg.type, read_vol_cfg.type);
	zassert_equal(new_vol_cfg.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(new_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(new_vol_cfg.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	/* 9. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 10. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	/* 11. Verify resized volume */
	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(new_vol_cfg.type, read_vol_cfg.type);
	zassert_equal(new_vol_cfg.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(new_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(new_vol_cfg.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	/* 9. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);
}

/**
 * \brief Verify that multiple volumes persist across a reboot.
 *
 * \details Scenario: Create three volumes with varying LEB counts (2, 4, 8).
 *          Verify all device info counters and per-volume configurations.
 *          Deinitialize, re-initialize, and confirm all three volumes persist
 *          with correct properties.
 *
 * \expect All three volumes are recoverable after reboot with matching names,
 *         types, LEB counts, and allocations. Heap memory is fully reclaimed.
 */
ZTEST(ubi_volumes, create_many_with_reboot)
{
	const size_t exp_ec_avr = 0;

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

	const struct ubi_volume_config vol_cfg_3 = {
		.name = { '/', 'u', 'b', 'i', '_', '3' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 8,
	};

	struct ubi_volume_config read_vol_cfg = { 0 };
	size_t read_alloc_lebs = 0;

	int vol_id_1 = -1;
	int vol_id_2 = -1;
	int vol_id_3 = -1;

	struct ubi_device *ubi = NULL;
	struct ubi_device_info info = { 0 };

	/* 1. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	/* 2. Create three volumes */
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_1, &vol_id_1));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_2, &vol_id_2));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_3, &vol_id_3));

	/* 3. Verify created volumes */
	zassert_equal(0, vol_id_1);
	zassert_equal(1, vol_id_2);
	zassert_equal(2, vol_id_3);

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(3, info.volume_count);
	zassert_equal(info.reserved_peb_count,
		      vol_cfg_1.leb_count + vol_cfg_2.leb_count + vol_cfg_3.leb_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_1, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_1.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_1.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_1.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_1.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_2, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_2.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_2.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_2.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_2.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_3, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_3.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_3.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_3.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_3.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	/* 4. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 5. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	/* 6. Verify created volumes */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(3, info.volume_count);
	zassert_equal(info.reserved_peb_count,
		      vol_cfg_1.leb_count + vol_cfg_2.leb_count + vol_cfg_3.leb_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_1, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_1.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_1.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_1.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_1.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_2, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_2.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_2.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_2.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_2.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_3, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_3.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_3.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_3.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_3.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	/* 7. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);
}

/**
 * \brief Verify selective volume removal with multiple volumes across reboots.
 *
 * \details Scenario: Create three volumes. Remove the middle volume and verify
 *          the remaining two persist after reboot. Remove the remaining volumes
 *          and verify the device is empty after another reboot.
 *
 * \expect After selective removal, only the expected volumes remain.
 *         After full removal, volume_count=0. Heap memory is reclaimed.
 */
ZTEST(ubi_volumes, create_many_with_remove_with_reboot)
{
	const size_t exp_ec_avr = 0;

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

	const struct ubi_volume_config vol_cfg_3 = {
		.name = { '/', 'u', 'b', 'i', '_', '3' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 8,
	};

	struct ubi_volume_config read_vol_cfg = { 0 };
	size_t read_alloc_lebs = 0;

	int vol_id_1 = -1;
	int vol_id_2 = -1;
	int vol_id_3 = -1;

	struct ubi_device *ubi = NULL;
	struct ubi_device_info info = { 0 };

	/* 1. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	/* 2. Create three volumes */
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_1, &vol_id_1));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_2, &vol_id_2));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_3, &vol_id_3));

	/* 3. Verify created volumes */
	zassert_equal(0, vol_id_1);
	zassert_equal(1, vol_id_2);
	zassert_equal(2, vol_id_3);

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.reserved_peb_count,
		      vol_cfg_1.leb_count + vol_cfg_2.leb_count + vol_cfg_3.leb_count);
	zassert_equal(3, info.volume_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_1, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_1.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_1.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_1.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_1.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_2, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_2.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_2.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_2.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_2.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_3, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_3.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_3.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_3.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_3.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	/* 4. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 5. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	/* 6. Verify created volumes */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(3, info.volume_count);
	zassert_equal(info.reserved_peb_count,
		      vol_cfg_1.leb_count + vol_cfg_2.leb_count + vol_cfg_3.leb_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_1, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_1.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_1.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_1.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_1.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_2, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_2.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_2.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_2.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_2.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_3, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_3.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_3.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_3.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_3.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	/* 7. Remove volume */
	zassert_ok(ubi_volume_remove(ubi, vol_id_2));

	/* 8. Verify after removed volume */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(info.reserved_peb_count, vol_cfg_1.leb_count + vol_cfg_3.leb_count);
	zassert_equal(2, info.volume_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, vol_id_2, &read_vol_cfg, &read_alloc_lebs));

	/* 9. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 10. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	/* 11. Verify existing volumes */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.reserved_peb_count, vol_cfg_1.leb_count + vol_cfg_3.leb_count);
	zassert_equal(2, info.volume_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_1, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_1.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_1.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_1.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_1.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, vol_id_2, &read_vol_cfg, &read_alloc_lebs));

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_3, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_3.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_3.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_3.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_3.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	/* 12. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 13. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	/* 14. Verify created volumes */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(2, info.volume_count);
	zassert_equal(info.reserved_peb_count, vol_cfg_1.leb_count + vol_cfg_3.leb_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_1, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_1.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_1.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_1.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_1.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, vol_id_2, &read_vol_cfg, &read_alloc_lebs));

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_3, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_3.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_3.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_3.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_3.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	/* 15. Remove left volumes */
	zassert_ok(ubi_volume_remove(ubi, vol_id_1));
	zassert_ok(ubi_volume_remove(ubi, vol_id_3));

	/* 16. Verify after removed volumes */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(0, info.reserved_peb_count);
	zassert_equal(0, info.volume_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, vol_id_1, &read_vol_cfg, &read_alloc_lebs));

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, vol_id_3, &read_vol_cfg, &read_alloc_lebs));

	/* 17. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 18. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	/* 19. Verify after removed all volumes */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(0, info.reserved_peb_count);
	zassert_equal(0, info.volume_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, vol_id_1, &read_vol_cfg, &read_alloc_lebs));

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, vol_id_2, &read_vol_cfg, &read_alloc_lebs));

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, vol_id_3, &read_vol_cfg, &read_alloc_lebs));

	/* 20. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);
}

/**
 * \brief Verify complex multi-volume resize scenarios (both grow and shrink)
 *        across reboots.
 *
 * \details Scenario: Create three dynamic volumes. Resize one downward
 *          (8\u21924 LEBs) and two upward (2\u21923, 4\u21926 LEBs). Verify all
 *          allocation counts. Deinitialize, re-initialize, and confirm the
 *          resized configurations persist.
 *
 * \expect All resize operations succeed. After reboot, each volume reports
 *         its updated leb_count. Total allocated LEBs match the sum of
 *         resized counts.
 */
ZTEST(ubi_volumes, create_many_with_resizes_lower_and_upper_with_reboot)
{
	const size_t exp_ec_avr = 0;

	const struct ubi_volume_config vol_cfg_1 = {
		.name = { '/', 'u', 'b', 'i', '_', '1' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};

	struct ubi_volume_config res_vol_cfg_1 = vol_cfg_1;
	res_vol_cfg_1.leb_count = 3;

	const struct ubi_volume_config vol_cfg_2 = {
		.name = { '/', 'u', 'b', 'i', '_', '2' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 4,
	};

	struct ubi_volume_config res_vol_cfg_2 = vol_cfg_2;
	res_vol_cfg_2.leb_count = 6;

	const struct ubi_volume_config vol_cfg_3 = {
		.name = { '/', 'u', 'b', 'i', '_', '3' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 8,
	};

	struct ubi_volume_config res_vol_cfg_3 = vol_cfg_3;
	res_vol_cfg_3.leb_count = 4;

	struct ubi_volume_config read_vol_cfg = { 0 };
	size_t read_alloc_lebs = 0;

	int vol_id_1 = -1;
	int vol_id_2 = -1;
	int vol_id_3 = -1;

	struct ubi_device *ubi = NULL;
	struct ubi_device_info info = { 0 };

	/* 1. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	/* 2. Create three volumes */
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_1, &vol_id_1));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_2, &vol_id_2));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_3, &vol_id_3));

	/* 3. Verify created volumes */
	zassert_equal(0, vol_id_1);
	zassert_equal(1, vol_id_2);
	zassert_equal(2, vol_id_3);

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.reserved_peb_count,
		      vol_cfg_1.leb_count + vol_cfg_2.leb_count + vol_cfg_3.leb_count);
	zassert_equal(3, info.volume_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_1, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_1.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_1.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_1.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_1.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_2, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_2.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_2.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_2.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_2.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_3, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_3.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_3.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_3.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_3.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	/* 4. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 5. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	/* 6. Verify created volumes */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(3, info.volume_count);
	zassert_equal(info.reserved_peb_count,
		      vol_cfg_1.leb_count + vol_cfg_2.leb_count + vol_cfg_3.leb_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_1, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_1.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_1.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_1.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_1.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_2, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_2.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_2.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_2.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_2.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_3, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_3.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_3.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_3.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_3.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	/* 7. Resize volume */
	zassert_ok(ubi_volume_resize(ubi, vol_id_3, &res_vol_cfg_3));

	/* 8. Verify after resized volume */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(info.reserved_peb_count,
		      vol_cfg_1.leb_count + vol_cfg_2.leb_count + res_vol_cfg_3.leb_count);
	zassert_equal(3, info.volume_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_3, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(res_vol_cfg_3.type, read_vol_cfg.type);
	zassert_equal(res_vol_cfg_3.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(res_vol_cfg_3.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(res_vol_cfg_3.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	/* 9. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 10. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	/* 11. Verify existing volumes */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.reserved_peb_count, vol_cfg_1.leb_count + vol_cfg_3.leb_count);
	zassert_equal(3, info.volume_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_1, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_1.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_1.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_1.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_1.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_2, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_2.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_2.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_2.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_2.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_3, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(res_vol_cfg_3.type, read_vol_cfg.type);
	zassert_equal(res_vol_cfg_3.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(res_vol_cfg_3.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(res_vol_cfg_3.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	/* 12. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 13. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	/* 14. Verify created volumes */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.reserved_peb_count,
		      vol_cfg_1.leb_count + vol_cfg_2.leb_count + res_vol_cfg_3.leb_count);
	zassert_equal(3, info.volume_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_1, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_1.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_1.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_1.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_1.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_2, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(vol_cfg_2.type, read_vol_cfg.type);
	zassert_equal(vol_cfg_2.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(vol_cfg_2.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(vol_cfg_2.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_3, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(res_vol_cfg_3.type, read_vol_cfg.type);
	zassert_equal(res_vol_cfg_3.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(res_vol_cfg_3.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(res_vol_cfg_3.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	/* 15. Remove left volumes */
	zassert_ok(ubi_volume_resize(ubi, vol_id_1, &res_vol_cfg_1));
	zassert_ok(ubi_volume_resize(ubi, vol_id_2, &res_vol_cfg_2));

	/* 16. Verify after resizes of volumes */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(info.reserved_peb_count,
		      res_vol_cfg_1.leb_count + res_vol_cfg_2.leb_count + res_vol_cfg_3.leb_count);
	zassert_equal(3, info.volume_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_1, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(res_vol_cfg_1.type, read_vol_cfg.type);
	zassert_equal(res_vol_cfg_1.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(res_vol_cfg_1.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(res_vol_cfg_1.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_2, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(res_vol_cfg_2.type, read_vol_cfg.type);
	zassert_equal(res_vol_cfg_2.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(res_vol_cfg_2.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(res_vol_cfg_2.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	/* 17. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);

	/* 18. Initialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	/* 19. Verify after resizes of all volumes */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(info.reserved_peb_count,
		      res_vol_cfg_1.leb_count + res_vol_cfg_2.leb_count + res_vol_cfg_3.leb_count);
	zassert_equal(3, info.volume_count);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_1, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(res_vol_cfg_1.type, read_vol_cfg.type);
	zassert_equal(res_vol_cfg_1.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(res_vol_cfg_1.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(res_vol_cfg_1.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_2, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(res_vol_cfg_2.type, read_vol_cfg.type);
	zassert_equal(res_vol_cfg_2.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(res_vol_cfg_2.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(res_vol_cfg_2.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	memset(&read_vol_cfg, 0, sizeof(read_vol_cfg));
	read_alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_3, &read_vol_cfg, &read_alloc_lebs));
	zassert_equal(res_vol_cfg_3.type, read_vol_cfg.type);
	zassert_equal(res_vol_cfg_3.leb_count, read_vol_cfg.leb_count);
	zassert_equal(strnlen(res_vol_cfg_3.name, UBI_VOLUME_NAME_MAX_LEN),
		      strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN));
	zassert_equal(0, strncmp(res_vol_cfg_3.name, read_vol_cfg.name,
				 strnlen(read_vol_cfg.name, UBI_VOLUME_NAME_MAX_LEN)));
	zassert_equal(0, read_alloc_lebs);

	/* 20. Deinitialize device */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	erase_counters_check(ubi, exp_ec_avr);

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);
}
