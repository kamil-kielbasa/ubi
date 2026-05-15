/**
 * \file    tests_ubi_secure_mixed.c
 * \author  Kamil Kielbasa
 *
 * \brief   Parity tests for secure backend: mixed volume/map/write/erase scenarios.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>
#include <ubi_secure.h>
#include <ubi_test.h>
#include "arrays.h"

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

ZTEST_SUITE(ubi_secure_mixed, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);

/**
 * \brief End-to-end scenario: create two volumes, write, remove one, resize
 *        the other, map LEBs, reboot, and verify everything.
 *
 * \details Scenario: Init secure device, create static vol_1 (2 LEBs) and dynamic
 *          vol_2 (2 LEBs), write data to both, remove vol_1, resize vol_2
 *          to 4 LEBs, map LEB 2, deinit, re-init and verify full state.
 *          Parity with plain ubi_mixed.scenario_1 (simplified).
 *
 * \expect After reboot: vol_1 gone (-ENOENT), vol_2 has 4 LEBs,
 *           original data readable in LEB 0, LEB 2 mapped with size 0;
 *           heap fully reclaimed after each deinit.
 */
ZTEST(ubi_secure_mixed, scenario_1)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();

	const struct ubi_volume_config vol_cfg_1 = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	const struct ubi_volume_config vol_cfg_2 = {
		.name = { '/', 'u', 'b', 'i', '_', '1' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};

	struct ubi_device *ubi = NULL;
	int vol_id_1 = -1;
	int vol_id_2 = -1;

	/* 1. Create two volumes and write data. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_1, &vol_id_1));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg_2, &vol_id_2));

	const uint8_t data1[] = { 0xAA, 0xBB, 0xCC, 0xDD };
	const uint8_t data2[] = { 0x11, 0x22, 0x33 };

	zassert_ok(ubi_leb_write(ubi, vol_id_1, 0, data1, sizeof(data1)));
	zassert_ok(ubi_leb_write(ubi, vol_id_2, 0, data2, sizeof(data2)));

	/* 2. Remove vol_1. */
	zassert_ok(ubi_volume_remove(ubi, vol_id_1));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count);

	/* 3. Resize vol_2 to 4 LEBs. */
	struct ubi_volume_config new_vol_cfg_2 = vol_cfg_2;
	new_vol_cfg_2.leb_count = 4;
	zassert_ok(ubi_volume_resize(ubi, vol_id_2, &new_vol_cfg_2));

	/* 4. Map LEB 2 in resized volume. */
	zassert_ok(ubi_leb_map(ubi, vol_id_2, 2));

	bool is_mapped = false;
	zassert_ok(ubi_leb_is_mapped(ubi, vol_id_2, 2, &is_mapped));
	zassert_true(is_mapped);

	/* 5. Deinit. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);

	/* 6. Re-init, verify state. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	ubi = NULL;
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count);

	/* Verify vol_2 has 4 LEBs. */
	struct ubi_volume_config read_cfg = { 0 };
	size_t alloc = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id_2, &read_cfg, &alloc));
	zassert_equal(new_vol_cfg_2.leb_count, read_cfg.leb_count);

	/* Verify data2 in LEB 0 still readable. */
	uint8_t rdata[8] = { 0 };
	size_t rsize = 0;
	zassert_ok(ubi_leb_get_size(ubi, vol_id_2, 0, &rsize));
	zassert_equal(sizeof(data2), rsize);
	zassert_ok(ubi_leb_read(ubi, vol_id_2, 0, 0, rdata, rsize));
	zassert_mem_equal(rdata, data2, sizeof(data2));

	/* Verify LEB 2 is still mapped (empty). */
	is_mapped = false;
	zassert_ok(ubi_leb_is_mapped(ubi, vol_id_2, 2, &is_mapped));
	zassert_true(is_mapped);

	size_t sz = 1;
	zassert_ok(ubi_leb_get_size(ubi, vol_id_2, 2, &sz));
	zassert_equal(0, sz);

	/* Verify vol_1 is gone. */
	struct ubi_volume_config tmp_cfg = { 0 };
	size_t tmp_alloc = 0;
	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, vol_id_1, &tmp_cfg, &tmp_alloc));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);
}
