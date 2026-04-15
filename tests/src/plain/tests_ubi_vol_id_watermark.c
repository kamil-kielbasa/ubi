/**
 * \file    tests_ubi_vol_id_watermark.c
 *
 * \brief   Tests for the persistent vol_id high-watermark (Task E).
 *
 * Verifies that:
 *   1. vol_id is never reused after remove within the same boot.
 *   2. vol_id is never reused after remove + reinit (reboot).
 *   3. Volume slot re-indexing does not change remaining vol_ids.
 *   4. vol_id overflow fails closed without wrapping.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* --------------------------------------- Include files --------------------------------------- */

/* UBI header: */
#include <ubi.h>
#include <ubi_test.h>
#include "ubi_test_fixture.h"

/* Zephyr headers: */
#include <zephyr/ztest.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>

/* Standard headers: */
#include <string.h>

/* -------------------------------------- Module defines --------------------------------------- */

#define DEV_HDR_SIZE (32U)
#define VOL_HDR_SIZE (48U)
#define NR_OF_RES_PEBS (2U)

/* ---------------------------- Module types and type definitiones ----------------------------- */
/* ------------------------- Module interface variables and constants -------------------------- */
/* ------------------------------ Static variables and constants ------------------------------- */

static struct ubi_mtd mtd = { 0 };
static struct ubi_device *g_ubi;

/* ------------------------------- Static function declarations -------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_testcase_before(void *ctx);
static void ztest_testcase_teardown(void *ctx);

/* -------------------------------- Static function definitions -------------------------------- */

static void *ztest_suite_setup(void)
{
	ubi_test_setup_mtd(&mtd);
	return NULL;
}

static void ztest_testcase_before(void *ctx)
{
	(void)ctx;
	ubi_test_partition_force_release_all();
	ubi_test_fault_reset();
	ubi_test_erase_partition();
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

/* --------------------------- Module interface function definitions --------------------------- */

ZTEST_SUITE(ubi_vol_id_watermark, NULL, ztest_suite_setup, ztest_testcase_before,
	    ztest_testcase_teardown, NULL);

/**
 * \brief vol_id is not reused after remove within the same boot.
 *
 * \details Create vol A (id=0), remove it, create vol B.
 *
 * \expect Vol B gets id=1, not 0.
 */
ZTEST(ubi_vol_id_watermark, volume_id_not_reused_after_remove_same_boot)
{
	struct ubi_device *ubi = ubi_test_init_device(&mtd);
	g_ubi = ubi;

	const struct ubi_volume_config cfg_a = {
		.name = "vol_a",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int id_a = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg_a, &id_a));
	zassert_equal(0, id_a);

	zassert_ok(ubi_volume_remove(ubi, id_a));

	const struct ubi_volume_config cfg_b = {
		.name = "vol_b",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int id_b = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg_b, &id_b));
	zassert_true(id_b > id_a, "vol_id must not be reused (got %d, prev %d)", id_b, id_a);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief vol_id is not reused after remove + reinit (simulated reboot).
 *
 * \details Create vol A (id=0), remove it, deinit, reinit from flash, create
 *          vol B.
 *
 * \expect Vol B gets id=1 because the watermark is persisted in the device
 *         header.
 */
ZTEST(ubi_vol_id_watermark, volume_id_not_reused_after_remove_and_reinit)
{
	struct ubi_device *ubi = ubi_test_init_device(&mtd);
	g_ubi = ubi;

	const struct ubi_volume_config cfg_a = {
		.name = "vol_a",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int id_a = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg_a, &id_a));
	zassert_equal(0, id_a);

	zassert_ok(ubi_volume_remove(ubi, id_a));
	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;

	/* Reinit from flash (simulated reboot). */
	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	g_ubi = ubi;

	const struct ubi_volume_config cfg_b = {
		.name = "vol_b",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int id_b = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg_b, &id_b));
	zassert_true(id_b > id_a, "vol_id must survive reboot (got %d, prev %d)", id_b, id_a);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Volume slot re-indexing after remove does not change vol_ids.
 *
 * \details Create 3 volumes (ids 0, 1, 2). Remove vol 1 (middle).
 *
 * \expect Remaining volumes keep their original vol_ids (0 and 2). After
 *         reinit, a new volume gets id=3 (not 1).
 */
ZTEST(ubi_vol_id_watermark, volume_slot_reindex_does_not_change_remaining_volume_ids)
{
	struct ubi_device *ubi = ubi_test_init_device(&mtd);
	g_ubi = ubi;

	const struct ubi_volume_config cfg0 = {
		.name = "vol_0",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	const struct ubi_volume_config cfg1 = {
		.name = "vol_1",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	const struct ubi_volume_config cfg2 = {
		.name = "vol_2",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};

	int id0 = -1, id1 = -1, id2 = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg0, &id0));
	zassert_ok(ubi_volume_create(ubi, &cfg1, &id1));
	zassert_ok(ubi_volume_create(ubi, &cfg2, &id2));

	zassert_equal(0, id0);
	zassert_equal(1, id1);
	zassert_equal(2, id2);

	/* Remove middle volume. */
	zassert_ok(ubi_volume_remove(ubi, id1));

	/* vol_0 and vol_2 must keep their original vol_ids. */
	struct ubi_volume_config read_cfg = { 0 };
	size_t alloc = 0;
	zassert_ok(ubi_volume_get_info(ubi, id0, &read_cfg, &alloc));
	zassert_equal(0, strncmp("vol_0", read_cfg.name, strlen("vol_0")));

	zassert_ok(ubi_volume_get_info(ubi, id2, &read_cfg, &alloc));
	zassert_equal(0, strncmp("vol_2", read_cfg.name, strlen("vol_2")));

	/* Verify after reinit. */
	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	g_ubi = ubi;

	zassert_ok(ubi_volume_get_info(ubi, id0, &read_cfg, &alloc));
	zassert_equal(0, strncmp("vol_0", read_cfg.name, strlen("vol_0")));

	zassert_ok(ubi_volume_get_info(ubi, id2, &read_cfg, &alloc));
	zassert_equal(0, strncmp("vol_2", read_cfg.name, strlen("vol_2")));

	/* New volume must get id=3, not id=1. */
	const struct ubi_volume_config cfg3 = {
		.name = "vol_3",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int id3 = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg3, &id3));
	zassert_equal(3, id3);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief vol_id overflow fails closed without wrapping.
 *
 * \details Set vol_id_watermark to UINT32_MAX on flash.
 *
 * \expect volume_create returns -ENOSPC. Device state is unchanged.
 */
ZTEST(ubi_vol_id_watermark, volume_id_overflow_fails_closed)
{
	struct ubi_device *ubi = ubi_test_init_device(&mtd);
	g_ubi = ubi;

	/* Write vol_id_watermark = UINT32_MAX directly into the reserved PEB. */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(mtd.partition_id, &fa));

	for (size_t peb = 0; peb < NR_OF_RES_PEBS; ++peb) {
		const size_t base = peb * mtd.erase_block_size;
		uint8_t hdr_buf[DEV_HDR_SIZE] = { 0 };

		zassert_ok(flash_area_read(fa, base, hdr_buf, sizeof(hdr_buf)));

		/* Patch vol_id_watermark field (offset 24, 4 bytes LE). */
		uint32_t max_val = UINT32_MAX;
		memcpy(&hdr_buf[24], &max_val, sizeof(max_val));

		/* Recompute CRC (covers bytes 0..27, CRC at offset 28). */
		uint32_t crc = crc32_ieee(hdr_buf, DEV_HDR_SIZE - sizeof(uint32_t));
		memcpy(&hdr_buf[28], &crc, sizeof(crc));

		zassert_ok(flash_area_erase(fa, base, mtd.erase_block_size));
		zassert_ok(flash_area_write(fa, base, hdr_buf, sizeof(hdr_buf)));
	}

	flash_area_close(fa);

	/* Reinit to pick up the patched header. */
	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;

	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	g_ubi = ubi;

	/* volume_create must fail. */
	const struct ubi_volume_config cfg = {
		.name = "overflow_vol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_equal(-ENOSPC, ubi_volume_create(ubi, &cfg, &vol_id),
		      "create must fail on vol_id overflow");

	/* Device must still be usable for reads. */
	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.volume_count);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
