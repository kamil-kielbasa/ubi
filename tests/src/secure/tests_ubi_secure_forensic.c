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

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>
#include "arrays.h"

/* Test fixtures: */
#include "ubi_test_secure_fixture.h"

/* Zephyr headers: */
#include <psa/crypto.h>
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>

/* Standard library headers: */
#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/** Scan buffer size — must fit one erase block. */
#define SCAN_BUF_SIZE 8192

/* Module types and type definitiones ----------------------------------------------------------- */

/* Module interface variables and constants ----------------------------------------------------- */

/* Static variables and constants --------------------------------------------------------------- */
static struct ubi_flash_desc flash = { 0 };
static struct ubi_device *g_ubi = NULL;

/* Static function declarations ----------------------------------------------------------------- */

static bool buf_contains_pattern(const uint8_t *haystack, size_t haystack_len,
				 const uint8_t *needle, size_t needle_len);
static bool flash_contains_pattern(const uint8_t *pattern, size_t pattern_len);
static void *ztest_suite_setup(void);
static void ztest_suite_before(void *ctx);
static void ztest_testcase_after(void *ctx);

/* Static function definitions ------------------------------------------------------------------ */

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

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	static uint8_t scan_buf[SCAN_BUF_SIZE];
	bool found = false;

	/* Scan only data PEBs (skip reserved PEBs 0 and 1). */
	const size_t start_offset = 2 * flash.erase_block_size;
	const size_t end_offset = UBI_PARTITION_SIZE;

	for (size_t off = start_offset; off < end_offset; off += flash.erase_block_size) {
		const size_t read_len = MIN(flash.erase_block_size, SCAN_BUF_SIZE);

		zassert_ok(flash_area_read(fa, off, scan_buf, read_len));

		if (buf_contains_pattern(scan_buf, read_len, pattern, pattern_len)) {
			found = true;
			break;
		}
	}

	flash_area_close(fa);
	return found;
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
	g_ubi = NULL;
}

static void ztest_testcase_after(void *ctx)
{
	(void)ctx;
	if (g_ubi != NULL) {
		(void)ubi_device_deinit(g_ubi);
		g_ubi = NULL;
	}
}

/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_secure_forensic, NULL, ztest_suite_setup, ztest_suite_before, ztest_testcase_after,
	    NULL);

/**
 * \brief Verify that plaintext write data does not appear on flash.
 *
 * \details Scenario: Write known plaintext arrays (array_128, array_256) through the
 *          secure backend, deinit, then scan all data PEBs for the raw
 *          plaintext bytes. Encrypted data must not match the original.
 *
 * \expect Neither array_128 nor array_256 found anywhere in data PEB area.
 */
ZTEST(ubi_secure_forensic, plaintext_data_absent_after_write)
{
	const struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'f', 'r', 'n', '1' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	/* Write known plaintext patterns. */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_128, ARRAY_SIZE(array_128)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 1, array_256, ARRAY_SIZE(array_256)));

	/* Deinit so all buffers are flushed. */
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
 * \details Scenario: Create a volume with a recognizable ASCII name, write data,
 *          deinit, then scan data PEBs for the raw name bytes. Volume
 *          names are stored in reserved PEB metadata (authenticated +
 *          encrypted), and should not leak into data PEB area.
 *
 * \expect Volume name bytes not found in data PEB area.
 */
ZTEST(ubi_secure_forensic, volume_name_absent_in_data_area)
{
	const struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'S', 'E', 'C', 'R', 'E', 'T' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_128, ARRAY_SIZE(array_128)));

	zassert_ok(ubi_device_deinit(ubi));

	/* The name "/SECRET" should not appear in data PEB area. */
	const uint8_t name_pattern[] = { '/', 'S', 'E', 'C', 'R', 'E', 'T' };

	zassert_false(flash_contains_pattern(name_pattern, sizeof(name_pattern)),
		      "Volume name found in data PEB area — metadata leakage");
}

/**
 * \brief Verify that test root key material does not appear on flash.
 *
 * \details Scenario: After secure format and write, scan the entire data PEB area
 *          for the raw 16-byte test root key material. Key material must
 *          never be written to flash.
 *
 * \expect Raw key material not found on flash.
 */
ZTEST(ubi_secure_forensic, key_material_absent_on_flash)
{
	const struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'f', 'r', 'n', '3' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_128, ARRAY_SIZE(array_128)));

	zassert_ok(ubi_device_deinit(ubi));

	/* Root key material must never appear anywhere on flash. */
	zassert_false(flash_contains_pattern(UBI_TEST_ROOT_KEY_MATERIAL,
					     sizeof(UBI_TEST_ROOT_KEY_MATERIAL)),
		      "Root key material found on flash — key leakage");
}

/**
 * \brief Verify that plaintext data is absent after overwrite and erase.
 *
 * \details Scenario: Write data, overwrite with different data, unmap, erase dirty
 *          PEBs, then scan for both the old and new plaintext. Neither
 *          should be present: old data was overwritten + erased, new data
 *          was encrypted.
 *
 * \expect Neither old nor new plaintext found on flash after erase cycle.
 */
ZTEST(ubi_secure_forensic, plaintext_absent_after_overwrite_and_erase)
{
	const struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'f', 'r', 'n', '4' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
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
 * \details Scenario: Format a plain device (no encryption), write known data, then
 *          scan for it. This validates that the forensic scan itself works —
 *          plaintext written without encryption MUST be found.
 *
 * \expect array_128 IS found on flash (plain mode does not encrypt).
 */
ZTEST(ubi_secure_forensic, plain_backend_plaintext_is_detectable)
{
	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'p', 'l', 'n', '1' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	/* Init as PLAIN (no crypto config). */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	g_ubi = ubi;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_128, ARRAY_SIZE(array_128)));

	zassert_ok(ubi_device_deinit(ubi));

	/* On plain backend, plaintext MUST be on flash — validates the scanner. */
	zassert_true(flash_contains_pattern(array_128, ARRAY_SIZE(array_128)),
		     "Forensic scan failed to detect plaintext on plain backend — scanner bug");
}

/**
 * \brief LEB write tail-padding bytes equal the flash erased value.
 *
 * \details Scenario: Write a small payload whose ciphertext+tag is shorter than one
 *          flash write block, deinit, then scan all data PEBs for the
 *          secure prefix magic ('UBIS' = 0x55424953 LE).  For each
 *          matching PEB, confirm the bytes between [tag-end, write-block-end]
 *          equal the flash erased value (rather than the previous 0x00).
 *
 * \expect At least one LEB found, and tail bytes equal erased_val.
 */
ZTEST(ubi_secure_forensic, leb_tail_padding_uses_erased_value)
{
	/* Layout constants — kept private from public test API; documented here.
	 * LEB region starts at peb_offset + 160 (UBI_SECURE_LEB_OFFSET).
	 * Prefix is 32 bytes, tag is 16 bytes appended after ciphertext.
	 * Magic is the first 4 bytes of the prefix, big-endian (sys_put_be32). */
	const size_t leb_offset_in_peb = 160U;
	const size_t prefix_size = 32U;
	const size_t tag_size = 16U;
	const uint8_t magic_be[4] = { 'U', 'B', 'I', 'S' }; /* 0x55424953 BE */

	/* Skip if write block size doesn't introduce padding for our payload. */
	const size_t payload_len = 5U;
	const size_t ct_tag_size = payload_len + tag_size;
	const size_t ct_write_size =
		((ct_tag_size + flash.write_block_size - 1) / flash.write_block_size) *
		flash.write_block_size;

	if (ct_write_size <= ct_tag_size) {
		ztest_test_skip();
		return;
	}

	uint8_t erased_val = 0;

	zassert_ok(ubi_test_get_erased_val(&flash, &erased_val));

	const struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 't', 'a', 'i', 'l' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	struct ubi_device *ubi = NULL;
	int vol_id = -1;
	const uint8_t small_payload[5] = { 0x01, 0x02, 0x03, 0x04, 0x05 };

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, small_payload, sizeof(small_payload)));
	zassert_ok(ubi_device_deinit(ubi));

	/* Scan data PEBs for the secure LEB prefix and verify tail padding. */
	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t start_offset = 2U * flash.erase_block_size;
	size_t leb_found = 0;
	uint8_t prefix_buf[4] = { 0 };
	uint8_t tail_buf[16] = { 0 };
	const size_t tail_len = ct_write_size - ct_tag_size;

	zassert_true(tail_len <= sizeof(tail_buf), "tail_buf too small");

	for (size_t off = start_offset; off < UBI_PARTITION_SIZE; off += flash.erase_block_size) {
		const size_t leb_off = off + leb_offset_in_peb;

		zassert_ok(flash_area_read(fa, leb_off, prefix_buf, sizeof(prefix_buf)));
		if (memcmp(prefix_buf, magic_be, sizeof(magic_be)) != 0) {
			continue;
		}

		const size_t tail_off = leb_off + prefix_size + ct_tag_size;

		zassert_ok(flash_area_read(fa, tail_off, tail_buf, tail_len));
		for (size_t i = 0; i < tail_len; i++) {
			zassert_equal(tail_buf[i], erased_val,
				      "PEB at off %zu: tail byte %zu = 0x%02x, want 0x%02x", off, i,
				      tail_buf[i], erased_val);
		}
		leb_found++;
	}

	flash_area_close(fa);
	zassert_true(leb_found >= 1, "no LEB prefix found on flash");
}
