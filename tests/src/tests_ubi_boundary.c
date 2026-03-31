/**
 * \file    tests_ubi_boundary.c
 *
 * \author  Kamil Kielbasa
 *
 * \brief   Tests for UBI LEB boundary and capacity edge cases.
 *
 * \version 0.9
 * \date    2026-03-26
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files ------------------------------------------------------------------------------- */

/* UBI header: */
#include <ubi.h>

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
#include <string.h>

/* Module defines ------------------------------------------------------------------------------ */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* UBI header sizes (must match lib/src/ubi_io.h). */
#define UBI_EC_HDR_SIZE (16)
#define UBI_VID_HDR_SIZE (32)

/* Module types and type definitiones ---------------------------------------------------------- */
/* Module interface variables and constants ---------------------------------------------------- */
/* Static variables and constants -------------------------------------------------------------- */

static struct ubi_mtd mtd = { 0 };

/* Static function declarations ---------------------------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_after(void *ctx);

static void ztest_testcase_before(void *ctx);
static void ztest_testcase_teardown(void *ctx);

/* Static function definitions ----------------------------------------------------------------- */

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

	zassert_ok(flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE));

	return;
}

static void ztest_testcase_teardown(void *ctx)
{
	(void)ctx;
	return;
}

/* Module interface function definitions ------------------------------------------------------- */

ZTEST_SUITE(ubi_boundary, NULL, ztest_suite_setup, ztest_testcase_before, ztest_testcase_teardown,
	    ztest_suite_after);

/**
 * \brief Verify that writing exactly the maximum LEB data capacity succeeds.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs. Calculate the maximum
 *          writable data per LEB (erase_block_size - EC_HDR - VID_HDR = 8144 bytes
 *          on 8 KB erase blocks). Fill a buffer with a 0xAB pattern of exactly that
 *          size and write it to LEB 0.
 *
 * \expect The write succeeds. The stored size equals the maximum data capacity.
 *         Reading back the full LEB returns identical data byte-for-byte.
 */
ZTEST(ubi_boundary, write_max_leb_data)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "max_leb",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const size_t max_data = mtd.erase_block_size - UBI_EC_HDR_SIZE - UBI_VID_HDR_SIZE;
	uint8_t *wbuf = k_malloc(max_data);
	zassert_not_null(wbuf);
	memset(wbuf, 0xAB, max_data);

	/* Write exactly max capacity — must succeed. */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, wbuf, max_data));
	k_free(wbuf);

	/* Read back and verify. */
	uint8_t *rbuf = k_malloc(max_data);
	zassert_not_null(rbuf);
	memset(rbuf, 0, max_data);

	size_t stored_size = 0;
	zassert_ok(ubi_leb_get_size(ubi, vol_id, 0, &stored_size));
	zassert_equal(stored_size, max_data);

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rbuf, max_data));

	/* Verify all bytes are 0xAB */
	bool all_match = true;
	for (size_t i = 0; i < max_data; i++) {
		if (rbuf[i] != 0xAB) {
			all_match = false;
			break;
		}
	}
	zassert_true(all_match, "Read-back data mismatch");

	k_free(rbuf);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that writing one byte beyond the maximum LEB capacity is rejected.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs. Attempt to write
 *          (max_data + 1) bytes to a single LEB, which exceeds the available
 *          data area after the EC and VID headers.
 *
 * \expect ubi_leb_write() returns -ENOSPC. No data is written to the LEB.
 */
ZTEST(ubi_boundary, write_exceeds_leb_capacity)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "exceed",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const size_t max_data = mtd.erase_block_size - UBI_EC_HDR_SIZE - UBI_VID_HDR_SIZE;
	uint8_t *wbuf = k_malloc(max_data + 1);
	zassert_not_null(wbuf);
	memset(wbuf, 0xCD, max_data + 1);

	/* One byte over capacity — must fail with -ENOSPC. */
	zassert_equal(-ENOSPC, ubi_leb_write(ubi, vol_id, 0, wbuf, max_data + 1));

	k_free(wbuf);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify reading at the exact end boundary of stored data.
 *
 * \details Scenario: Write an 8-byte pattern to LEB 0. Issue a read with
 *          offset=4 and size=4, so that offset + size equals exactly the
 *          stored data size (8 bytes). This tests the boundary condition
 *          where the read touches the last byte of stored data.
 *
 * \expect ubi_leb_read() succeeds. The 4 returned bytes match the tail
 *         portion (bytes 4..7) of the original pattern.
 */
ZTEST(ubi_boundary, read_at_exact_boundary)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "rbound",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t pattern[] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, pattern, sizeof(pattern)));

	/* Read last 4 bytes using offset — offset(4) + size(4) = 8 = stored_size. */
	uint8_t tail[4] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 4, tail, sizeof(tail)));
	zassert_mem_equal(tail, &pattern[4], sizeof(tail));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify writes at and just above the write-block alignment boundary.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs. Write exactly 16 bytes
 *          (aligned to WRITE_BLOCK_SIZE_ALIGNMENT) to LEB 0, and 17 bytes (one
 *          byte over alignment, triggering the partial-block padding path in
 *          ubi_leb_data_write()) to LEB 1. Read both back.
 *
 * \expect Both writes succeed. Read-back data matches the original patterns
 *         exactly, confirming that the padding logic does not corrupt data.
 */
ZTEST(ubi_boundary, write_alignment_boundary)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "align",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Write exactly 16 bytes (aligned) */
	uint8_t aligned[16];
	memset(aligned, 0xAA, sizeof(aligned));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, aligned, sizeof(aligned)));

	uint8_t rbuf[16] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rbuf, sizeof(rbuf)));
	zassert_mem_equal(aligned, rbuf, sizeof(aligned));

	/* Write 17 bytes (1 byte over alignment — exercises padding path) */
	uint8_t unaligned[17];
	memset(unaligned, 0xBB, sizeof(unaligned));
	zassert_ok(ubi_leb_write(ubi, vol_id, 1, unaligned, sizeof(unaligned)));

	uint8_t rbuf2[17] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, rbuf2, sizeof(rbuf2)));
	zassert_mem_equal(unaligned, rbuf2, sizeof(unaligned));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify writes smaller than the write-block alignment (< 16 bytes).
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs. Write 1 byte (the
 *          minimum possible write) to LEB 0 and 15 bytes (just under the 16-byte
 *          alignment boundary) to LEB 1. These sizes exercise the small-buffer
 *          padding path in ubi_leb_data_write() where the data is smaller than
 *          a single alignment unit.
 *
 * \expect Both writes succeed. ubi_leb_get_size() returns the exact logical
 *         size (1 and 15). Read-back data matches the original bytes.
 */
ZTEST(ubi_boundary, write_sub_alignment)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "sub",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Write 1 byte — minimum possible write */
	const uint8_t one = 0xFF;
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, &one, 1));

	size_t stored = 0;
	zassert_ok(ubi_leb_get_size(ubi, vol_id, 0, &stored));
	zassert_equal(stored, 1);

	uint8_t rb = 0;
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, &rb, 1));
	zassert_equal(rb, 0xFF);

	/* Write 15 bytes — just under alignment */
	uint8_t buf15[15];
	memset(buf15, 0xCC, sizeof(buf15));
	zassert_ok(ubi_leb_write(ubi, vol_id, 1, buf15, sizeof(buf15)));

	uint8_t rbuf15[15] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, rbuf15, sizeof(rbuf15)));
	zassert_mem_equal(buf15, rbuf15, sizeof(buf15));

	zassert_ok(ubi_device_deinit(ubi));
}

/* --- Sequence number monotonicity across remount --- */

/* Raw VID header for direct flash reads. */
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

/**
 * \brief Verify that global_sqnum is strictly monotonic after device re-init.
 *
 * \details Scenario: init → create volume → write LEB 0 → deinit →
 *          init → write LEB 1 → read raw VID headers of both LEBs.
 *
 * \expect The second write's sqnum is strictly greater than the first's.
 */
ZTEST(ubi_boundary, sqnum_monotonic_across_remount)
{
	/* First session: write LEB 0. */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "sqn",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data1[] = { 0xAA };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data1, sizeof(data1)));

	zassert_ok(ubi_device_deinit(ubi));

	/* Second session: write LEB 1. */
	ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	/* Re-read vol_id after remount. */
	struct ubi_volume_config cfg2;
	size_t alloc;
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &cfg2, &alloc));

	const uint8_t data2[] = { 0xBB };
	zassert_ok(ubi_leb_write(ubi, vol_id, 1, data2, sizeof(data2)));

	/* Read raw VID headers to compare sqnums. */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(FIXED_PARTITION_ID(ubi_partition), &fa));

	const size_t nr_of_pebs = fa->fa_size / mtd.erase_block_size;
	uint64_t sqnum_leb0 = 0;
	uint64_t sqnum_leb1 = 0;

	for (size_t p = CONFIG_UBI_DEV_HDR_NR_OF_RES_PEBS; p < nr_of_pebs; ++p) {
		struct raw_vid_hdr vid;
		zassert_ok(flash_area_read(fa, (p * mtd.erase_block_size) + UBI_EC_HDR_SIZE, &vid,
					   sizeof(vid)));

		if (vid.magic != 0x55424921)
			continue;

		if (vid.vol_id == (uint32_t)vol_id && vid.lnum == 0)
			sqnum_leb0 = vid.sqnum;
		else if (vid.vol_id == (uint32_t)vol_id && vid.lnum == 1)
			sqnum_leb1 = vid.sqnum;
	}

	flash_area_close(fa);

	zassert_true(sqnum_leb0 > 0, "LEB 0 sqnum must be non-zero");
	zassert_true(sqnum_leb1 > sqnum_leb0, "LEB 1 sqnum (%llu) must be > LEB 0 sqnum (%llu)",
		     sqnum_leb1, sqnum_leb0);

	zassert_ok(ubi_device_deinit(ubi));
}
