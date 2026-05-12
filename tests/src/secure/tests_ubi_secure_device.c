/**
 * \file    tests_ubi_secure_device.c
 * \author  Kamil Kielbasa
 *
 * \brief   Parity tests for secure backend device init/deinit and info.
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
	ubi_test_secure_before_impl();
}

/* Module interface function definitions -------------------------------------------------------- */
/**
 * \brief Secure init/deinit on blank flash: device info is valid.
 *
 * \details Scenario: Format a blank partition in secure mode, query device info,
 *          verify all pool counters and geometry, then deinit and check
 *          for memory leaks. Parity with plain ubi_device.init_deinit.
 *
 * \expect All PEBs free, zero dirty/bad/volume counts, leb_size within
 *           (0, erase_block_size), heap fully reclaimed after deinit.
 */
ZTEST(ubi_secure_device, test_init_deinit)
{
	/* Secure reserves extra PEBs for the reserved-PEB bank. */
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;
	struct ubi_device_info info = { 0 };

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_not_null(ubi);

	zassert_ok(ubi_device_get_info(ubi, &info));

	zassert_equal(0, info.reserved_peb_count);
	zassert_true(info.total_peb_count > 0);
	zassert_equal(info.free_peb_count, info.total_peb_count);
	zassert_equal(0, info.dirty_peb_count);
	zassert_equal(0, info.bad_peb_count);
	zassert_between_inclusive(info.leb_size, 1, flash.erase_block_size - 1);
	zassert_equal(0, info.volume_count);

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));

	memory_check(&before_init, &after_init, &after_deinit);
}

/**
 * \brief Secure init/deinit/init cycle (reboot persistence).
 *
 * \details Scenario: Format on blank, deinit, re-init. The second init must attach
 *          to the existing secure metadata and report identical geometry.
 *          Parity with plain ubi_device.init_deinit_init.
 *
 * \expect Both cycles return 0; total_peb_count, leb_size, and
 *           volume_count are identical across the reboot boundary.
 */
ZTEST(ubi_secure_device, test_init_deinit_init)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	/* First cycle: format on blank. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_not_null(ubi);

	struct ubi_device_info info1 = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info1));

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Second cycle: attach to existing. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_not_null(ubi);

	struct ubi_device_info info2 = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info2));

	zassert_equal(info1.total_peb_count, info2.total_peb_count);
	zassert_equal(info1.leb_size, info2.leb_size);
	zassert_equal(info1.volume_count, info2.volume_count);

	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST_SUITE(ubi_secure_device, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);
