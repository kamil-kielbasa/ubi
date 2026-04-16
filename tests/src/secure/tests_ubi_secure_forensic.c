/**
 * \file    tests_ubi_secure_forensic.c
 * \author  Kamil Kielbasa
 *
 * \brief   Portable forensic scan: verify that plaintext data and known
 *          secrets never appear on the flash medium after secure writes.
 *
 * \details Scans the raw flash partition after secure write operations,
 *          searching for:
 *          - known plaintext test data patterns,
 *          - volume name strings,
 *          - test key material,
 *          - ASCII artefacts that should never appear on encrypted media.
 *
 *          This test is portable to hardware (uses flash_area_read, not host
 *          file access). For the host-side Python equivalent, see
 *          scripts/scan_flash.py.
 *
 * Requires CONFIG_UBI_CRYPTO=y and CONFIG_UBI_TEST_API_ENABLE=y.
 *
 * \copyright Copyright (c) 2026
 */

/* --------------------------------------- Include files --------------------------------------- */
#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>
#include "arrays.h"

#include "ubi_test_secure_fixture.h"

#include <psa/crypto.h>

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>

#include <errno.h>
#include <string.h>

/* -------------------------------------- Module defines --------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/** Scan buffer size — must fit one erase block. */
#define SCAN_BUF_SIZE 8192

/* ------------------------------------- Static variables -------------------------------------- */

static struct ubi_mtd mtd = { 0 };
static struct ubi_device *g_ubi = NULL;

/* ----------------------------------- Forensic scan helpers ----------------------------------- */

/**
 * \brief Search for a byte pattern in a buffer.
 *
 * \return true if pattern found, false otherwise.
 */
static bool buf_contains_pattern(const uint8_t *haystack, size_t haystack_len,
				 const uint8_t *needle, size_t needle_len)
{
	if (needle_len == 0 || needle_len > haystack_len) {
		return false;
	}

	for (size_t i = 0; i <= haystack_len - needle_len; i++) {
		if (memcmp(&haystack[i], needle, needle_len) == 0) {
			return true;
		}
	}

	return false;
}

/**
 * \brief Scan the entire flash partition for a forbidden pattern.
 *
 * Reads one erase block at a time and searches for the pattern.
 * Skips reserved PEBs (0 and 1) since those contain authenticated
 * metadata, not user data.
 *
 * \return true if pattern found anywhere in data PEB area, false otherwise.
 */
static bool flash_contains_pattern(const uint8_t *pattern, size_t pattern_len)
{
	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(mtd.partition_id, &fa));

	static uint8_t scan_buf[SCAN_BUF_SIZE];
	bool found = false;

	/* Scan only data PEBs (skip reserved PEBs 0 and 1). */
	const size_t start_offset = 2 * mtd.erase_block_size;
	const size_t end_offset = UBI_PARTITION_SIZE;

	for (size_t off = start_offset; off < end_offset; off += mtd.erase_block_size) {
		const size_t read_len = MIN(mtd.erase_block_size, SCAN_BUF_SIZE);

		zassert_ok(flash_area_read(fa, off, scan_buf, read_len));

		if (buf_contains_pattern(scan_buf, read_len, pattern, pattern_len)) {
			found = true;
			break;
		}
	}

	flash_area_close(fa);
	return found;
}

/* ---------------------------------- Suite setup / teardown ----------------------------------- */

static void *ztest_suite_setup(void)
{
	const struct device *flash_dev = UBI_PARTITION_DEVICE;

	zassert_true(device_is_ready(flash_dev));

	struct flash_pages_info page_info = { 0 };

	zassert_ok(flash_get_page_info_by_offs(flash_dev, 0, &page_info));

	mtd.partition_id = FIXED_PARTITION_ID(UBI_PARTITION_NAME);
	mtd.erase_block_size = page_info.size;
	mtd.write_block_size = flash_get_write_block_size(flash_dev);

	zassert_equal(psa_crypto_init(), PSA_SUCCESS);
	ubi_test_import_root_key();

	return NULL;
}

static void ztest_suite_before(void *ctx)
{
	ARG_UNUSED(ctx);
	g_ubi = NULL;
	ubi_test_fault_reset();
	ubi_test_partition_force_release_all();
	zassert_ok(flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE));
}

static void ztest_testcase_after(void *ctx)
{
	(void)ctx;
	if (g_ubi != NULL) {
		(void)ubi_device_deinit(g_ubi);
		g_ubi = NULL;
	}
}

/* ------------------------------------------- Tests ------------------------------------------- */

/**
 * \brief Verify that plaintext write data does not appear on flash.
 *
 * \details Write known plaintext arrays (array_128, array_256) through the
 *          secure backend, deinit, then scan all data PEBs for the raw
 *          plaintext bytes. Encrypted data must not match the original.
 *
 * \expected Neither array_128 nor array_256 found anywhere in data PEB area.
 */
ZTEST(ubi_secure_forensic, test_plaintext_data_absent_after_write)
{
	const struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'f', 'r', 'n', '1' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));
	g_ubi = ubi;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	/* Write known plaintext patterns. */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_128, ARRAY_SIZE(array_128)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 1, array_256, ARRAY_SIZE(array_256)));

	/* Deinit so all buffers are flushed. */
	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));

	/* Forensic scan: plaintext must not appear on flash. */
	zassert_false(flash_contains_pattern(array_128, ARRAY_SIZE(array_128)),
		      "Plaintext array_128 found on flash — encryption failure");
	zassert_false(flash_contains_pattern(array_256, ARRAY_SIZE(array_256)),
		      "Plaintext array_256 found on flash — encryption failure");
}

/**
 * \brief Verify that volume name strings do not appear in plaintext on flash.
 *
 * \details Create a volume with a recognizable ASCII name, write data,
 *          deinit, then scan data PEBs for the raw name bytes. Volume
 *          names are stored in reserved PEB metadata (authenticated +
 *          encrypted), and should not leak into data PEB area.
 *
 * \expected Volume name bytes not found in data PEB area.
 */
ZTEST(ubi_secure_forensic, test_volume_name_absent_in_data_area)
{
	const struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'S', 'E', 'C', 'R', 'E', 'T' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));
	g_ubi = ubi;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_128, ARRAY_SIZE(array_128)));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));

	/* The name "/SECRET" should not appear in data PEB area. */
	const uint8_t name_pattern[] = { '/', 'S', 'E', 'C', 'R', 'E', 'T' };

	zassert_false(flash_contains_pattern(name_pattern, sizeof(name_pattern)),
		      "Volume name found in data PEB area — metadata leakage");
}

/**
 * \brief Verify that test root key material does not appear on flash.
 *
 * \details After secure format and write, scan the entire data PEB area
 *          for the raw 16-byte test root key material. Key material must
 *          never be written to flash.
 *
 * \expected Raw key material not found on flash.
 */
ZTEST(ubi_secure_forensic, test_key_material_absent_on_flash)
{
	const struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'f', 'r', 'n', '3' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));
	g_ubi = ubi;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_128, ARRAY_SIZE(array_128)));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));

	/* Root key material must never appear anywhere on flash. */
	zassert_false(flash_contains_pattern(UBI_TEST_ROOT_KEY_MATERIAL,
					     sizeof(UBI_TEST_ROOT_KEY_MATERIAL)),
		      "Root key material found on flash — key leakage");
}

/**
 * \brief Verify that plaintext data is absent after overwrite and erase.
 *
 * \details Write data, overwrite with different data, unmap, erase dirty
 *          PEBs, then scan for both the old and new plaintext. Neither
 *          should be present: old data was overwritten + erased, new data
 *          was encrypted.
 *
 * \expected Neither old nor new plaintext found on flash after erase cycle.
 */
ZTEST(ubi_secure_forensic, test_plaintext_absent_after_overwrite_and_erase)
{
	const struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'f', 'r', 'n', '4' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));
	g_ubi = ubi;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	/* Write, then overwrite. */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_128, ARRAY_SIZE(array_128)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_256, ARRAY_SIZE(array_256)));

	/* Unmap to make the PEB dirty, then erase. */
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));

	while (info.dirty_peb_count > 0) {
		zassert_ok(ubi_device_erase_peb(ubi));
		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(ubi, &info));
	}

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));

	/* Neither old nor new plaintext should be on flash. */
	zassert_false(flash_contains_pattern(array_128, ARRAY_SIZE(array_128)),
		      "Old plaintext (array_128) found after overwrite+erase");
	zassert_false(flash_contains_pattern(array_256, ARRAY_SIZE(array_256)),
		      "New plaintext (array_256) found after overwrite+erase");
}

/**
 * \brief Negative test: verify forensic scan detects plaintext on plain backend.
 *
 * \details Format a plain device (no encryption), write known data, then
 *          scan for it. This validates that the forensic scan itself works —
 *          plaintext written without encryption MUST be found.
 *
 * \expected array_128 IS found on flash (plain mode does not encrypt).
 */
ZTEST(ubi_secure_forensic, test_plain_backend_plaintext_is_detectable)
{
	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'p', 'l', 'n', '1' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	/* Init as PLAIN (no crypto config). */
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	g_ubi = ubi;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_128, ARRAY_SIZE(array_128)));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));

	/* On plain backend, plaintext MUST be on flash — validates the scanner. */
	zassert_true(flash_contains_pattern(array_128, ARRAY_SIZE(array_128)),
		     "Forensic scan failed to detect plaintext on plain backend — scanner bug");
}

/* ------------------------------------ Suite registration ------------------------------------- */

ZTEST_SUITE(ubi_secure_forensic, NULL, ztest_suite_setup, ztest_suite_before, ztest_testcase_after,
	    NULL);
