/**
 * \file    tests_ubi_recovery.c
 *
 * \author  Kamil Kielbasa
 *
 * \brief   Tests for UBI init-time corruption recovery and PEB classification.
 *
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>
#include <ubi_test.h>

/* Test fixtures: */
#include "ubi_test_fixture.h"

/* Zephyr headers: */
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <zephyr/toolchain/common.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/sys/crc.h>

/* Standard library headers: */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* On-flash header sizes and magic numbers (must match ubi_utils.h) */
#define EC_HDR_MAGIC (0x55424923U)
#define EC_HDR_SIZE (16U)
#define VID_HDR_MAGIC (0x55424921U)
#define VID_HDR_SIZE (32U)
#define DEV_HDR_MAGIC (0x55424925U)
#define DEV_HDR_SIZE (32U)
#define VOL_HDR_MAGIC (0x55424926U)
#define VOL_HDR_SIZE (48U)
#define NR_OF_RES_PEBS (2U)

/* Module types and type definitiones ----------------------------------------------------------- */

/* Packed representations of on-flash headers for raw writes. */
struct raw_ec_hdr {
	uint32_t magic;
	uint8_t version;
	uint8_t padding[3];
	uint32_t ec;
	uint32_t hdr_crc;
};

struct raw_vid_hdr {
	uint32_t magic;
	uint8_t version;
	uint8_t padding[3];
	uint32_t lnum;
	uint32_t vol_id;
	uint64_t sqnum;
	uint32_t data_size;
	uint32_t hdr_crc;
};

/* Module interface variables and constants ----------------------------------------------------- */

/* Static variables and constants --------------------------------------------------------------- */

static struct ubi_flash_desc flash = { 0 };

/* Static function declarations ----------------------------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_after(void *ctx);

static void ztest_testcase_before(void *ctx);
static void ztest_testcase_teardown(void *ctx);
static void raw_write_ec_hdr(const struct flash_area *fa, size_t pnum, size_t erase_block_size,
			     uint32_t ec);
static void raw_write_vid_hdr(const struct flash_area *fa, size_t pnum, size_t erase_block_size,
			      uint32_t lnum, uint32_t vol_id, uint64_t sqnum, uint32_t data_size);

/* Static function definitions ------------------------------------------------------------------ */

static void *ztest_suite_setup(void)
{
	ubi_test_setup_mtd(&flash);
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
	ubi_test_erase_partition();
}

static void ztest_testcase_teardown(void *ctx)
{
	(void)ctx;
	return;
}

/**
 * \brief Write a valid EC header to a PEB via raw flash write.
 */
static void raw_write_ec_hdr(const struct flash_area *fa, size_t pnum, size_t erase_block_size,
			     uint32_t ec)
{
	struct raw_ec_hdr hdr = {
		.magic = EC_HDR_MAGIC,
		.version = 1,
		.padding = { 0 },
		.ec = ec,
		.hdr_crc = 0,
	};
	hdr.hdr_crc = crc32_ieee((const uint8_t *)&hdr, sizeof(hdr) - sizeof(hdr.hdr_crc));

	const size_t offset = pnum * erase_block_size;
	zassert_ok(flash_area_write(fa, offset, &hdr, sizeof(hdr)));
}

/**
 * \brief Write a valid VID header to a PEB via raw flash write.
 */
static void raw_write_vid_hdr(const struct flash_area *fa, size_t pnum, size_t erase_block_size,
			      uint32_t lnum, uint32_t vol_id, uint64_t sqnum, uint32_t data_size)
{
	struct raw_vid_hdr hdr = {
		.magic = VID_HDR_MAGIC,
		.version = 1,
		.padding = { 0 },
		.lnum = lnum,
		.vol_id = vol_id,
		.sqnum = sqnum,
		.data_size = data_size,
		.hdr_crc = 0,
	};
	hdr.hdr_crc = crc32_ieee((const uint8_t *)&hdr, sizeof(hdr) - sizeof(hdr.hdr_crc));

	const size_t offset = (pnum * erase_block_size) + EC_HDR_SIZE;
	zassert_ok(flash_area_write(fa, offset, &hdr, sizeof(hdr)));
}

/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_recovery, NULL, ztest_suite_setup, ztest_testcase_before, ztest_testcase_teardown,
	    ztest_suite_after);

/**
 * \brief Verify that a PEB with a corrupt EC header is classified as bad during init.
 *
 * \details Scenario: Initialize the device normally and write data to a static
 *          volume. Deinitialize, then erase PEB 2 (the first data PEB) and write
 *          garbage bytes (invalid magic) at its EC header location via raw flash
 *          I/O. Re-initialize the device.
 *          This exercises init scan branch 4.1 (EC header read failure).
 *
 * \expect ubi_device_init() succeeds. ubi_device_get_info() reports
 *         bad_peb_count >= 1, confirming the corrupted PEB was isolated.
 */
ZTEST(ubi_recovery, corrupt_ec_header_becomes_bad_peb)
{
	/* First, do a normal init + deinit so device/volume headers exist. */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "rec1",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xAA, 0xBB, 0xCC, 0xDD };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Corrupt a data PEB's EC header with garbage bytes. */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	/* PEB 2 is the first data PEB. Erase it first (required on real
	 * hardware where flash bits can only go 1→0), then write bad magic. */
	const size_t peb2_offset = NR_OF_RES_PEBS * flash.erase_block_size;
	zassert_ok(flash_area_erase(fa, peb2_offset, flash.erase_block_size));

	const uint8_t garbage[EC_HDR_SIZE] = { 0xDE, 0xAD, 0xBE, 0xEF };
	zassert_ok(flash_area_write(fa, peb2_offset, garbage, sizeof(garbage)));

	flash_area_close(fa);

	/* Re-init: the corrupted PEB should be classified as bad. */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.bad_peb_count >= 1, "Expected at least 1 bad PEB after EC corruption");

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that a PEB with a valid EC but corrupt VID CRC is classified as bad.
 *
 * \details Scenario: Initialize the device normally and write data through the
 *          UBI API. Deinitialize, then erase PEB 2 and write a valid EC header
 *          followed by a VID header with an intentionally wrong CRC (0xDEADBEEF)
 *          via raw flash I/O. Re-initialize the device.
 *          This exercises init scan branch 4.3 (VID CRC validation failure).
 *
 * \expect ubi_device_init() succeeds. ubi_device_get_info() reports
 *         bad_peb_count >= 1, confirming the PEB with the bad VID CRC
 *         was isolated as a bad block.
 */
ZTEST(ubi_recovery, corrupt_vid_crc_becomes_bad_peb)
{
	/* Normal init to set up device/volume headers. */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "rec2",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0x11, 0x22, 0x33, 0x44 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Now corrupt the VID header on PEB that holds LEB 0. */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	/* Find a PEB with valid EC header (PEB 2 or later). We'll use PEB 2. */
	/* First erase PEB 2, write valid EC, then write VID with bad CRC. */
	const size_t peb_idx = NR_OF_RES_PEBS;
	const size_t peb_offset = peb_idx * flash.erase_block_size;

	zassert_ok(flash_area_erase(fa, peb_offset, flash.erase_block_size));

	/* Write valid EC header */
	raw_write_ec_hdr(fa, peb_idx, flash.erase_block_size, 1);

	/* Write VID header with corrupted CRC (valid magic but wrong CRC) */
	struct raw_vid_hdr bad_vid = {
		.magic = VID_HDR_MAGIC,
		.version = 1,
		.padding = { 0 },
		.lnum = 0,
		.vol_id = (uint32_t)vol_id,
		.sqnum = 1,
		.data_size = sizeof(data),
		.hdr_crc = 0xDEADBEEF, /* intentionally wrong CRC */
	};
	zassert_ok(flash_area_write(fa, peb_offset + EC_HDR_SIZE, &bad_vid, sizeof(bad_vid)));

	flash_area_close(fa);

	/* Re-init: PEB with bad VID CRC should be classified as bad. */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.bad_peb_count >= 1,
		     "Expected at least 1 bad PEB after VID CRC corruption");

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that a PEB with a valid EC and empty VID is classified as free.
 *
 * \details Scenario: Initialize and deinitialize the device so that the
 *          device and volume headers exist on reserved PEBs. Then erase a data
 *          PEB and write only a valid EC header via raw flash I/O, leaving the
 *          VID header area at the erased state (all 0xFF). Re-initialize.
 *          This exercises init scan branch 4.2 (empty VID detection).
 *
 * \expect ubi_device_init() succeeds. ubi_device_get_info() reports
 *         free_peb_count >= 1, confirming the PEB was classified as free
 *         and available for allocation.
 */
ZTEST(ubi_recovery, valid_ec_empty_vid_becomes_free_peb)
{
	/* Normal init so device/volume headers are written. */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Erase a data PEB and write only a valid EC header (VID stays 0xFF). */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t peb_idx = NR_OF_RES_PEBS;
	const size_t peb_offset = peb_idx * flash.erase_block_size;

	zassert_ok(flash_area_erase(fa, peb_offset, flash.erase_block_size));
	raw_write_ec_hdr(fa, peb_idx, flash.erase_block_size, 5);

	flash_area_close(fa);

	/* Re-init: PEB should be classified as free. */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.free_peb_count >= 1, "PEB with valid EC + empty VID should be free");

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that a PEB referencing a non-existent volume is classified as dirty.
 *
 * \details Scenario: Initialize the device and create a volume. Deinitialize,
 *          then erase a data PEB and write a valid EC header followed by a valid
 *          VID header that references vol_id=9999 — a volume that does not exist
 *          in the device header. Re-initialize.
 *          This exercises init scan branch 4.4.3 (volume not found).
 *
 * \expect ubi_device_init() succeeds. ubi_device_get_info() reports
 *         dirty_peb_count >= 1, confirming the orphaned PEB was classified
 *         as dirty and is eligible for erasure and reuse.
 */
ZTEST(ubi_recovery, vid_orphan_volume_becomes_dirty_peb)
{
	/* Init and create a volume, then deinit. */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "rec4",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Write valid EC + VID on a PEB, but with a vol_id that doesn't exist. */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t peb_idx = NR_OF_RES_PEBS;
	const size_t peb_offset = peb_idx * flash.erase_block_size;

	zassert_ok(flash_area_erase(fa, peb_offset, flash.erase_block_size));
	raw_write_ec_hdr(fa, peb_idx, flash.erase_block_size, 1);

	/* Use a vol_id that doesn't match any real volume (e.g., 9999). */
	raw_write_vid_hdr(fa, peb_idx, flash.erase_block_size, 0, 9999, 1, 4);

	flash_area_close(fa);

	/* Re-init: PEB should be classified as dirty (orphan volume reference). */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.dirty_peb_count >= 1, "Orphan vol_id VID should produce dirty PEB");

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that duplicate LEBs with different sequence numbers are resolved
 *        in favor of the higher sqnum during init.
 *
 * \details Scenario: Initialize the device, create a volume, and write to LEB 0
 *          twice (generating two PEBs with increasing sqnums). Erase the dirty
 *          PEB so it becomes free. Deinitialize, then find a free PEB via raw
 *          flash scan and inject a duplicate VID header mapping the same
 *          (vol_id, lnum=0) but with sqnum=1 (lower than the existing mapping).
 *          Re-initialize.
 *          This exercises init scan branches 4.4.7.1 (lower sqnum → dirty) and
 *          4.4.7.2 (higher sqnum replaces in EBA).
 *
 * \expect ubi_device_init() succeeds. dirty_peb_count >= 1 (the lower-sqnum
 *         duplicate was moved to dirty). Reading LEB 0 returns the data from
 *         the second write (the one with the higher sqnum), confirming correct
 *         conflict resolution.
 */
ZTEST(ubi_recovery, duplicate_leb_sqnum_conflict_resolution)
{
	/* Init, create a volume, write data to LEB 0, deinit. */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "rec5",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Write to LEB 0 to get a valid mapping. */
	const uint8_t data1[] = { 0x01, 0x02, 0x03, 0x04 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data1, sizeof(data1)));

	/* Write again to get a second PEB with higher sqnum (old goes dirty). */
	const uint8_t data2[] = { 0x0A, 0x0B, 0x0C, 0x0D };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data2, sizeof(data2)));

	/* Erase the dirty PEB so it becomes free. */
	zassert_ok(ubi_device_erase_peb(ubi));

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Now manually create a duplicate: pick a free PEB (one with valid EC
	 * and empty VID), erase it, and write duplicate headers for LEB 0
	 * with a lower sqnum than the existing one. */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t nr_of_pebs = fa->fa_size / flash.erase_block_size;
	size_t free_peb = 0;

	/* Find a PEB with valid EC header + empty VID (starts with 0xFF). */
	for (size_t p = NR_OF_RES_PEBS; p < nr_of_pebs; ++p) {
		/* Read EC header magic */
		uint32_t ec_magic;
		zassert_ok(flash_area_read(fa, p * flash.erase_block_size, &ec_magic,
					   sizeof(ec_magic)));

		if (ec_magic != EC_HDR_MAGIC) {
			continue;
		}

		/* Read VID header magic */
		uint32_t vid_magic = 0;
		zassert_ok(flash_area_read(fa, (p * flash.erase_block_size) + EC_HDR_SIZE,
					   &vid_magic, sizeof(vid_magic)));

		if (vid_magic == 0xFFFFFFFF) {
			/* This is a free PEB (valid EC, empty VID). */
			free_peb = p;
			break;
		}
	}

	zassert_true(free_peb >= NR_OF_RES_PEBS, "Must find a free PEB for the test");

	/* Erase this PEB and write duplicate EC + VID with low sqnum=1. */
	zassert_ok(flash_area_erase(fa, free_peb * flash.erase_block_size, flash.erase_block_size));
	raw_write_ec_hdr(fa, free_peb, flash.erase_block_size, 0);
	raw_write_vid_hdr(fa, free_peb, flash.erase_block_size, 0, (uint32_t)vol_id, 1,
			  sizeof(data1));

	flash_area_close(fa);

	/* Re-init: should resolve the conflict in favor of the higher sqnum. */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));

	/* The lower-sqnum duplicate should be classified as dirty. */
	zassert_true(info.dirty_peb_count >= 1, "Lower-sqnum duplicate should become dirty");

	/* LEB 0 should still contain the newer data. */
	uint8_t rdata[4] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, data2, sizeof(data2));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_device_erase_peb() is a safe no-op when no dirty PEBs exist.
 *
 * \details Scenario: Initialize a clean device with no volumes and no prior
 *          write activity. Confirm dirty_peb_count is 0. Call
 *          ubi_device_erase_peb().
 *
 * \expect The call returns 0. Device state (free_peb_count, dirty_peb_count)
 *         remains unchanged, confirming no unintended side effects.
 */
ZTEST(ubi_recovery, erase_peb_no_dirty)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.dirty_peb_count);

	/* Erase with nothing dirty should be a no-op. */
	zassert_ok(ubi_device_erase_peb(ubi));

	/* Device state should be unchanged. */
	struct ubi_device_info info2;
	zassert_ok(ubi_device_get_info(ubi, &info2));
	zassert_equal(info.free_peb_count, info2.free_peb_count);
	zassert_equal(0, info2.dirty_peb_count);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_device_init() recovers when the volume header on one
 *        reserved PEB is corrupt.
 *
 * \details Scenario: Initialize the device and create a volume. Deinitialize,
 *          then corrupt the volume header on reserved PEB 1 by writing garbage
 *          bytes at the volume header offset while preserving the device header.
 *          Re-initialize. The recovery logic detects the corrupt PEB and restores
 *          it from the valid PEB.
 *
 * \expect ubi_device_init() succeeds. The volume is accessible and data can be
 *         read after recovery.
 */
ZTEST(ubi_recovery, init_recovers_corrupt_vol_header)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "corrvol",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xCA, 0xFE, 0xBA, 0xBE };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Read the device header from bank 0. */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	uint8_t dev_hdr_buf[DEV_HDR_SIZE];
	zassert_ok(flash_area_read(fa, 0, dev_hdr_buf, sizeof(dev_hdr_buf)));

	/* Corrupt the volume header on bank 1 only. Bank 0 stays valid. */
	const size_t bank1_offset = 1 * flash.erase_block_size;
	zassert_ok(flash_area_erase(fa, bank1_offset, flash.erase_block_size));
	/* Rewrite valid device header on bank 1. */
	zassert_ok(flash_area_write(fa, bank1_offset, dev_hdr_buf, sizeof(dev_hdr_buf)));
	/* Write garbage over the volume header area on bank 1. */
	const uint8_t garbage[VOL_HDR_SIZE] = { 0xBA, 0xAD, 0xF0, 0x0D };
	zassert_ok(flash_area_write(fa, bank1_offset + DEV_HDR_SIZE, garbage, sizeof(garbage)));

	flash_area_close(fa);

	/* Re-init should succeed: recovery reads vol header from bank 0. */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	/* Verify volume data is intact after recovery. */
	uint8_t rdata[4] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, data, sizeof(data));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that a LEB exceeding the volume's leb_count is classified as dirty during init.
 *
 * \details Scenario: Initialize the device, create a volume with leb_count=2,
 *          write data to LEBs 0 and 1. Resize down to leb_count=1 and deinitialize.
 *          Erase dirty PEBs. Then inject a PEB with LEB=1 for this vol_id on a
 *          free PEB. Since the volume header says leb_count=1, LEB=1 is out of
 *          bounds and init scan branch 4.4.4 classifies it as dirty.
 *
 * \expect ubi_device_init() succeeds. dirty_peb_count >= 1 because the
 *         out-of-bounds LEB was moved to the dirty list. LEB 0 data is intact.
 */
ZTEST(ubi_recovery, leb_exceeds_volume_count_becomes_dirty)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	/* Create volume with 2 LEBs, write to both. */
	const struct ubi_volume_config cfg2 = {
		.name = "shrink",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg2, &vol_id));

	const uint8_t data0[] = { 0xA0, 0xA1 };
	const uint8_t data1[] = { 0xB0, 0xB1 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data0, sizeof(data0)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 1, data1, sizeof(data1)));

	/* Resize down to 1 LEB — LEB 1 gets moved to dirty and erased. */
	const struct ubi_volume_config cfg1 = {
		.name = "shrink",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	zassert_ok(ubi_volume_resize(ubi, vol_id, &cfg1));

	/* Erase dirty PEBs so the old LEB 1 PEB becomes free. */
	struct ubi_device_info info_pre = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_pre));
	for (size_t i = 0; i < info_pre.dirty_peb_count + 1; ++i)
		zassert_ok(ubi_device_erase_peb(ubi));

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Inject a PEB with LEB=1 for this vol_id on a free PEB. */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t nr_of_pebs = fa->fa_size / flash.erase_block_size;
	size_t free_peb = 0;

	for (size_t p = NR_OF_RES_PEBS; p < nr_of_pebs; ++p) {
		uint32_t ec_magic;
		zassert_ok(flash_area_read(fa, p * flash.erase_block_size, &ec_magic,
					   sizeof(ec_magic)));
		if (ec_magic != EC_HDR_MAGIC)
			continue;

		uint32_t vid_magic = 0;
		zassert_ok(flash_area_read(fa, (p * flash.erase_block_size) + EC_HDR_SIZE,
					   &vid_magic, sizeof(vid_magic)));
		if (vid_magic == 0xFFFFFFFF) {
			free_peb = p;
			break;
		}
	}
	zassert_true(free_peb >= NR_OF_RES_PEBS, "Need a free PEB");

	zassert_ok(flash_area_erase(fa, free_peb * flash.erase_block_size, flash.erase_block_size));
	raw_write_ec_hdr(fa, free_peb, flash.erase_block_size, 0);
	/* LEB 1 with this vol_id — exceeds leb_count=1, so out-of-bounds. */
	raw_write_vid_hdr(fa, free_peb, flash.erase_block_size, 1, (uint32_t)vol_id, 1,
			  sizeof(data1));

	flash_area_close(fa);

	/* Re-init: The out-of-bounds LEB should be classified as dirty. */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.dirty_peb_count >= 1, "Out-of-bounds LEB should produce dirty PEB");

	/* LEB 0 data should still be intact. */
	uint8_t rdata[2] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, data0, sizeof(data0));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify duplicate LEB resolution when the existing EBA entry's EC header is corrupt.
 *
 * \details Scenario: Initialize the device, create a volume, and write to LEB 0.
 *          Deinitialize. Find the PEB that holds LEB 0 and corrupt its EC header
 *          (overwrite with garbage). Then inject a second PEB with a valid EC+VID
 *          mapping the same (vol_id, LEB 0). Re-initialize.
 *          Because the original PEB has a corrupt EC header, it gets classified as
 *          bad during init phase 4.1. The injected PEB is then inserted normally
 *          via phase 4.4.5.
 *
 * \expect ubi_device_init() succeeds. bad_peb_count >= 1 (the PEB with the corrupt
 *         EC was moved to bad blocks).
 */
ZTEST(ubi_recovery, duplicate_leb_with_corrupt_existing_ec)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "dupec",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xDA, 0xDB, 0xDC, 0xDD };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Find the PEB that has a valid VID for (vol_id, LEB 0). */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t nr_of_pebs = fa->fa_size / flash.erase_block_size;
	size_t leb0_peb = 0;
	size_t free_peb = 0;

	for (size_t p = NR_OF_RES_PEBS; p < nr_of_pebs; ++p) {
		uint32_t ec_magic;
		zassert_ok(flash_area_read(fa, p * flash.erase_block_size, &ec_magic,
					   sizeof(ec_magic)));
		if (ec_magic != EC_HDR_MAGIC)
			continue;

		struct raw_vid_hdr vid;
		zassert_ok(flash_area_read(fa, (p * flash.erase_block_size) + EC_HDR_SIZE, &vid,
					   sizeof(vid)));

		if (vid.magic == VID_HDR_MAGIC && vid.vol_id == (uint32_t)vol_id && vid.lnum == 0) {
			leb0_peb = p;
		} else if (vid.magic == 0xFFFFFFFF && free_peb == 0) {
			free_peb = p;
		}
	}

	zassert_true(leb0_peb >= NR_OF_RES_PEBS, "Must find PEB for LEB 0");
	zassert_true(free_peb >= NR_OF_RES_PEBS, "Must find a free PEB");

	/* Corrupt the EC header of the PEB that currently holds LEB 0. */
	const uint8_t garbage[EC_HDR_SIZE] = { 0xFE, 0xED, 0xFA, 0xCE };
	zassert_ok(flash_area_erase(fa, leb0_peb * flash.erase_block_size, flash.erase_block_size));
	zassert_ok(
		flash_area_write(fa, leb0_peb * flash.erase_block_size, garbage, sizeof(garbage)));

	/* Inject a new valid PEB for (vol_id, LEB 0) on the free PEB. */
	zassert_ok(flash_area_erase(fa, free_peb * flash.erase_block_size, flash.erase_block_size));
	raw_write_ec_hdr(fa, free_peb, flash.erase_block_size, 0);
	raw_write_vid_hdr(fa, free_peb, flash.erase_block_size, 0, (uint32_t)vol_id, 100,
			  sizeof(data));

	flash_area_close(fa);

	/* Re-init: The corrupt-EC PEB goes to bad blocks (phase 4.1).
	 * The injected PEB is inserted normally (phase 4.4.5). */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.bad_peb_count >= 1, "PEB with corrupt EC should be bad");

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_device_init() with a higher sqnum duplicate replaces the existing
 *        LEB and the existing entry goes to dirty.
 *
 * \details Scenario: Init, create volume, write LEB 0 with sqnum X. Deinit.
 *          Inject a PEB with valid headers for same (vol_id, LEB 0) but with a
 *          sqnum much higher than X. Re-init. The existing entry (lower sqnum)
 *          should be replaced: older goes dirty, newer stays in EBA.
 *          This verifies init scan branch 4.4.7.2 with the higher sqnum winning.
 *
 * \expect ubi_device_init() succeeds. dirty_peb_count >= 1.
 */
ZTEST(ubi_recovery, duplicate_leb_higher_sqnum_replaces_existing)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "duphigh",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data_old[] = { 0x10, 0x20, 0x30 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data_old, sizeof(data_old)));

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Find a free PEB to inject the duplicate. */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t nr_of_pebs = fa->fa_size / flash.erase_block_size;
	size_t free_peb = 0;

	for (size_t p = NR_OF_RES_PEBS; p < nr_of_pebs; ++p) {
		uint32_t ec_magic;
		zassert_ok(flash_area_read(fa, p * flash.erase_block_size, &ec_magic,
					   sizeof(ec_magic)));
		if (ec_magic != EC_HDR_MAGIC)
			continue;

		uint32_t vid_magic = 0;
		zassert_ok(flash_area_read(fa, (p * flash.erase_block_size) + EC_HDR_SIZE,
					   &vid_magic, sizeof(vid_magic)));
		if (vid_magic == 0xFFFFFFFF) {
			free_peb = p;
			break;
		}
	}
	zassert_true(free_peb >= NR_OF_RES_PEBS, "Must find a free PEB");

	/* Inject a duplicate with very high sqnum (9999) so it wins. */
	const uint8_t data_new[] = { 0xA0, 0xB0, 0xC0 };
	zassert_ok(flash_area_erase(fa, free_peb * flash.erase_block_size, flash.erase_block_size));
	raw_write_ec_hdr(fa, free_peb, flash.erase_block_size, 0);
	raw_write_vid_hdr(fa, free_peb, flash.erase_block_size, 0, (uint32_t)vol_id, 9999,
			  sizeof(data_new));

	/* Pad the raw write to flash write-block alignment (hardware requirement). */
	uint8_t aligned_buf[16] = { 0 };
	memcpy(aligned_buf, data_new, sizeof(data_new));
	zassert_ok(flash_area_write(
		fa, (free_peb * flash.erase_block_size) + EC_HDR_SIZE + VID_HDR_SIZE, aligned_buf,
		sizeof(aligned_buf)));

	flash_area_close(fa);

	/* Re-init: higher sqnum PEB should replace existing, old goes dirty. */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.dirty_peb_count >= 1, "Lower-sqnum PEB should become dirty");

	/* Reading LEB 0 should return the new data. */
	uint8_t rdata[3] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, data_new, sizeof(data_new));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that a corrupt EC header on a data PEB with valid VID is classified as bad
 *        even when the VID is semantically valid.
 *
 * \details Scenario: The EC header is the first check in init_scan_pebs. If it fails, the PEB
 *          goes straight to bad blocks regardless of VID state. Write valid VID data
 *          after a corrupt EC to verify the EC check is definitive.
 *
 * \expect bad_peb_count >= 1. Data PEB with corrupt EC and valid VID is bad.
 */
ZTEST(ubi_recovery, corrupt_ec_with_valid_vid_still_bad)
{
	/* Init and create volume with data */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "ecvid",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0x55, 0x66, 0x77, 0x88 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Corrupt EC header on PEB 2 but write valid VID after it */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t peb_idx = NR_OF_RES_PEBS;
	const size_t peb_offset = peb_idx * flash.erase_block_size;

	zassert_ok(flash_area_erase(fa, peb_offset, flash.erase_block_size));

	/* Write garbage EC header */
	const uint8_t garbage[EC_HDR_SIZE] = { 0xBA, 0xAD, 0xBA, 0xAD };
	zassert_ok(flash_area_write(fa, peb_offset, garbage, sizeof(garbage)));

	/* Write a valid VID header after the corrupt EC */
	raw_write_vid_hdr(fa, peb_idx, flash.erase_block_size, 0, (uint32_t)vol_id, 1,
			  sizeof(data));

	flash_area_close(fa);

	/* Re-init: EC check happens first, PEB should be bad */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.bad_peb_count >= 1,
		     "PEB with corrupt EC should be bad regardless of VID state");

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that multiple corrupt data PEBs are all classified correctly during init.
 *
 * \details Scenario: Corrupt EC headers on 3 data PEBs. Re-init and verify bad_peb_count == 3.
 *
 * \expect bad_peb_count == 3. Remaining PEBs are free.
 */
ZTEST(ubi_recovery, multiple_corrupt_pebs_all_classified)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t nr_of_pebs = fa->fa_size / flash.erase_block_size;
	const size_t corrupt_count = (nr_of_pebs - NR_OF_RES_PEBS >= 3) ? 3 : 1;

	for (size_t i = 0; i < corrupt_count; ++i) {
		const size_t peb_idx = NR_OF_RES_PEBS + i;
		const size_t offset = peb_idx * flash.erase_block_size;

		zassert_ok(flash_area_erase(fa, offset, flash.erase_block_size));
		const uint8_t garbage[EC_HDR_SIZE] = { 0xDE, 0xAD, 0xBE, 0xEF };
		zassert_ok(flash_area_write(fa, offset, garbage, sizeof(garbage)));
	}

	flash_area_close(fa);

	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(corrupt_count, info.bad_peb_count,
		      "All corrupt PEBs should be classified as bad");

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that a PEB with valid EC, erased VID, and erased data is classified as free.
 *
 * \details Scenario: Init probes the data area when VID is erased. If the data area
 *          prefix is also erased, the PEB is genuinely free.
 *
 * \expect  PEB is in the free pool. free_peb_count includes this PEB.
 */
ZTEST(ubi_recovery, valid_ec_erased_vid_and_erased_data_is_free)
{
	/* Normal init so device/volume headers are written. */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info_baseline = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_baseline));

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Erase one data PEB, write only EC (VID + data remain erased). */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t peb_idx = NR_OF_RES_PEBS;
	const size_t peb_offset = peb_idx * flash.erase_block_size;

	zassert_ok(flash_area_erase(fa, peb_offset, flash.erase_block_size));
	raw_write_ec_hdr(fa, peb_idx, flash.erase_block_size, 7);

	flash_area_close(fa);

	/* Re-init: PEB should be classified as free. */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info_after = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_after));

	zassert_true(info_after.free_peb_count >= 1,
		     "PEB with valid EC + erased VID + erased data should be free");
	/* Fresh device — same as baseline since we only recreated the same PEB state. */
	zassert_equal(info_after.free_peb_count, info_baseline.free_peb_count,
		      "Free PEB count should match baseline");
	zassert_equal(info_after.dirty_peb_count, 0, "No dirty PEBs expected");

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that a PEB with valid EC, erased VID, but non-erased data is dirty.
 *
 * \details Scenario: An interrupted commit can leave data written but VID erased.
 *          Init must not classify this as free (which would cause data
 *          corruption when reused). Instead, the PEB must be classified
 *          as dirty (uncommitted).
 *
 * \expect  PEB is in the dirty pool. dirty_peb_count includes this PEB.
 */
ZTEST(ubi_recovery, valid_ec_erased_vid_and_present_data_is_dirty)
{
	/* Normal init so device/volume headers are written. */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info_baseline = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_baseline));

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t peb_idx = NR_OF_RES_PEBS;
	const size_t peb_offset = peb_idx * flash.erase_block_size;

	/* Erase the PEB, write valid EC header. */
	zassert_ok(flash_area_erase(fa, peb_offset, flash.erase_block_size));
	raw_write_ec_hdr(fa, peb_idx, flash.erase_block_size, 3);

	/* Write a few non-erased bytes at the start of the data area.
	 * Data area starts at EC_HDR_SIZE + VID_HDR_SIZE = 48 within the PEB. */
	const uint8_t dirty_data[16] = {
		0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04,
		0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
	};
	const size_t data_area_offset = peb_offset + EC_HDR_SIZE + VID_HDR_SIZE;
	zassert_ok(flash_area_write(fa, data_area_offset, dirty_data, sizeof(dirty_data)));

	flash_area_close(fa);

	/* Re-init: PEB should be classified as dirty (uncommitted). */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info_after = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_after));

	zassert_true(info_after.dirty_peb_count >= 1,
		     "PEB with valid EC + erased VID + present data should be dirty");
	zassert_equal(info_after.free_peb_count, info_baseline.free_peb_count - 1,
		      "One fewer free PEB (moved to dirty)");

	zassert_ok(ubi_device_deinit(ubi));
}
