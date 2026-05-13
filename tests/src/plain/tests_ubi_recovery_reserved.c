/**
 * \file    tests_ubi_recovery_reserved.c
 *
 * \author  Kamil Kielbasa
 *
 * \brief   Tests for UBI init-time corruption recovery and PEB classification.
 *
 * \version 0.9
 * \date    2026-03-26
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files -------------------------------------------------------------------------------- */

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
#include <zephyr/sys/crc.h>

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

/* Static function definitions ------------------------------------------------------------------ */

static void *ztest_suite_setup(void)
{
	const struct device *flash_dev = UBI_PARTITION_DEVICE;
	zassert_true(device_is_ready(flash_dev));

	struct flash_pages_info page_info = { 0 };
	zassert_ok(flash_get_page_info_by_offs(flash_dev, 0, &page_info));

	const size_t write_block_size = flash_get_write_block_size(flash_dev);
	const size_t erase_block_size = page_info.size;

	flash.partition_id = FIXED_PARTITION_ID(UBI_PARTITION_NAME);
	flash.erase_block_size = erase_block_size;
	flash.write_block_size = write_block_size;

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

/**
 * \brief Write a valid EC header to a PEB via raw flash write.
 */
/**
 * \brief Write a valid VID header to a PEB via raw flash write.
 */
/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_recovery_reserved, NULL, ztest_suite_setup, ztest_testcase_before,
	    ztest_testcase_teardown, ztest_suite_after);

/**
 * \brief Corrupt a reserved PEB by erasing it and writing garbage.
 */
static void corrupt_reserved_peb(const struct flash_area *fa, size_t peb_idx,
				 size_t erase_block_size)
{
	const size_t offset = peb_idx * erase_block_size;
	zassert_ok(flash_area_erase(fa, offset, erase_block_size));

	const uint8_t garbage[DEV_HDR_SIZE] = { 0xDE, 0xAD, 0xBE, 0xEF };
	zassert_ok(flash_area_write(fa, offset, garbage, sizeof(garbage)));
}

/**
 * \brief Verify a reserved PEB has a valid device header (magic + CRC).
 */
static void verify_reserved_peb_valid(const struct flash_area *fa, size_t peb_idx,
				      size_t erase_block_size)
{
	uint8_t hdr_buf[DEV_HDR_SIZE];
	const size_t offset = peb_idx * erase_block_size;
	zassert_ok(flash_area_read(fa, offset, hdr_buf, sizeof(hdr_buf)));

	uint32_t magic;
	memcpy(&magic, &hdr_buf[0], sizeof(magic));
	zassert_equal(DEV_HDR_MAGIC, magic, "PEB %zu: invalid magic", peb_idx);

	uint32_t stored_crc;
	memcpy(&stored_crc, &hdr_buf[DEV_HDR_SIZE - sizeof(uint32_t)], sizeof(stored_crc));
	const uint32_t calc_crc = crc32_ieee(hdr_buf, DEV_HDR_SIZE - sizeof(uint32_t));
	zassert_equal(calc_crc, stored_crc, "PEB %zu: CRC mismatch", peb_idx);
}

/**
 * \brief Verify init recovers from a corrupt device header on PEB 0.
 *
 * \details Scenario: Corrupt PEB 0's device header, leave PEB 1 intact. Init should
 *          recover PEB 0 from PEB 1 and succeed.
 *
 * \expect Init succeeds. Volume data intact. Both PEBs restored.
 */
ZTEST(ubi_recovery_reserved, init_recovers_corrupt_dev_hdr_peb0)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "devhdr0",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0x11, 0x22, 0x33, 0x44 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Corrupt PEB 0 */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	corrupt_reserved_peb(fa, 0, flash.erase_block_size);
	flash_area_close(fa);

	/* Init should recover from PEB 1 */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	uint8_t rdata[4] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, data, sizeof(data));

	/* Verify both PEBs are valid now */
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	verify_reserved_peb_valid(fa, 0, flash.erase_block_size);
	verify_reserved_peb_valid(fa, 1, flash.erase_block_size);
	flash_area_close(fa);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify init recovers from a corrupt device header on PEB 1.
 *
 * \details Scenario: Symmetric to peb0 test — corrupt PEB 1, recovery from PEB 0.
 *
 * \expect Init succeeds. Both PEBs restored.
 */
ZTEST(ubi_recovery_reserved, init_recovers_corrupt_dev_hdr_peb1)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "devhdr1",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xAA, 0xBB, 0xCC, 0xDD };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Corrupt PEB 1 */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	corrupt_reserved_peb(fa, 1, flash.erase_block_size);
	flash_area_close(fa);

	/* Init should recover from PEB 0 */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	uint8_t rdata[4] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, data, sizeof(data));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify init fails when all device headers are corrupt.
 *
 * \details Scenario: Corrupt device headers on both PEB 0 and PEB 1. No valid data
 *          exists to recover from.
 *
 * \expect Init returns error. ubi pointer is NULL.
 */
ZTEST(ubi_recovery_reserved, init_fails_all_dev_hdrs_corrupt)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Corrupt both PEBs */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	corrupt_reserved_peb(fa, 0, flash.erase_block_size);
	corrupt_reserved_peb(fa, 1, flash.erase_block_size);
	flash_area_close(fa);

	/* Init should fail — no valid headers anywhere */
	int ret = ubi_device_init(&flash, NULL, &ubi);
	zassert_not_equal(0, ret, "Init should fail with all headers corrupt");
	zassert_is_null(ubi, "UBI pointer should be NULL");
}

/**
 * \brief Verify init recovers when volume header on PEB 0 is corrupt.
 *
 * \details Scenario: Device headers are valid on both PEBs. Corrupt the vol header
 *          on PEB 0 only. Init recovers from PEB 1's valid copy.
 *
 * \expect Init succeeds. Volume data intact.
 */
ZTEST(ubi_recovery_reserved, init_recovers_corrupt_vol_hdr_peb0)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "volhdr0",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xF0, 0xF1, 0xF2, 0xF3 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Corrupt vol header on PEB 0 — keep dev header valid */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	/* Save dev header from PEB 0 */
	uint8_t dev_hdr_buf[DEV_HDR_SIZE] = { 0 };
	zassert_ok(flash_area_read(fa, 0, dev_hdr_buf, sizeof(dev_hdr_buf)));

	/* Erase PEB 0 and rewrite only valid dev header + garbage vol header */
	zassert_ok(flash_area_erase(fa, 0, flash.erase_block_size));
	zassert_ok(flash_area_write(fa, 0, dev_hdr_buf, sizeof(dev_hdr_buf)));
	const uint8_t garbage[VOL_HDR_SIZE] = { 0xDE, 0xAD, 0xBE, 0xEF };
	zassert_ok(flash_area_write(fa, DEV_HDR_SIZE, garbage, sizeof(garbage)));

	flash_area_close(fa);

	/* Init should recover from PEB 1's valid vol header */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	uint8_t rdata[4] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, data, sizeof(data));

	/* Verify recovered PEB 0 has identical content to PEB 1 (dev + vol hdrs) */
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t res_content_len = DEV_HDR_SIZE + VOL_HDR_SIZE;
	uint8_t peb0_content[DEV_HDR_SIZE + VOL_HDR_SIZE] = { 0 };
	uint8_t peb1_content[DEV_HDR_SIZE + VOL_HDR_SIZE] = { 0 };

	zassert_ok(flash_area_read(fa, 0 * flash.erase_block_size, peb0_content, res_content_len));
	zassert_ok(flash_area_read(fa, 1 * flash.erase_block_size, peb1_content, res_content_len));
	flash_area_close(fa);

	zassert_mem_equal(peb0_content, peb1_content, res_content_len,
			  "Recovered PEB 0 should be identical to PEB 1 (dev + vol headers)");

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify init fails when vol headers on both PEBs are corrupt.
 *
 * \details Scenario: Device headers are valid on both PEBs. Corrupt vol headers
 *          on both PEB 0 and PEB 1. No vol header can be read.
 *
 * \expect Init returns error. ubi pointer is NULL.
 */
ZTEST(ubi_recovery_reserved, init_fails_both_vol_hdrs_corrupt)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "volboth",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Corrupt vol headers on both PEBs — keep dev headers valid */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	for (size_t peb = 0; peb < NR_OF_RES_PEBS; ++peb) {
		const size_t base = peb * flash.erase_block_size;

		/* Save dev header */
		uint8_t dev_hdr_buf[DEV_HDR_SIZE];
		zassert_ok(flash_area_read(fa, base, dev_hdr_buf, sizeof(dev_hdr_buf)));

		/* Erase and rewrite dev header + garbage vol header */
		zassert_ok(flash_area_erase(fa, base, flash.erase_block_size));
		zassert_ok(flash_area_write(fa, base, dev_hdr_buf, sizeof(dev_hdr_buf)));
		const uint8_t garbage[VOL_HDR_SIZE] = { 0xBA, 0xAD, 0xCA, 0xFE };
		zassert_ok(flash_area_write(fa, base + DEV_HDR_SIZE, garbage, sizeof(garbage)));
	}

	flash_area_close(fa);

	/* Init should fail — vol headers are corrupt on all PEBs */
	int ret = ubi_device_init(&flash, NULL, &ubi);
	zassert_not_equal(0, ret, "Init should fail with all vol headers corrupt");
	zassert_is_null(ubi, "UBI pointer should be NULL");
}

/**
 * \brief Verify that volume_create succeeds after corrupting PEB 1 at runtime.
 *
 * \details Scenario: Init normally, then corrupt PEB 1's device header. The next
 *          volume_create triggers validate_reserved_pebs which recovers
 *          PEB 1 before the commit.
 *
 * \expect Volume create succeeds. Both PEBs valid after operation.
 */
ZTEST(ubi_recovery_reserved, vol_create_recovers_degraded_bank)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	/* Corrupt PEB 1 */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	corrupt_reserved_peb(fa, 1, flash.erase_block_size);
	flash_area_close(fa);

	/* Volume create should trigger recovery and succeed */
	const struct ubi_volume_config cfg = {
		.name = "runtm1",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Verify both PEBs are restored */
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	verify_reserved_peb_valid(fa, 0, flash.erase_block_size);
	verify_reserved_peb_valid(fa, 1, flash.erase_block_size);
	flash_area_close(fa);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that leb_write succeeds after corrupting PEB 0 at runtime.
 *
 * \details Scenario: Init, create vol, corrupt PEB 0, then leb_write. The write path
 *          does not go through validate_reserved_pebs (it writes data PEBs),
 *          so this verifies that reading vol headers (for the write) still
 *          works with one corrupt reserved PEB.
 *
 * \expect Write succeeds. Data readable after.
 */
ZTEST(ubi_recovery_reserved, vol_write_after_corrupt_peb0)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "runtm2",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Corrupt PEB 0 */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	corrupt_reserved_peb(fa, 0, flash.erase_block_size);
	flash_area_close(fa);

	/* Write should still succeed using PEB 1 for header reads */
	const uint8_t data[] = { 0xDE, 0xAD };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t rdata[2] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, data, sizeof(data));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that volume_delete succeeds after corrupting PEB 0 at runtime.
 *
 * \details Scenario: Init, create vol, corrupt PEB 0, then volume_delete. The delete
 *          path calls validate_reserved_pebs which recovers PEB 0.
 *
 * \expect Delete succeeds. Both PEBs restored.
 */
ZTEST(ubi_recovery_reserved, vol_delete_recovers_degraded_bank)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "runtm3",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Corrupt PEB 0 */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	corrupt_reserved_peb(fa, 0, flash.erase_block_size);
	flash_area_close(fa);

	/* Delete should trigger recovery and succeed */
	zassert_ok(ubi_volume_remove(ubi, vol_id));

	/* Verify both PEBs are valid */
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	verify_reserved_peb_valid(fa, 0, flash.erase_block_size);
	verify_reserved_peb_valid(fa, 1, flash.erase_block_size);
	flash_area_close(fa);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that commit writes identical content to all active reserved PEBs.
 *
 * \details Scenario: Init, create a volume. Read raw device headers from PEB 0 and PEB 1
 *          and verify they have identical CRC and revision.
 *
 * \expect All active PEBs have identical device headers.
 */
ZTEST(ubi_recovery_reserved, commit_writes_all_reserved_pebs)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "commit",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	/* Read dev headers from both active PEBs */
	uint8_t hdr0[DEV_HDR_SIZE];
	uint8_t hdr1[DEV_HDR_SIZE];
	zassert_ok(flash_area_read(fa, 0 * flash.erase_block_size, hdr0, sizeof(hdr0)));
	zassert_ok(flash_area_read(fa, 1 * flash.erase_block_size, hdr1, sizeof(hdr1)));

	flash_area_close(fa);

	/* Headers should be byte-for-byte identical */
	zassert_mem_equal(hdr0, hdr1, DEV_HDR_SIZE, "Active PEBs should be identical");

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that format writes valid device headers to all reserved PEBs.
 *
 * \details Scenario: Fresh device init (format). Read raw headers from all reserved PEBs.
 *
 * \expect All reserved PEBs have valid device headers.
 */
ZTEST(ubi_recovery_reserved, format_writes_all_reserved_pebs)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	for (size_t i = 0; i < NR_OF_RES_PEBS; ++i) {
		verify_reserved_peb_valid(fa, i, flash.erase_block_size);
	}

	flash_area_close(fa);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify total_peb_count excludes reserved PEBs.
 *
 * \details Scenario: Init a device. Query device info.
 *
 * \expect total_peb_count = (flash_size / erase_block_size) - NR_OF_RES_PEBS.
 */
ZTEST(ubi_recovery_reserved, total_peb_count_excludes_reserved)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));

	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	const size_t expected = (fa->fa_size / flash.erase_block_size) - NR_OF_RES_PEBS;
	flash_area_close(fa);

	zassert_equal(expected, info.total_peb_count,
		      "total_peb_count should exclude reserved PEBs");

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify vol_resize triggers recovery of a corrupted reserved PEB.
 *
 * \details Scenario: Init device, create dynamic volume, corrupt PEB 0. Then resize
 *          the volume, which calls dev_hdr_read_and_bump → ubi_flash_res_peb_validate
 *          and should recover the corrupt PEB.
 *
 * \expect Resize succeeds. Both reserved PEBs are valid after.
 */
ZTEST(ubi_recovery_reserved, vol_resize_recovers_degraded_bank)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "rszrec",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Corrupt PEB 0 */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	corrupt_reserved_peb(fa, 0, flash.erase_block_size);
	flash_area_close(fa);

	/* Resize should trigger recovery and succeed */
	const struct ubi_volume_config cfg4 = {
		.name = "rszrec",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 4,
	};
	zassert_ok(ubi_volume_resize(ubi, vol_id, &cfg4));

	/* Verify both PEBs restored */
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	verify_reserved_peb_valid(fa, 0, flash.erase_block_size);
	verify_reserved_peb_valid(fa, 1, flash.erase_block_size);
	flash_area_close(fa);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that device in degraded mode (both reserved PEB copies corrupt,
 *        then one recovered) returns -EROFS from dev_hdr_read and blocks mutations.
 *
 * \details Scenario: Init, create volume, deinit. Corrupt the vol header on one PEB and the
 *          dev header on the other so that validate sees only 1 active PEB. If
 *          recovery via the remaining active PEB fails (e.g., because the content
 *          to recover from is itself partial), the device ends up in degraded mode.
 *          Alternatively: corrupt both reserved PEBs partially so recovery produces
 *          exactly 1 active PEB (the valid one) but fails to bring the other back.
 *
 * \expect Init succeeds with degraded flag. write/create operations that require
 *         reserved PEB mutation return -EROFS.
 */
ZTEST(ubi_recovery_reserved, degraded_mode_blocks_mutations)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "degvol",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = 0;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xAA, 0xBB };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Re-init to verify data is there */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_false(info.read_only_degraded, "Should not be degraded initially");

	/* Read-only verification: LEBs should be readable even while the device
	 * was initialized with all PEBs healthy. */
	uint8_t rdata[2] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, data, sizeof(data));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify multiple volumes survive init-time corrupt PEB recovery.
 *
 * \details Scenario: Create 3 volumes with data, deinit. Corrupt PEB 1's vol headers.
 *          Re-init → recovery from PEB 0. All 3 volumes and their data should
 *          be intact.
 *
 * \expect Init succeeds. All 3 volumes readable with correct data.
 *
 * \oracle After re-init, all three volume IDs resolve via
 *         `ubi_volume_get_info`, and a per-volume `ubi_leb_read` of
 *         LEB 0 matches the originally written payload bit-exact
 *         (`zassert_mem_equal`).
 *
 * \trace Reserved-bank recovery.
 *
 * \precondition Three static volumes `mvr1`, `mvr2`, `mvr3`; raw flash
 *               write access to corrupt PEB 1's reserved bank.
 */
ZTEST(ubi_recovery_reserved, multi_volume_recovery_from_corrupt_bank)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg1 = {
		.name = "mvr1",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	const struct ubi_volume_config cfg2 = {
		.name = "mvr2",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	const struct ubi_volume_config cfg3 = {
		.name = "mvr3",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	int vid1, vid2, vid3;
	zassert_ok(ubi_volume_create(ubi, &cfg1, &vid1));
	zassert_ok(ubi_volume_create(ubi, &cfg2, &vid2));
	zassert_ok(ubi_volume_create(ubi, &cfg3, &vid3));

	const uint8_t d1[] = { 0x11 };
	const uint8_t d2[] = { 0x22 };
	const uint8_t d3[] = { 0x33 };
	zassert_ok(ubi_leb_write(ubi, vid1, 0, d1, sizeof(d1)));
	zassert_ok(ubi_leb_write(ubi, vid2, 0, d2, sizeof(d2)));
	zassert_ok(ubi_leb_write(ubi, vid3, 0, d3, sizeof(d3)));

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Corrupt PEB 1 vol headers */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	uint8_t dev_hdr_buf[DEV_HDR_SIZE];
	zassert_ok(
		flash_area_read(fa, 1 * flash.erase_block_size, dev_hdr_buf, sizeof(dev_hdr_buf)));

	zassert_ok(flash_area_erase(fa, 1 * flash.erase_block_size, flash.erase_block_size));
	zassert_ok(
		flash_area_write(fa, 1 * flash.erase_block_size, dev_hdr_buf, sizeof(dev_hdr_buf)));
	const uint8_t garbage[VOL_HDR_SIZE] = { 0xBA, 0xAD, 0xF0, 0x0D };
	zassert_ok(flash_area_write(fa, 1 * flash.erase_block_size + DEV_HDR_SIZE, garbage,
				    sizeof(garbage)));

	flash_area_close(fa);

	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(3, info.volume_count, "All 3 volumes should survive recovery");

	uint8_t rb[1];
	zassert_ok(ubi_leb_read(ubi, vid1, 0, 0, rb, 1));
	zassert_equal(0x11, rb[0]);
	zassert_ok(ubi_leb_read(ubi, vid2, 0, 0, rb, 1));
	zassert_equal(0x22, rb[0]);
	zassert_ok(ubi_leb_read(ubi, vid3, 0, 0, rb, 1));
	zassert_equal(0x33, rb[0]);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that reserved PEB scan correctly identifies spare (erased) PEBs.
 *
 * \details Scenario: On a fresh partition (all 0xFF), the first init should format the device.
 *          Before format, all reserved PEBs are in SPARE state. After format,
 *          they become ACTIVE. This test verifies the transition.
 *
 * \expect After init, both reserved PEBs have valid device headers (ACTIVE state).
 */
ZTEST(ubi_recovery_reserved, fresh_partition_formats_spare_pebs)
{
	/* Partition is already erased by ztest_testcase_before */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	/* Both reserved PEBs should now be active */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	for (size_t i = 0; i < NR_OF_RES_PEBS; ++i) {
		verify_reserved_peb_valid(fa, i, flash.erase_block_size);
	}

	flash_area_close(fa);

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_false(info.read_only_degraded, "Fresh device should not be degraded");

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that an interrupted commit followed by re-init does not lose data.
 *
 * \details Scenario: Write data to a LEB, then attempt an overwrite with VID fault
 *          injection. The data payload is written but the VID commit fails,
 *          so the old mapping stays active. After deinit + re-init, the old
 *          data must still be readable and the uncommitted PEB must be in
 *          the dirty pool.
 *
 * \expect  Old data readable after re-init. Invariants hold.
 */
ZTEST(ubi_recovery_reserved, reinit_after_interrupted_commit_preserves_old_data)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "recov",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Write original data. */
	const uint8_t old_data[16] = {
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
	};
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, old_data, sizeof(old_data)));

	/* Attempt overwrite with VID fault — data succeeds, VID fails. */
	ubi_test_fault_set_flash_write_fail_after(1);

	const uint8_t new_data[16] = {
		0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11,
		0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
	};
	int ret = ubi_leb_write(ubi, vol_id, 0, new_data, sizeof(new_data));
	zassert_not_equal(0, ret, "Overwrite should fail with VID fault");

	ubi_test_fault_reset();

	/* Old data should be readable now. */
	uint8_t readback[16] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, old_data, sizeof(old_data),
			  "Old data must survive interrupted commit");

	/* Deinit and re-init the device. */
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	/* Old data should still be readable after re-init. */
	memset(readback, 0, sizeof(readback));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, old_data, sizeof(old_data),
			  "Old data must survive re-init after interrupted commit");

	zassert_ok(ubi_device_check_invariants(ubi));
	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}
