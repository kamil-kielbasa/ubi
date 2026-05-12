/**
 * \file    tests_ubi_secure_write_read.c
 * \author  Kamil Kielbasa
 *
 * \brief   Parity tests for secure backend LEB write, read and get_size.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

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

/* Module defines ------------------------------------------------------------------------------- */

/* Module types and type definitiones ----------------------------------------------------------- */

/* Module interface variables and constants ----------------------------------------------------- */
#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* Static variables and constants --------------------------------------------------------------- */

/* Static function declarations ----------------------------------------------------------------- */
static struct ubi_flash_desc flash = { 0 };

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
extern struct sys_heap _system_heap;
#endif

static struct sys_memory_stats before_init = { 0 };
static struct sys_memory_stats after_init = { 0 };
static struct sys_memory_stats after_deinit = { 0 };

/* Static function definitions ------------------------------------------------------------------ */
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

static void *ztest_suite_setup(void)
{
	ubi_test_secure_suite_setup_impl(&flash);
	return NULL;
}

static void ztest_suite_before(void *ctx)
{
	(void)ctx;
	ubi_test_partition_force_release_all();
	zassert_ok(flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE));
}

/* Module interface function definitions -------------------------------------------------------- */
/**
 * \brief Single LEB write persists across reboot.
 *
 * \details Scenario: Create a 4-LEB static volume, write 128-byte array to LEB 2,
 *          read back and verify, deinit, re-init and verify the data
 *          persists with identical size and content.
 *          Parity with plain ubi_write_read.one_volume_one_leb_operation_with_reboot.
 *
 * \expect Data in LEB 2 matches original after reboot; leb_get_size
 *           returns 128; heap fully reclaimed after each deinit.
 */
ZTEST(ubi_secure_write_read, test_one_leb_with_reboot)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;
	const int lnum = 2;

	/* 1. Init, create, write. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_leb_write(ubi, vol_id, lnum, array_128, ARRAY_SIZE(array_128)));

	/* 2. Read back and verify. */
	uint8_t rdata[ARRAY_SIZE(array_128)] = { 0 };
	size_t rdata_size = 0;

	zassert_ok(ubi_leb_get_size(ubi, vol_id, lnum, &rdata_size));
	zassert_equal(ARRAY_SIZE(array_128), rdata_size);

	zassert_ok(ubi_leb_read(ubi, vol_id, lnum, 0, rdata, rdata_size));
	zassert_mem_equal(rdata, array_128, ARRAY_SIZE(array_128));

	/* 3. Deinit. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	memory_check(&before_init, &after_init, &after_deinit);

	/* 4. Re-init and verify persistence. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	rdata_size = 0;
	memset(rdata, 0, sizeof(rdata));

	zassert_ok(ubi_leb_get_size(ubi, vol_id, lnum, &rdata_size));
	zassert_equal(ARRAY_SIZE(array_128), rdata_size);

	zassert_ok(ubi_leb_read(ubi, vol_id, lnum, 0, rdata, rdata_size));
	zassert_mem_equal(rdata, array_128, ARRAY_SIZE(array_128));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	memory_check(&before_init, &after_init, &after_deinit);
}

/**
 * \brief Multiple LEBs with varying data sizes persist across reboot.
 *
 * \details Scenario: Create a 4-LEB static volume, write different-length payloads
 *          to all 4 LEBs, verify each, deinit, re-init and confirm every
 *          LEB retains its original data and size.
 *          Parity with plain ubi_write_read.one_volume_many_leb_operations_with_reboot.
 *
 * \expect Each LEB reports correct size and content after reboot.
 */
ZTEST(ubi_secure_write_read, test_many_lebs_with_reboot)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	const int lebs[] = { 0, 1, 2, 3 };
	const uint8_t wdata[4][16] = {
		{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 },
		{ 100, 101, 102, 103, 104, 105 },
		{ 200, 201, 202, 203 },
		{ 10, 11, 12, 13, 14, 15, 16, 17, 18 },
	};
	const size_t wlens[] = { 10, 6, 4, 9 };

	/* 1. Init, create, write all LEBs. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	for (size_t i = 0; i < ARRAY_SIZE(lebs); ++i) {
		zassert_ok(ubi_leb_write(ubi, vol_id, lebs[i], wdata[i], wlens[i]));
	}

	/* 2. Read and verify. */
	for (size_t i = 0; i < ARRAY_SIZE(lebs); ++i) {
		uint8_t rdata[16] = { 0 };
		size_t rsize = 0;

		zassert_ok(ubi_leb_get_size(ubi, vol_id, lebs[i], &rsize));
		zassert_equal(wlens[i], rsize);

		zassert_ok(ubi_leb_read(ubi, vol_id, lebs[i], 0, rdata, rsize));
		zassert_mem_equal(rdata, wdata[i], wlens[i]);
	}

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* 3. Re-init and verify persistence. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	for (size_t i = 0; i < ARRAY_SIZE(lebs); ++i) {
		uint8_t rdata[16] = { 0 };
		size_t rsize = 0;

		zassert_ok(ubi_leb_get_size(ubi, vol_id, lebs[i], &rsize));
		zassert_equal(wlens[i], rsize);

		zassert_ok(ubi_leb_read(ubi, vol_id, lebs[i], 0, rdata, rsize));
		zassert_mem_equal(rdata, wdata[i], wlens[i]);
	}

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief LEB overwrite: write, overwrite with different data, verify.
 *
 * \details Scenario: Write 4 bytes to LEB 0, read back, then overwrite with 6
 *          different bytes and verify the new content and size replace
 *          the original.
 *
 * \expect After overwrite: leb_get_size returns 6, read returns the
 *           second payload; original data no longer present.
 */
ZTEST(ubi_secure_write_read, test_overwrite)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;
	const int lnum = 0;

	const uint8_t data1[] = { 0xDE, 0xAD, 0xBE, 0xEF };
	const uint8_t data2[] = { 0xCA, 0xFE, 0xBA, 0xBE, 0x42, 0x43 };

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	/* Write initial data. */
	zassert_ok(ubi_leb_write(ubi, vol_id, lnum, data1, sizeof(data1)));

	size_t rsize = 0;
	zassert_ok(ubi_leb_get_size(ubi, vol_id, lnum, &rsize));
	zassert_equal(sizeof(data1), rsize);

	/* Overwrite with different data. */
	zassert_ok(ubi_leb_write(ubi, vol_id, lnum, data2, sizeof(data2)));

	rsize = 0;
	uint8_t rdata[8] = { 0 };
	zassert_ok(ubi_leb_get_size(ubi, vol_id, lnum, &rsize));
	zassert_equal(sizeof(data2), rsize);

	zassert_ok(ubi_leb_read(ubi, vol_id, lnum, 0, rdata, rsize));
	zassert_mem_equal(rdata, data2, sizeof(data2));

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST_SUITE(ubi_secure_write_read, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);
