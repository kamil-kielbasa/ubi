/**
 * \file    tests_ubi_secure_device.c
 * \author  Kamil Kielbasa
 *
 * \brief   Parity tests for secure backend device init/deinit and info.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>
#include <ubi_secure.h>
#include <ubi_test.h>

/* Test fixtures: */
#include "ubi_test_fixture.h"
#include "ubi_test_memory.h"
#include "ubi_test_secure_fixture.h"

/* Zephyr headers: */
#include <psa/crypto.h>
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/sys_heap.h>

/* Standard library headers: */
#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

/* Module types and type definitiones ----------------------------------------------------------- */

/* Module interface variables and constants ----------------------------------------------------- */

/* Static variables and constants --------------------------------------------------------------- */

static struct ubi_flash_desc flash = { 0 };

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
extern struct sys_heap _system_heap;
#endif

static struct sys_memory_stats before_init = { 0 };
static struct sys_memory_stats after_init = { 0 };
static struct sys_memory_stats after_deinit = { 0 };

/* Static function declarations ----------------------------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_before(void *ctx);

/* Static function definitions ------------------------------------------------------------------ */

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

ZTEST_SUITE(ubi_secure_device, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);

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
ZTEST(ubi_secure_device, init_deinit)
{
	/* Secure reserves extra PEBs for the reserved-PEB bank. */
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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

	ubi_test_memory_check(&before_init, &after_init, &after_deinit);
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
ZTEST(ubi_secure_device, init_deinit_init)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
