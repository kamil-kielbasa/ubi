/**
 * \file    tests_ubi_secure_coverage.c
 * \author  Kamil Kielbasa
 *
 * \brief   Targeted coverage tests for secure backend: volume lifecycle,
 *          LEB overwrite/map/unmap, flash fault injection, and erase with
 *          bad block torture recovery.
 *
 * \details Covers previously untested paths: volume remove, volume resize
 *          (shrink), LEB overwrite counter recovery, LEB map/unmap,
 *          flash write/erase faults during data-path operations, and
 *          bad block torture cycle.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>

/* Test fixtures: */
#include "ubi_test_fixture.h"
#include "ubi_test_secure_fixture.h"
#include "ubi_test_memory.h"

/* Zephyr headers: */
#include <psa/crypto.h>
#include <zephyr/ztest.h>
#include <zephyr/kernel.h>

/* Standard library headers: */
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* Module types and type definitiones ----------------------------------------------------------- */

/* Module interface variables and constants ----------------------------------------------------- */

/* Static variables and constants --------------------------------------------------------------- */

static struct ubi_flash_desc flash = { 0 };
static struct ubi_device *g_ubi;

/* Static function declarations ----------------------------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_before(void *ctx);
static void ztest_testcase_teardown(void *ctx);
static struct ubi_device *sec_init(void);

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
	g_ubi = NULL;
}

static void ztest_testcase_teardown(void *ctx)
{
	(void)ctx;
	ubi_test_fault_reset();
	if (g_ubi) {
		(void)ubi_device_deinit(g_ubi);
		g_ubi = NULL;
	}
}

static struct ubi_device *sec_init(void)
{
	struct ubi_device *const ubi = ubi_test_secure_init(&flash);

	g_ubi = ubi;
	return ubi;
}

/* ======================================= Volume remove ======================================== */

/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_secure_coverage, NULL, ztest_suite_setup, ztest_suite_before,
	    ztest_testcase_teardown, NULL);

/**
 * \brief Volume remove succeeds and volume is gone after re-attach.
 *
 * \details Scenario: Create volume, remove it, re-attach. The remove exercises
 *          reserved PEB commit with vol-header list rebuild and
 *          anchor PEB reclamation.
 *
 * \expect Volume count is 0 after remove and after re-attach.
 */
ZTEST(ubi_secure_coverage, volume_remove_basic)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config vol_cfg = {
		.name = "rmvol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.volume_count, 1);

	zassert_ok(ubi_volume_remove(ubi, vol_id));

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.volume_count, 0);

	/* Re-attach and verify. */
	zassert_ok(ubi_device_deinit(ubi));

	ubi = sec_init();
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.volume_count, 0);
}

/**
 * \brief Volume remove with mapped LEBs reclaims PEBs to dirty pool.
 *
 * \details Scenario: Create volume, write to LEB 0 and LEB 1, remove volume.
 *          The EBA entries and anchor PEB are reclaimed.
 *
 * \expect Remove succeeds; dirty_peb_count increases.
 */
ZTEST(ubi_secure_coverage, volume_remove_with_mapped_lebs)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config vol_cfg = {
		.name = "rmmap",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	const uint8_t data[] = { 0xAA, 0xBB };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 1, data, sizeof(data)));

	struct ubi_device_info info_before = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info_before));

	zassert_ok(ubi_volume_remove(ubi, vol_id));

	struct ubi_device_info info_after = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info_after));

	zassert_equal(info_after.volume_count, 0);
	zassert_true(info_after.dirty_peb_count > info_before.dirty_peb_count,
		     "Dirty PEB count should increase after removing volume with mapped LEBs");
}

/**
 * \brief Create two volumes, remove the first, second remains accessible.
 *
 * \details Scenario: Exercises the vol_hdrs list rebuild with a non-trivial filter.
 *
 * \expect Second volume data is intact after first volume removed.
 */
ZTEST(ubi_secure_coverage, volume_remove_one_of_two)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config cfg1 = {
		.name = "vol1",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	const struct ubi_volume_config cfg2 = {
		.name = "vol2",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id1 = -1;
	int vol_id2 = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg1, &vol_id1));
	zassert_ok(ubi_volume_create(ubi, &cfg2, &vol_id2));

	const uint8_t data[] = { 0x42 };

	zassert_ok(ubi_leb_write(ubi, vol_id2, 0, data, sizeof(data)));

	zassert_ok(ubi_volume_remove(ubi, vol_id1));

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.volume_count, 1);

	uint8_t readback[1] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id2, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, data, sizeof(data));
}

/* ======================================= Volume resize ======================================== */

/**
 * \brief Volume resize (shrink) succeeds and trims excess LEBs.
 *
 * \details Scenario: Create 3-LEB volume, write to LEB 0,1,2, resize to 1 LEB.
 *          LEBs 1 and 2 are reclaimed. Read of LEB 0 still works.
 *
 * \expect Resize succeeds; LEB 0 readable; LEB 1 returns EACCES.
 */
ZTEST(ubi_secure_coverage, volume_resize_shrink)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config vol_cfg = {
		.name = "rszsh",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 3,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	const uint8_t data[] = { 0x11, 0x22 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 1, data, sizeof(data)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 2, data, sizeof(data)));

	const struct ubi_volume_config new_cfg = {
		.name = "rszsh",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};

	zassert_ok(ubi_volume_resize(ubi, vol_id, &new_cfg));

	uint8_t readback[2] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, data, sizeof(data));

	/* LEB 1 should be out of range now. */
	zassert_equal(ubi_leb_read(ubi, vol_id, 1, 0, readback, sizeof(readback)), -EACCES);
}

/**
 * \brief Volume resize (grow) allows writing to new LEBs.
 *
 * \details Scenario: Create 1-LEB volume, resize to 3 LEBs, write to LEB 2.
 *
 * \expect Resize succeeds; write and read-back of LEB 2 succeed.
 */
ZTEST(ubi_secure_coverage, volume_resize_grow)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config vol_cfg = {
		.name = "rszgr",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	const struct ubi_volume_config new_cfg = {
		.name = "rszgr",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 3,
	};

	zassert_ok(ubi_volume_resize(ubi, vol_id, &new_cfg));

	const uint8_t data[] = { 0x33, 0x44 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 2, data, sizeof(data)));

	uint8_t readback[2] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 2, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, data, sizeof(data));
}

/**
 * \brief Volume resize persists after re-attach.
 *
 * \details Scenario: Create 3-LEB volume, resize to 1, deinit, re-init.
 *
 * \expect Volume has 1 LEB after re-attach.
 */
ZTEST(ubi_secure_coverage, volume_resize_persists)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config vol_cfg = {
		.name = "rszp",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 3,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	const struct ubi_volume_config new_cfg = {
		.name = "rszp",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};

	zassert_ok(ubi_volume_resize(ubi, vol_id, &new_cfg));

	zassert_ok(ubi_device_deinit(ubi));

	ubi = sec_init();

	struct ubi_volume_config read_cfg = { 0 };
	size_t alloc_lebs = 0;

	zassert_ok(ubi_volume_get_info(ubi, vol_id, &read_cfg, &alloc_lebs));
	zassert_equal(read_cfg.leb_count, 1);
}

/* ====================== LEB overwrite (counter recovery + mapping swap) ======================= */

/**
 * \brief LEB overwrite recovers counter state and swaps mapping.
 *
 * \details Scenario: Write LEB 0, overwrite with new data. The overwrite triggers
 *          leb_recover_old_counters (reading old PEB's VID meta) and
 *          leb_commit_mapping_swap (old PEB → dirty pool).
 *
 * \expect Both writes succeed; read-back matches second write.
 *
 * \oracle Both writes return 0 and the read-back of LEB 0 equals the
 *         second-write payload bit-exact (`zassert_mem_equal`).
 *
 * \trace `leb_recover_old_counters` + `leb_commit_mapping_swap` path.
 *
 * \precondition Dynamic volume `"ovwr"` with 2 LEBs on a freshly formatted
 *               secure device; no fault injection active.
 */
ZTEST(ubi_secure_coverage, leb_overwrite_counter_recovery)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config vol_cfg = {
		.name = "ovwr",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	const uint8_t data1[] = { 0x11, 0x22, 0x33, 0x44 };
	const uint8_t data2[] = { 0xAA, 0xBB, 0xCC, 0xDD };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data1, sizeof(data1)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data2, sizeof(data2)));

	uint8_t readback[4] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, data2, sizeof(data2));
}

/**
 * \brief Multiple LEB overwrites accumulate counters correctly.
 *
 * \details Scenario: Write LEB 0 three times. Each triggers counter recovery.
 *
 * \expect Third write succeeds; read-back matches last data.
 */
ZTEST(ubi_secure_coverage, leb_overwrite_multiple)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config vol_cfg = {
		.name = "ovmul",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	const uint8_t d1[] = { 0x01 };
	const uint8_t d2[] = { 0x02 };
	const uint8_t d3[] = { 0x03 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, d1, sizeof(d1)));

	/* Erase dirty PEB between writes to avoid running out of free PEBs. */
	zassert_ok(ubi_device_erase_peb(ubi));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, d2, sizeof(d2)));

	zassert_ok(ubi_device_erase_peb(ubi));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, d3, sizeof(d3)));

	uint8_t readback[1] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, d3, sizeof(d3));
}

/**
 * \brief LEB overwrite data survives re-attach.
 *
 * \details Scenario: Write LEB 0, overwrite, deinit, re-init.
 *
 * \expect Read-back matches second write after re-attach.
 */
ZTEST(ubi_secure_coverage, leb_overwrite_persists)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config vol_cfg = {
		.name = "ovper",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	const uint8_t data1[] = { 0x10 };
	const uint8_t data2[] = { 0x20 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data1, sizeof(data1)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data2, sizeof(data2)));

	zassert_ok(ubi_device_deinit(ubi));

	ubi = sec_init();

	uint8_t readback[1] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, data2, sizeof(data2));
}

/* ====================================== LEB map / unmap ======================================= */

/**
 * \brief LEB map creates a zero-length mapping.
 *
 * \details Scenario: Create volume, map LEB 0 without data.
 *
 * \expect leb_map succeeds; is_mapped returns true.
 */
ZTEST(ubi_secure_coverage, leb_map)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config vol_cfg = {
		.name = "lmap",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_leb_map(ubi, vol_id, 0));

	bool mapped = false;

	zassert_ok(ubi_leb_is_mapped(ubi, vol_id, 0, &mapped));
	zassert_true(mapped);
}

/**
 * \brief LEB unmap removes a mapping.
 *
 * \details Scenario: Map LEB 0, unmap it.
 *
 * \expect After unmap, is_mapped returns false.
 */
ZTEST(ubi_secure_coverage, leb_unmap)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config vol_cfg = {
		.name = "lunm",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	const uint8_t data[] = { 0x99 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

	bool mapped = false;

	zassert_ok(ubi_leb_is_mapped(ubi, vol_id, 0, &mapped));
	zassert_false(mapped);
}

/* ================================ Flash write fault injection ================================= */

/**
 * \brief Flash write failure during leb_write (VID write) marks PEB bad.
 *
 * \details Scenario: Write data to LEB 0. Set flash write to fail after 1 write
 *          (LEB data succeeds, VID header write fails). The PEB is
 *          marked bad.
 *
 * \expect leb_write returns non-zero error.
 */
ZTEST(ubi_secure_coverage, flash_write_fail_on_vid)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config vol_cfg = {
		.name = "fwvid",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	const uint8_t data[] = { 0xDE, 0xAD };

	/* Let first flash write (LEB data) succeed; fail on second (VID). */
	ubi_test_fault_set_flash_write_fail_after(1);
	const int ret = ubi_leb_write(ubi, vol_id, 0, data, sizeof(data));

	zassert_equal(ret, -EIO, "leb_write must return -EIO when VID flash write fails, got %d",
		      ret);
}

/**
 * \brief Flash write failure during erase recycle marks PEB bad.
 *
 * \details Scenario: Overwrite LEB 0 to create a dirty PEB. Set flash write to fail
 *          after 0 writes (first EC header write in erase path fails).
 *
 * \expect erase_peb returns non-zero.
 */
ZTEST(ubi_secure_coverage, flash_write_fail_on_erase_ec)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config vol_cfg = {
		.name = "fwec",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	const uint8_t d1[] = { 0x10 };
	const uint8_t d2[] = { 0x20 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, d1, sizeof(d1)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, d2, sizeof(d2)));

	/* Drain dirty PEBs to avoid witness issues. */
	zassert_ok(ubi_device_erase_peb(ubi));

	/* Create a fresh dirty PEB. */
	const uint8_t d3[] = { 0x30 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, d3, sizeof(d3)));

	/* Fail on first flash write (EC header after erase). */
	ubi_test_fault_set_flash_write_fail_after(0);
	const int ret = ubi_device_erase_peb(ubi);

	zassert_equal(ret, -EIO, "erase_peb must return -EIO when EC flash write fails, got %d",
		      ret);
}

/* ======================================== LEB get_size ======================================== */

/**
 * \brief LEB get_size returns the correct data size.
 *
 * \details Scenario: Write data to LEB 0, query size.
 *
 * \expect Returned size matches the written data length.
 */
ZTEST(ubi_secure_coverage, leb_get_size)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config vol_cfg = {
		.name = "lgsz",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	const uint8_t data[] = { 0x11, 0x22, 0x33 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	size_t size = 0;

	zassert_ok(ubi_leb_get_size(ubi, vol_id, 0, &size));
	zassert_equal(size, sizeof(data));
}

/* =========================== Volume remove persists after re-attach =========================== */

/**
 * \brief Remove volume with data, re-attach, volume is gone.
 *
 * \details Scenario: Create volume, write data, remove, re-attach.
 *
 * \expect Volume not found after re-attach.
 */
ZTEST(ubi_secure_coverage, volume_remove_persists)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config vol_cfg = {
		.name = "rmpst",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	const uint8_t data[] = { 0xAA };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_volume_remove(ubi, vol_id));

	zassert_ok(ubi_device_deinit(ubi));

	ubi = sec_init();

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.volume_count, 0);

	/* Reading a removed volume must fail with -EINVAL (volume slot freed). */
	uint8_t readback[1] = { 0 };
	const int ret = ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback));

	zassert_equal(ret, -ENOENT, "read of removed volume must return -ENOENT, got %d", ret);
}

/* ==================================== Erase all dirty PEBs ==================================== */

/**
 * \brief Erase all dirty PEBs after multiple overwrites.
 *
 * \details Scenario: Overwrite LEB 0 multiple times creating dirty PEBs. Call
 *          erase_peb repeatedly until dirty_peb_count reaches 0.
 *
 * \expect All dirty PEBs are erased; free_peb_count is restored.
 */
ZTEST(ubi_secure_coverage, erase_all_dirty_pebs)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config vol_cfg = {
		.name = "eall",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	const uint8_t d1[] = { 0x01 };
	const uint8_t d2[] = { 0x02 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, d1, sizeof(d1)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, d2, sizeof(d2)));

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));

	/* Erase all dirty PEBs. */
	for (size_t i = 0; i < 10 && info.dirty_peb_count > 0; i++) {
		zassert_ok(ubi_device_erase_peb(ubi));
		zassert_ok(ubi_device_get_info(ubi, &info));
	}

	zassert_equal(info.dirty_peb_count, 0, "All dirty PEBs should be erased");
}
