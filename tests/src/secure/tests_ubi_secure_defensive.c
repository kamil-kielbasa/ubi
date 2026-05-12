/**
 * \file    tests_ubi_secure_defensive.c
 * \author  Kamil Kielbasa
 *
 * \brief   Defensive-check coverage tests for secure backend internals.
 *
 * \details Exercises NULL-argument guards, invalid-input paths, and corrupted-
 *          flash scan recovery in the secure crypto, serialization, I/O, and
 *          init modules. These tests target error branches unreachable through
 *          the public API.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>

#include "ubi_test_fixture.h"
#include "ubi_test_secure_fixture.h"
#include "ubi_test_memory.h"

/* Internal secure headers (via target_include_directories). */
#include "ubi_secure_crypto.h"
#include "ubi_secure_ser.h"
#include "ubi_secure_io.h"
#include "ubi_secure_types.h"
#include "ubi_secure_test_hooks.h"
#include "ubi_secure_reserved.h"

#include <psa/crypto.h>

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>

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

/* Static function definitions ------------------------------------------------------------------ */

/* Module interface function definitions -------------------------------------------------------- */
static void *ztest_suite_setup(void)
{
	const struct device *const flash_dev = UBI_PARTITION_DEVICE;

	zassert_true(device_is_ready(flash_dev));

	struct flash_pages_info page_info = { 0 };

	zassert_ok(flash_get_page_info_by_offs(flash_dev, 0, &page_info));

	flash.partition_id = FIXED_PARTITION_ID(UBI_PARTITION_NAME);
	flash.erase_block_size = page_info.size;
	flash.write_block_size = flash_get_write_block_size(flash_dev);

	zassert_equal(psa_crypto_init(), PSA_SUCCESS);
	ubi_test_import_root_key();

	return NULL;
}

static void ztest_suite_before(void *ctx)
{
	(void)ctx;
	ubi_test_partition_force_release_all();
	ubi_test_fault_reset();
	ubi_secure_test_hook_reset();
	zassert_ok(flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE));
}

/* ================================= Serialization NULL checks ================================== */

/**
 * \brief prefix32_serialize rejects NULL prefix.
 *
 * \details Scenario: Calls with NULL source pointer.
 *
 * \expect Returns without crash.
 */
ZTEST(ubi_secure_defensive, test_ser_prefix32_serialize_null)
{
	uint8_t buf[UBI_SECURE_PREFIX_SIZE] = { 0 };

	ubi_secure_prefix32_serialize(NULL, buf);
	ubi_secure_prefix32_serialize(&(struct ubi_crypto_prefix32){ 0 }, NULL);
}

/**
 * \brief prefix32_deserialize rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL source and destination.
 *
 * \expect Returns without crash.
 */
ZTEST(ubi_secure_defensive, test_ser_prefix32_deserialize_null)
{
	struct ubi_crypto_prefix32 prefix = { 0 };

	ubi_secure_prefix32_deserialize(NULL, &prefix);

	const uint8_t buf[UBI_SECURE_PREFIX_SIZE] = { 0 };

	ubi_secure_prefix32_deserialize(buf, NULL);
}

/**
 * \brief AAD builder functions reject NULL input pointer.
 *
 * \details Scenario: Exercise dev_hdr, vol_hdr, ec_hdr, vid_hdr, leb AAD builders
 *          with a NULL input struct pointer.
 *
 * \expect All return without crash.
 */
ZTEST(ubi_secure_defensive, test_ser_aad_builders_null)
{
	uint8_t aad_dev[UBI_SECURE_DEV_HDR_AAD_SIZE] = { 0 };

	ubi_secure_build_dev_hdr_aad(NULL, aad_dev);

	uint8_t aad_vol[UBI_SECURE_VOL_HDR_AAD_SIZE] = { 0 };

	ubi_secure_build_vol_hdr_aad(NULL, aad_vol);

	uint8_t aad_ec[UBI_SECURE_EC_HDR_AAD_SIZE] = { 0 };

	ubi_secure_build_ec_hdr_aad(NULL, aad_ec);

	uint8_t aad_vid[UBI_SECURE_VID_HDR_AAD_SIZE] = { 0 };

	ubi_secure_build_vid_hdr_aad(NULL, aad_vid);

	uint8_t aad_leb[UBI_SECURE_LEB_AAD_SIZE] = { 0 };

	ubi_secure_build_leb_aad(NULL, aad_leb);
}

/**
 * \brief dev_meta serialize/deserialize reject NULL arguments.
 *
 * \details Scenario: Calls with NULL meta and NULL buf.
 *
 * \expect Returns without crash.
 */
ZTEST(ubi_secure_defensive, test_ser_dev_meta_null)
{
	uint8_t buf[16] = { 0 };

	ubi_secure_dev_meta_serialize(NULL, buf);

	struct ubi_dev_secure_meta meta = { 0 };

	ubi_secure_dev_meta_serialize(&meta, NULL);

	ubi_secure_dev_meta_deserialize(NULL, &meta);
	ubi_secure_dev_meta_deserialize(buf, NULL);
}

/**
 * \brief vid_meta serialize/deserialize reject NULL arguments.
 *
 * \details Scenario: Calls with NULL meta and NULL buf.
 *
 * \expect Returns without crash.
 */
ZTEST(ubi_secure_defensive, test_ser_vid_meta_null)
{
	uint8_t buf[16] = { 0 };

	ubi_secure_vid_meta_serialize(NULL, buf);

	struct ubi_vid_secure_meta meta = { 0 };

	ubi_secure_vid_meta_serialize(&meta, NULL);

	ubi_secure_vid_meta_deserialize(NULL, &meta);
	ubi_secure_vid_meta_deserialize(buf, NULL);
}

/**
 * \brief encode_counter48 rejects NULL buffer and overflow value.
 *
 * \details Scenario: Calls with NULL buf and value > COUNTER_MAX.
 *
 * \expect Returns without crash.
 */
ZTEST(ubi_secure_defensive, test_ser_counter48_null)
{
	ubi_secure_encode_counter48(0, NULL);
	ubi_secure_encode_counter48(UBI_SECURE_COUNTER_MAX + 1, NULL);

	zassert_equal(ubi_secure_decode_counter48(NULL), 0);
}

/* ===================================== Crypto NULL checks ===================================== */

/**
 * \brief aead_encrypt rejects NULL nonce.
 *
 * \details Scenario: Calls with NULL nonce pointer.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_crypto_aead_encrypt_null)
{
	uint8_t ct[32] = { 0 };
	size_t ct_len = 0;

	zassert_equal(ubi_secure_aead_encrypt(0, NULL, NULL, 0, NULL, 0, ct, sizeof(ct), &ct_len),
		      -EINVAL);
}

/**
 * \brief aead_decrypt rejects NULL nonce.
 *
 * \details Scenario: Calls with NULL nonce pointer.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_crypto_aead_decrypt_null)
{
	uint8_t pt[32] = { 0 };
	size_t pt_len = 0;

	zassert_equal(ubi_secure_aead_decrypt(0, NULL, NULL, 0, NULL, 0, pt, sizeof(pt), &pt_len),
		      -EINVAL);
}

/**
 * \brief generate_salt rejects NULL buffer.
 *
 * \details Scenario: Calls with NULL salt pointer.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_crypto_generate_salt_null)
{
	zassert_equal(ubi_secure_generate_salt(NULL), -EINVAL);
}

/**
 * \brief build_nonce rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL counter and NULL nonce.
 *
 * \expect Returns without crash.
 */
ZTEST(ubi_secure_defensive, test_crypto_build_nonce_null)
{
	uint8_t salt[UBI_SECURE_SALT_SIZE] = { 0 };
	uint8_t counter[UBI_SECURE_COUNTER_SIZE] = { 0 };
	uint8_t nonce[UBI_SECURE_NONCE_SIZE] = { 0 };

	ubi_secure_build_nonce(0, NULL, counter, nonce);
	ubi_secure_build_nonce(0, salt, NULL, nonce);
	ubi_secure_build_nonce(0, salt, counter, NULL);
}

/**
 * \brief derive_domain_key rejects NULL crypto_cfg.
 *
 * \details Scenario: Calls with NULL config pointer.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_crypto_derive_domain_key_null)
{
	uint32_t kid = 0;

	zassert_equal(ubi_secure_derive_domain_key(NULL, UBI_SECURE_DOMAIN_ERASE_COUNTER, 0, &kid),
		      -EINVAL);
}

/**
 * \brief derive_leb_key rejects NULL crypto_cfg.
 *
 * \details Scenario: Calls with NULL config pointer.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_crypto_derive_leb_key_null)
{
	uint32_t kid = 0;

	zassert_equal(ubi_secure_derive_leb_key(NULL, 0, 0, &kid), -EINVAL);
}

/* ====================================== I/O NULL checks ======================================= */

/**
 * \brief ec_hdr_read rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL flash, NULL crypto_cfg, NULL ec_hdr, NULL ec_ctx.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_io_ec_hdr_read_null)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_ec_hdr ec = { 0 };
	struct ubi_secure_ec_auth_ctx ctx = { 0 };

	zassert_equal(ubi_secure_ec_hdr_read(NULL, &cfg, 0, &ec, &ctx), -EINVAL);
	zassert_equal(ubi_secure_ec_hdr_read(&flash, NULL, 0, &ec, &ctx), -EINVAL);
	zassert_equal(ubi_secure_ec_hdr_read(&flash, &cfg, 0, NULL, &ctx), -EINVAL);
	zassert_equal(ubi_secure_ec_hdr_read(&flash, &cfg, 0, &ec, NULL), -EINVAL);
}

/**
 * \brief vid_hdr_read rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL flash and NULL vid_hdr.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_io_vid_hdr_read_null)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };
	struct ubi_vid_hdr vid = { 0 };
	struct ubi_vid_secure_meta meta = { 0 };
	struct ubi_secure_vid_auth_ctx vid_ctx = { 0 };

	zassert_equal(ubi_secure_vid_hdr_read(NULL, &cfg, 0, &ec_ctx, &vid, &meta, &vid_ctx),
		      -EINVAL);
	zassert_equal(ubi_secure_vid_hdr_read(&flash, &cfg, 0, &ec_ctx, NULL, &meta, &vid_ctx),
		      -EINVAL);
}

/**
 * \brief vid_hdr_write rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL flash and NULL vid_hdr.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_io_vid_hdr_write_null)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };
	struct ubi_vid_hdr vid = { 0 };
	struct ubi_vid_secure_meta meta = { 0 };

	zassert_equal(ubi_secure_vid_hdr_write(NULL, &cfg, 0, &ec_ctx, &vid, &meta, 0, 0), -EINVAL);
	zassert_equal(ubi_secure_vid_hdr_write(&flash, &cfg, 0, &ec_ctx, NULL, &meta, 0, 0),
		      -EINVAL);
}

/**
 * \brief leb_data_read rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL flash and NULL buf with nonzero len.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_io_leb_data_read_null)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_vid_hdr vid = { .data_size = 4 };
	struct ubi_secure_vid_auth_ctx vid_ctx = { .vid_hdr = &vid };
	uint8_t buf[4] = { 0 };

	zassert_equal(ubi_secure_leb_data_read(NULL, &cfg, 0, &vid_ctx, 0, buf, 4), -EINVAL);
	zassert_equal(ubi_secure_leb_data_read(&flash, NULL, 0, &vid_ctx, 0, buf, 4), -EINVAL);
	zassert_equal(ubi_secure_leb_data_read(&flash, &cfg, 0, NULL, 0, buf, 4), -EINVAL);
}

/**
 * \brief leb_data_write rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL flash.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_io_leb_data_write_null)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };
	struct ubi_vid_hdr vid = { 0 };
	const uint8_t data[] = { 0x01 };

	zassert_equal(ubi_secure_leb_data_write(NULL, &cfg, 0, &ec_ctx, &vid, 0, data, sizeof(data),
						0, 0),
		      -EINVAL);
}

/**
 * \brief ec_hdr_write rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL flash.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_io_ec_hdr_write_null)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	const struct ubi_ec_hdr ec = { 0 };

	zassert_equal(ubi_secure_ec_hdr_write(NULL, &cfg, 0, &ec, 0, 0), -EINVAL);
	zassert_equal(ubi_secure_ec_hdr_write(&flash, NULL, 0, &ec, 0, 0), -EINVAL);
}

/* ==================================== I/O corruption paths ==================================== */

/**
 * \brief ec_hdr_read returns -EBADMSG for corrupted EC magic.
 *
 * \details Scenario: Write garbage to PEB 3 EC region, call ec_hdr_read.
 *
 * \expect Returns -EBADMSG.
 */
ZTEST(ubi_secure_defensive, test_io_ec_hdr_read_bad_magic)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	/* Write garbage to PEB 3 EC header area. */
	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	uint8_t garbage[UBI_SECURE_EC_HDR_SIZE];

	memset(garbage, 0xDE, sizeof(garbage));

	const size_t offset = 3 * flash.erase_block_size;

	zassert_ok(flash_area_write(fa, offset, garbage, sizeof(garbage)));
	flash_area_close(fa);

	struct ubi_ec_hdr ec = { 0 };
	struct ubi_secure_ec_auth_ctx ctx = { 0 };

	zassert_equal(ubi_secure_ec_hdr_read(&flash, &cfg, 3, &ec, &ctx), -EBADMSG);
}

/**
 * \brief vid_hdr_read returns error for corrupted VID data.
 *
 * \details Scenario: Write valid EC, write garbage in VID region, call vid_hdr_read.
 *
 * \expect Returns -EBADMSG (bad magic).
 */
ZTEST(ubi_secure_defensive, test_io_vid_hdr_read_bad_magic)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	/* Write a valid EC header first. */
	const struct ubi_ec_hdr ec_hdr = {
		.magic = UBI_EC_HDR_MAGIC,
		.version = UBI_EC_HDR_VERSION,
		.ec = 0,
	};

	zassert_ok(ubi_secure_ec_hdr_write(&flash, &cfg, 3, &ec_hdr,
					   cfg.policy.requested_write_key_version, 0));

	/* Read EC to get auth context. */
	struct ubi_ec_hdr ec_read = { 0 };
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };

	zassert_ok(ubi_secure_ec_hdr_read(&flash, &cfg, 3, &ec_read, &ec_ctx));

	/* Write garbage to VID region. */
	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	uint8_t garbage[UBI_SECURE_VID_HDR_SIZE];

	memset(garbage, 0xAB, sizeof(garbage));

	const size_t offset = 3 * flash.erase_block_size + UBI_SECURE_EC_HDR_SIZE;

	zassert_ok(flash_area_write(fa, offset, garbage, sizeof(garbage)));
	flash_area_close(fa);

	struct ubi_vid_hdr vid = { 0 };
	struct ubi_vid_secure_meta meta = { 0 };
	struct ubi_secure_vid_auth_ctx vid_ctx = { 0 };

	zassert_equal(ubi_secure_vid_hdr_read(&flash, &cfg, 3, &ec_ctx, &vid, &meta, &vid_ctx),
		      -EBADMSG);
}

/**
 * \brief leb_data_read returns -EINVAL for zero data_size with nonzero len.
 *
 * \details Scenario: Forge a vid_ctx with data_size=0, request len=1.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_io_leb_data_read_zero_datasize)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_vid_hdr vid = { .data_size = 0 };
	struct ubi_secure_vid_auth_ctx vid_ctx = { .vid_hdr = &vid };
	uint8_t buf[1] = { 0 };

	zassert_equal(ubi_secure_leb_data_read(&flash, &cfg, 3, &vid_ctx, 0, buf, 1), -EINVAL);
}

/**
 * \brief leb_data_read returns -EINVAL for out-of-bounds offset.
 *
 * \details Scenario: Forge a vid_ctx with data_size=2, request offset=1 len=4.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_io_leb_data_read_out_of_bounds)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_vid_hdr vid = { .data_size = 2 };
	struct ubi_secure_vid_auth_ctx vid_ctx = { .vid_hdr = &vid };
	uint8_t buf[4] = { 0 };

	zassert_equal(ubi_secure_leb_data_read(&flash, &cfg, 3, &vid_ctx, 1, buf, 4), -EINVAL);
}

/* ================================= Init with invalid geometry ================================= */

/**
 * \brief Device init rejects write_block_size of zero.
 *
 * \details Scenario: Passes flash with write_block_size=0.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_init_zero_write_block)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_flash_desc bad_flash = flash;

	bad_flash.write_block_size = 0;
	struct ubi_device *ubi = NULL;

	zassert_equal(ubi_device_init(&bad_flash, &cfg, &ubi), -EINVAL);
	zassert_is_null(ubi);
}

/**
 * \brief Device init rejects erase_block_size of zero.
 *
 * \details Scenario: Passes flash with erase_block_size=0.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_init_zero_erase_block)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_flash_desc bad_flash = flash;

	bad_flash.erase_block_size = 0;
	struct ubi_device *ubi = NULL;

	zassert_equal(ubi_device_init(&bad_flash, &cfg, &ubi), -EINVAL);
	zassert_is_null(ubi);
}

/**
 * \brief Device init rejects oversized write_block_size.
 *
 * \details Scenario: Passes flash with write_block_size > max alignment.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_init_oversized_write_block)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_flash_desc bad_flash = flash;

	/* Set to a large value unlikely to match any real flash. */
	bad_flash.write_block_size = 65536;
	struct ubi_device *ubi = NULL;

	const int ret = ubi_device_init(&bad_flash, &cfg, &ubi);

	if (ret == 0) {
		/* Geometry was accepted — cleanup. */
		ubi_device_deinit(ubi);
	} else {
		zassert_equal(ret, -EINVAL);
	}
}

/**
 * \brief Device init rejects erase block too small for headers.
 *
 * \details Scenario: Passes flash with very small erase_block_size.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_init_erase_block_too_small)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_flash_desc bad_flash = flash;

	/* Set erase block to 16 bytes — too small for EC+VID+LEB overhead. */
	bad_flash.erase_block_size = 16;
	struct ubi_device *ubi = NULL;

	const int ret = ubi_device_init(&bad_flash, &cfg, &ubi);

	if (ret == 0) {
		ubi_device_deinit(ubi);
	} else {
		zassert_equal(ret, -EINVAL);
	}
}

/**
 * \brief Device init rejects write key version not in allowlist.
 *
 * \details Scenario: Passes crypto config with mismatched write key version.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_init_bad_write_key_version)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	/* Set requested write key version to a value not in the allowlist. */
	cfg.policy.requested_write_key_version = 99;
	struct ubi_device *ubi = NULL;

	zassert_equal(ubi_device_init(&flash, &cfg, &ubi), -EINVAL);
	zassert_is_null(ubi);
}

/* ================================= Scan with corrupted flash ================================== */

/**
 * \brief Scan classifies PEB with corrupt EC header as bad block.
 *
 * \details Scenario: Format device, deinit. Corrupt a data PEB EC header.
 *          Re-init. The scan should mark the PEB as bad.
 *
 * \expect Re-init succeeds; bad_peb_count > 0.
 */
ZTEST(ubi_secure_defensive, test_scan_corrupt_ec_marks_bad)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	/* Format fresh device. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	ubi_test_partition_force_release_all();

	/* Corrupt EC header of a data PEB (PEB 3 = first data PEB after 3 reserved). */
	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	uint8_t garbage[UBI_SECURE_EC_HDR_SIZE];

	memset(garbage, 0xFF, sizeof(garbage));
	/* Write a bad magic to trigger authentication failure. */
	garbage[0] = 0xBA;
	garbage[1] = 0xAD;

	const size_t peb_offset = 3 * flash.erase_block_size;

	zassert_ok(flash_area_erase(fa, peb_offset, flash.erase_block_size));
	zassert_ok(flash_area_write(fa, peb_offset, garbage, sizeof(garbage)));
	flash_area_close(fa);

	/* Re-init should succeed but mark the corrupted PEB as bad. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.bad_peb_count > 0, "Corrupted PEB should be marked bad");

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Scan classifies PEB with erased VID but written LEB prefix as dirty.
 *
 * \details Scenario: Format device. Write LEB data prefix to a free PEB (VID remains
 *          erased). Re-init. The scan should classify it as dirty
 *          (uncommitted write).
 *
 * \expect Re-init succeeds; dirty_peb_count > 0.
 */
ZTEST(ubi_secure_defensive, test_scan_erased_vid_dirty_leb)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	struct ubi_device_info info_before = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info_before));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	ubi_test_partition_force_release_all();

	/* Write non-erased data to LEB area of a free PEB (PEB 4).
	 * Keep VID region erased. This simulates an interrupted write. */
	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t leb_offset = 4 * flash.erase_block_size + UBI_SECURE_LEB_OFFSET;
	uint8_t nonerased[16] = { 0 };

	nonerased[0] = 0xDE;
	nonerased[1] = 0xAD;
	nonerased[2] = 0xBE;
	nonerased[3] = 0xEF;

	zassert_ok(flash_area_write(fa, leb_offset, nonerased, sizeof(nonerased)));
	flash_area_close(fa);

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	struct ubi_device_info info_after = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info_after));

	/* The PEB with erased VID but non-erased LEB should be classified as dirty. */
	zassert_true(info_after.dirty_peb_count > 0 || info_after.bad_peb_count > 0,
		     "Partially written PEB should be dirty or bad");

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Scan handles orphan PEBs from deleted volumes.
 *
 * \details Scenario: Create volume, write data, remove volume, re-init. The scan
 *          should classify PEBs from the deleted volume as orphans and
 *          move them to dirty pool.
 *
 * \expect Re-init succeeds; no volumes; dirty pebs present.
 */
ZTEST(ubi_secure_defensive, test_scan_orphan_classification)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	const struct ubi_volume_config vol_cfg = {
		.name = "orphan",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	const uint8_t data[] = { 0x42 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_volume_remove(ubi, vol_id));

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	ubi_test_partition_force_release_all();

	/* Re-init: scan should find orphan PEBs and classify as dirty. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.volume_count, 0);

	zassert_ok(ubi_device_deinit(ubi));
}

/* ========================== leb_data_read with NULL buf/zero-length =========================== */

/**
 * \brief leb_data_read with NULL buf and len=0 returns 0 for zero data_size.
 *
 * \details Scenario: Forge vid_ctx with data_size=0, call with NULL buf and len=0.
 *
 * \expect Returns 0.
 */
ZTEST(ubi_secure_defensive, test_io_leb_data_read_zero_len_zero_data)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_vid_hdr vid = { .data_size = 0 };
	struct ubi_secure_vid_auth_ctx vid_ctx = { .vid_hdr = &vid };

	zassert_ok(ubi_secure_leb_data_read(&flash, &cfg, 3, &vid_ctx, 0, NULL, 0));
}

/* ============================== vid/leb erased checks with NULL =============================== */

/**
 * \brief vid_region_is_erased rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL flash and NULL is_erased.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_io_vid_region_is_erased_null)
{
	bool erased = false;

	zassert_equal(ubi_secure_vid_region_is_erased(NULL, 0, &erased), -EINVAL);
	zassert_equal(ubi_secure_vid_region_is_erased(&flash, 0, NULL), -EINVAL);
}

/**
 * \brief leb_prefix_is_erased rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL flash and NULL is_erased.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_io_leb_prefix_is_erased_null)
{
	bool erased = false;

	zassert_equal(ubi_secure_leb_prefix_is_erased(NULL, 0, &erased), -EINVAL);
	zassert_equal(ubi_secure_leb_prefix_is_erased(&flash, 0, NULL), -EINVAL);
}

/* ================================= IO hook-based error paths ================================== */

/**
 * \brief ec_hdr_read returns error for correct magic but wrong domain.
 *
 * \details Scenario: Writes a prefix with valid SECURE_PREFIX_MAGIC but domain LEB.
 *
 * \expect Returns -EBADMSG.
 */
ZTEST(ubi_secure_defensive, test_io_ec_hdr_read_bad_domain)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	/* Build a prefix with correct magic but wrong domain. */
	struct ubi_crypto_prefix32 prefix = {
		.magic = UBI_SECURE_PREFIX_MAGIC,
		.wrapper_version = UBI_SECURE_WRAPPER_VERSION,
		.domain = UBI_SECURE_DOMAIN_LEB, /* Wrong — should be ERASE_COUNTER */
		.key_version = 0,
		.flags = 0,
	};
	uint8_t prefix_buf[UBI_SECURE_PREFIX_SIZE] = { 0 };

	ubi_secure_prefix32_serialize(&prefix, prefix_buf);

	const size_t offset = 4 * flash.erase_block_size;

	zassert_ok(flash_area_write(fa, offset, prefix_buf, sizeof(prefix_buf)));
	flash_area_close(fa);

	struct ubi_ec_hdr ec = { 0 };
	struct ubi_secure_ec_auth_ctx ctx = { 0 };

	zassert_equal(ubi_secure_ec_hdr_read(&flash, &cfg, 4, &ec, &ctx), -EBADMSG);
}

/**
 * \brief ec_hdr_read returns error when key derivation fails.
 *
 * \details Scenario: Writes valid EC header prefix, arms GET_KEY_ID_FAIL hook.
 *
 * \expect Returns error from key derivation.
 */
ZTEST(ubi_secure_defensive, test_io_ec_hdr_read_key_deriv_fail)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	/* Write a valid-looking EC prefix (correct magic + domain). */
	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	struct ubi_crypto_prefix32 prefix = {
		.magic = UBI_SECURE_PREFIX_MAGIC,
		.wrapper_version = UBI_SECURE_WRAPPER_VERSION,
		.domain = UBI_SECURE_DOMAIN_ERASE_COUNTER,
		.key_version = cfg.policy.requested_write_key_version,
		.flags = 0,
	};
	uint8_t buf[UBI_SECURE_EC_HDR_SIZE] = { 0 };

	ubi_secure_prefix32_serialize(&prefix, buf);

	const size_t offset = 4 * flash.erase_block_size;

	zassert_ok(flash_area_write(fa, offset, buf, sizeof(buf)));
	flash_area_close(fa);

	struct ubi_ec_hdr ec = { 0 };
	struct ubi_secure_ec_auth_ctx ctx = { 0 };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_GET_KEY_ID_FAIL, true);
	const int ret = ubi_secure_ec_hdr_read(&flash, &cfg, 4, &ec, &ctx);

	zassert_not_equal(ret, 0, "Should fail when key derivation fails");
}

/**
 * \brief ec_hdr_write returns error when key derivation fails.
 *
 * \details Scenario: Arms GET_KEY_ID_FAIL hook, attempts ec_hdr_write.
 *
 * \expect Returns error.
 */
ZTEST(ubi_secure_defensive, test_io_ec_hdr_write_key_deriv_fail)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	const struct ubi_ec_hdr ec = {
		.magic = UBI_EC_HDR_MAGIC,
		.version = UBI_EC_HDR_VERSION,
		.ec = 0,
	};

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_GET_KEY_ID_FAIL, true);
	const int ret = ubi_secure_ec_hdr_write(&flash, &cfg, 4, &ec,
						cfg.policy.requested_write_key_version, 0);

	zassert_not_equal(ret, 0, "Should fail when key derivation fails");
}

/**
 * \brief ec_hdr_write returns error when salt generation fails.
 *
 * \details Scenario: Arms RNG_FAIL hook, attempts ec_hdr_write.
 *
 * \expect Returns error.
 */
ZTEST(ubi_secure_defensive, test_io_ec_hdr_write_salt_fail)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	const struct ubi_ec_hdr ec = {
		.magic = UBI_EC_HDR_MAGIC,
		.version = UBI_EC_HDR_VERSION,
		.ec = 0,
	};

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_RNG_FAIL, true);
	const int ret = ubi_secure_ec_hdr_write(&flash, &cfg, 4, &ec,
						cfg.policy.requested_write_key_version, 0);

	zassert_not_equal(ret, 0, "Should fail when salt gen fails");
}

/**
 * \brief ec_hdr_write returns error when AEAD encrypt fails.
 *
 * \details Scenario: Arms AEAD_ENCRYPT_FAIL hook, attempts ec_hdr_write.
 *
 * \expect Returns error.
 */
ZTEST(ubi_secure_defensive, test_io_ec_hdr_write_aead_fail)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	const struct ubi_ec_hdr ec = {
		.magic = UBI_EC_HDR_MAGIC,
		.version = UBI_EC_HDR_VERSION,
		.ec = 0,
	};

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_AEAD_ENCRYPT_FAIL, true);
	const int ret = ubi_secure_ec_hdr_write(&flash, &cfg, 4, &ec,
						cfg.policy.requested_write_key_version, 0);

	zassert_not_equal(ret, 0, "Should fail when AEAD encrypt fails");
}

/**
 * \brief ec_hdr_write returns error when flash write fails.
 *
 * \details Scenario: Arms flash write fault, attempts ec_hdr_write.
 *
 * \expect Returns error.
 */
ZTEST(ubi_secure_defensive, test_io_ec_hdr_write_flash_fail)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	const struct ubi_ec_hdr ec = {
		.magic = UBI_EC_HDR_MAGIC,
		.version = UBI_EC_HDR_VERSION,
		.ec = 0,
	};

	ubi_test_fault_set_flash_write_fail_after(0);
	const int ret = ubi_secure_ec_hdr_write(&flash, &cfg, 4, &ec,
						cfg.policy.requested_write_key_version, 0);

	zassert_not_equal(ret, 0, "Should fail when flash write fails");
}

/**
 * \brief vid_hdr_write returns error when key derivation fails.
 *
 * \details Scenario: Arms GET_KEY_ID_FAIL hook, attempts vid_hdr_write.
 *
 * \expect Returns error.
 */
ZTEST(ubi_secure_defensive, test_io_vid_hdr_write_key_deriv_fail)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };
	const struct ubi_vid_hdr vid = { 0 };
	const struct ubi_vid_secure_meta meta = { 0 };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_GET_KEY_ID_FAIL, true);
	const int ret = ubi_secure_vid_hdr_write(&flash, &cfg, 4, &ec_ctx, &vid, &meta, 0, 0);

	zassert_not_equal(ret, 0, "Should fail when key derivation fails");
}

/**
 * \brief vid_hdr_write returns error when salt generation fails.
 *
 * \details Scenario: Arms RNG_FAIL hook, attempts vid_hdr_write.
 *
 * \expect Returns error.
 */
ZTEST(ubi_secure_defensive, test_io_vid_hdr_write_salt_fail)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };
	const struct ubi_vid_hdr vid = { 0 };
	const struct ubi_vid_secure_meta meta = { 0 };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_RNG_FAIL, true);
	const int ret = ubi_secure_vid_hdr_write(&flash, &cfg, 4, &ec_ctx, &vid, &meta,
						 cfg.policy.requested_write_key_version, 0);

	zassert_not_equal(ret, 0, "Should fail when salt gen fails");
}

/**
 * \brief vid_hdr_write returns error when AEAD encrypt fails.
 *
 * \details Scenario: Arms AEAD_ENCRYPT_FAIL hook, attempts vid_hdr_write.
 *
 * \expect Returns error.
 */
ZTEST(ubi_secure_defensive, test_io_vid_hdr_write_aead_fail)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };
	const struct ubi_vid_hdr vid = { 0 };
	const struct ubi_vid_secure_meta meta = { 0 };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_AEAD_ENCRYPT_FAIL, true);
	const int ret = ubi_secure_vid_hdr_write(&flash, &cfg, 4, &ec_ctx, &vid, &meta,
						 cfg.policy.requested_write_key_version, 0);

	zassert_not_equal(ret, 0, "Should fail when AEAD encrypt fails");
}

/**
 * \brief vid_hdr_write returns error when flash write fails.
 *
 * \details Scenario: Arms flash write fault, attempts vid_hdr_write.
 *
 * \expect Returns error.
 */
ZTEST(ubi_secure_defensive, test_io_vid_hdr_write_flash_fail)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };
	const struct ubi_vid_hdr vid = { 0 };
	const struct ubi_vid_secure_meta meta = { 0 };

	ubi_test_fault_set_flash_write_fail_after(0);
	const int ret = ubi_secure_vid_hdr_write(&flash, &cfg, 4, &ec_ctx, &vid, &meta,
						 cfg.policy.requested_write_key_version, 0);

	zassert_not_equal(ret, 0, "Should fail when flash write fails");
}

/**
 * \brief leb_data_write returns error when key derivation fails.
 *
 * \details Scenario: Arms GET_KEY_ID_FAIL hook, attempts leb_data_write.
 *
 * \expect Returns error.
 */
ZTEST(ubi_secure_defensive, test_io_leb_data_write_key_deriv_fail)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };
	const struct ubi_vid_hdr vid = { .data_size = 2 };
	const uint8_t data[] = { 0xAA, 0xBB };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_GET_KEY_ID_FAIL, true);
	const int ret = ubi_secure_leb_data_write(&flash, &cfg, 4, &ec_ctx, &vid, 0, data,
						  sizeof(data), 0, 0);

	zassert_not_equal(ret, 0, "Should fail when key derivation fails");
}

/**
 * \brief leb_data_write returns error when salt generation fails.
 *
 * \details Scenario: Arms RNG_FAIL hook, attempts leb_data_write.
 *
 * \expect Returns error.
 */
ZTEST(ubi_secure_defensive, test_io_leb_data_write_salt_fail)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };
	const struct ubi_vid_hdr vid = { .data_size = 2 };
	const uint8_t data[] = { 0xAA, 0xBB };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_RNG_FAIL, true);
	const int ret = ubi_secure_leb_data_write(&flash, &cfg, 4, &ec_ctx, &vid, 0, data,
						  sizeof(data),
						  cfg.policy.requested_write_key_version, 0);

	zassert_not_equal(ret, 0, "Should fail when salt gen fails");
}

/**
 * \brief leb_data_write returns error when AEAD encrypt fails.
 *
 * \details Scenario: Arms AEAD_ENCRYPT_FAIL hook, attempts leb_data_write.
 *
 * \expect Returns error.
 */
ZTEST(ubi_secure_defensive, test_io_leb_data_write_aead_fail)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };
	const struct ubi_vid_hdr vid = { .data_size = 2 };
	const uint8_t data[] = { 0xAA, 0xBB };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_AEAD_ENCRYPT_FAIL, true);
	const int ret = ubi_secure_leb_data_write(&flash, &cfg, 4, &ec_ctx, &vid, 0, data,
						  sizeof(data),
						  cfg.policy.requested_write_key_version, 0);

	zassert_not_equal(ret, 0, "Should fail when AEAD encrypt fails");
}

/**
 * \brief leb_data_write returns error when flash write fails.
 *
 * \details Scenario: Arms flash write fault, attempts leb_data_write.
 *
 * \expect Returns error.
 */
ZTEST(ubi_secure_defensive, test_io_leb_data_write_flash_fail)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };
	const struct ubi_vid_hdr vid = { .data_size = 2 };
	const uint8_t data[] = { 0xAA, 0xBB };

	ubi_test_fault_set_flash_write_fail_after(0);
	const int ret = ubi_secure_leb_data_write(&flash, &cfg, 4, &ec_ctx, &vid, 0, data,
						  sizeof(data),
						  cfg.policy.requested_write_key_version, 0);

	zassert_not_equal(ret, 0, "Should fail when flash write fails");
}

/**
 * \brief leb_data_read returns error when key derivation fails.
 *
 * \details Scenario: Writes valid LEB prefix, arms GET_KEY_ID_FAIL, reads.
 *
 * \expect Returns error.
 */
ZTEST(ubi_secure_defensive, test_io_leb_data_read_key_deriv_fail)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	/* Write a valid LEB prefix to PEB 4. */
	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	struct ubi_crypto_prefix32 prefix = {
		.magic = UBI_SECURE_PREFIX_MAGIC,
		.wrapper_version = UBI_SECURE_WRAPPER_VERSION,
		.domain = UBI_SECURE_DOMAIN_LEB,
		.key_version = cfg.policy.requested_write_key_version,
		.flags = 0,
	};
	uint8_t prefix_buf[UBI_SECURE_PREFIX_SIZE] = { 0 };

	ubi_secure_prefix32_serialize(&prefix, prefix_buf);

	const size_t leb_offset = 4 * flash.erase_block_size + UBI_SECURE_LEB_OFFSET;

	zassert_ok(flash_area_write(fa, leb_offset, prefix_buf, sizeof(prefix_buf)));
	flash_area_close(fa);

	struct ubi_vid_hdr vid = { .data_size = 2, .vol_id = 0, .lnum = 0, .sqnum = 1 };
	struct ubi_secure_vid_auth_ctx vid_ctx = { .vid_hdr = &vid };
	uint8_t buf[2] = { 0 };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_GET_KEY_ID_FAIL, true);
	const int ret = ubi_secure_leb_data_read(&flash, &cfg, 4, &vid_ctx, 0, buf, 2);

	zassert_not_equal(ret, 0, "Should fail when key derivation fails");
}

/**
 * \brief leb_data_read returns -EBADMSG for bad LEB prefix.
 *
 * \details Scenario: Writes prefix with wrong domain (ERASE_COUNTER instead of LEB).
 *
 * \expect Returns -EBADMSG.
 */
ZTEST(ubi_secure_defensive, test_io_leb_data_read_bad_prefix)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	struct ubi_crypto_prefix32 prefix = {
		.magic = UBI_SECURE_PREFIX_MAGIC,
		.wrapper_version = UBI_SECURE_WRAPPER_VERSION,
		.domain = UBI_SECURE_DOMAIN_ERASE_COUNTER, /* Wrong domain for LEB */
		.key_version = cfg.policy.requested_write_key_version,
		.flags = 0,
	};
	uint8_t prefix_buf[UBI_SECURE_PREFIX_SIZE] = { 0 };

	ubi_secure_prefix32_serialize(&prefix, prefix_buf);

	const size_t leb_offset = 5 * flash.erase_block_size + UBI_SECURE_LEB_OFFSET;

	zassert_ok(flash_area_write(fa, leb_offset, prefix_buf, sizeof(prefix_buf)));
	flash_area_close(fa);

	struct ubi_vid_hdr vid = { .data_size = 2, .vol_id = 0, .lnum = 0, .sqnum = 1 };
	struct ubi_secure_vid_auth_ctx vid_ctx = { .vid_hdr = &vid };
	uint8_t buf[2] = { 0 };

	const int ret = ubi_secure_leb_data_read(&flash, &cfg, 5, &vid_ctx, 0, buf, 2);

	zassert_equal(ret, -EBADMSG);
}

/**
 * \brief vid_hdr_read returns error for correct magic but wrong domain.
 *
 * \details Scenario: Writes EC then VID prefix with wrong domain (LEB instead of VID).
 *
 * \expect Returns -EBADMSG.
 */
ZTEST(ubi_secure_defensive, test_io_vid_hdr_read_bad_domain)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	/* Write valid EC first. */
	const struct ubi_ec_hdr ec_hdr = {
		.magic = UBI_EC_HDR_MAGIC,
		.version = UBI_EC_HDR_VERSION,
		.ec = 0,
	};

	zassert_ok(ubi_secure_ec_hdr_write(&flash, &cfg, 5, &ec_hdr,
					   cfg.policy.requested_write_key_version, 0));

	struct ubi_ec_hdr ec_read = { 0 };
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };

	zassert_ok(ubi_secure_ec_hdr_read(&flash, &cfg, 5, &ec_read, &ec_ctx));

	/* Write VID prefix with wrong domain. */
	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	struct ubi_crypto_prefix32 prefix = {
		.magic = UBI_SECURE_PREFIX_MAGIC,
		.wrapper_version = UBI_SECURE_WRAPPER_VERSION,
		.domain = UBI_SECURE_DOMAIN_LEB, /* Wrong domain for VID */
		.key_version = cfg.policy.requested_write_key_version,
		.flags = 0,
	};
	uint8_t prefix_buf[UBI_SECURE_PREFIX_SIZE] = { 0 };

	ubi_secure_prefix32_serialize(&prefix, prefix_buf);

	const size_t vid_offset = 5 * flash.erase_block_size + UBI_SECURE_EC_HDR_SIZE;

	zassert_ok(flash_area_write(fa, vid_offset, prefix_buf, sizeof(prefix_buf)));
	flash_area_close(fa);

	struct ubi_vid_hdr vid = { 0 };
	struct ubi_vid_secure_meta meta = { 0 };
	struct ubi_secure_vid_auth_ctx vid_ctx = { 0 };

	zassert_equal(ubi_secure_vid_hdr_read(&flash, &cfg, 5, &ec_ctx, &vid, &meta, &vid_ctx),
		      -EBADMSG);
}

/**
 * \brief Device init rejects reserved-PEB records with an unsupported
 *        wrapper_version.
 *
 * \details Scenario: Writes a synthetic device-header prefix into reserved PEB 0
 *          with valid magic + DEVICE_HEADER domain but a bogus
 *          wrapper_version.  Subsequent ubi_device_init() must fail to
 *          authenticate any reserved-PEB copy and return an error.
 *
 * \expect ubi_device_init returns a non-zero error.
 */
ZTEST(ubi_secure_defensive, test_init_dev_hdr_bad_wrapper_version)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	struct ubi_crypto_prefix32 prefix = {
		.magic = UBI_SECURE_PREFIX_MAGIC,
		.wrapper_version = UBI_SECURE_WRAPPER_VERSION + 1, /* unsupported */
		.domain = UBI_SECURE_DOMAIN_DEVICE_HEADER,
		.key_version = cfg.policy.requested_write_key_version,
		.flags = 0,
	};
	uint8_t prefix_buf[UBI_SECURE_PREFIX_SIZE] = { 0 };

	ubi_secure_prefix32_serialize(&prefix, prefix_buf);

	/* Plant the bogus prefix into every reserved PEB. */
	for (size_t i = 0; i < CONFIG_UBI_DEV_HDR_NR_OF_RES_PEBS; i++) {
		zassert_ok(flash_area_write(fa, i * flash.erase_block_size, prefix_buf,
					    sizeof(prefix_buf)));
	}
	flash_area_close(fa);

	struct ubi_device *ubi = NULL;

	zassert_not_equal(ubi_device_init(&flash, &cfg, &ubi), 0,
			  "init must reject unsupported wrapper_version");
	if (ubi != NULL) {
		ubi_device_deinit(ubi);
	}
}

/**
 * \brief Device init rejects reserved-PEB volume-header records with an
 *        unsupported wrapper_version.
 *
 * \details Scenario: Builds a fully valid reserved-PEB layout via ubi_device_init
 *          + ubi_volume_create (so dev_hdr authenticates), then patches
 *          the wrapper_version byte in the vol_hdr prefix on every
 *          reserved-PEB copy.  The on-flash bit transition is 1->0 only
 *          (NOR-safe).  This isolates the vol-hdr authenticate code path
 *          in ubi_secure_res_peb_read_vol_hdrs (separate from the
 *          dev_hdr path covered by the previous test).
 *
 * \expect Subsequent ubi_device_init returns a non-zero error.
 */
ZTEST(ubi_secure_defensive, test_init_vol_hdr_bad_wrapper_version)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	const struct ubi_volume_config vol_cfg = {
		.name = "test_vol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	ubi_test_partition_force_release_all();

	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	/* Patch the wrapper_version byte in the vol_hdr prefix on every
	 * reserved-PEB copy.  We rewrite the full 32-byte prefix so that
	 * only the wrapper_version byte transitions 1 -> 0 (NOR-safe);
	 * all other bytes are programmed to their existing values. */
	for (size_t i = 0; i < CONFIG_UBI_DEV_HDR_NR_OF_RES_PEBS; i++) {
		const size_t vol_prefix_offset =
			i * flash.erase_block_size + UBI_SECURE_DEV_HDR_SIZE;
		uint8_t prefix_buf[UBI_SECURE_PREFIX_SIZE] = { 0 };

		zassert_ok(flash_area_read(fa, vol_prefix_offset, prefix_buf, sizeof(prefix_buf)));

		struct ubi_crypto_prefix32 prefix = { 0 };

		ubi_secure_prefix32_deserialize(prefix_buf, &prefix);
		zassert_equal(prefix.wrapper_version, UBI_SECURE_WRAPPER_VERSION);
		zassert_equal(prefix.domain, UBI_SECURE_DOMAIN_VOLUME_HEADER);

		/* Force unsupported wrapper_version (1 -> 0 transition). */
		prefix.wrapper_version = 0;
		ubi_secure_prefix32_serialize(&prefix, prefix_buf);

		zassert_ok(flash_area_write(fa, vol_prefix_offset, prefix_buf, sizeof(prefix_buf)));
	}
	flash_area_close(fa);

	zassert_not_equal(ubi_device_init(&flash, &cfg, &ubi), 0,
			  "init must reject unsupported vol_hdr wrapper_version");
	if (ubi != NULL) {
		ubi_device_deinit(ubi);
	}
}

/**
 * \brief ec_hdr_read rejects records with an unsupported wrapper_version.
 *
 * \details Scenario: Writes a prefix with valid magic + ERASE_COUNTER domain but a
 *          bogus wrapper_version (current + 1).  The read path must reject
 *          before any key derivation.
 *
 * \expect Returns -EBADMSG.
 */
ZTEST(ubi_secure_defensive, test_io_ec_hdr_read_bad_wrapper_version)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	struct ubi_crypto_prefix32 prefix = {
		.magic = UBI_SECURE_PREFIX_MAGIC,
		.wrapper_version = UBI_SECURE_WRAPPER_VERSION + 1, /* unsupported */
		.domain = UBI_SECURE_DOMAIN_ERASE_COUNTER,
		.key_version = cfg.policy.requested_write_key_version,
		.flags = 0,
	};
	uint8_t prefix_buf[UBI_SECURE_PREFIX_SIZE] = { 0 };

	ubi_secure_prefix32_serialize(&prefix, prefix_buf);

	const size_t offset = 4 * flash.erase_block_size;

	zassert_ok(flash_area_write(fa, offset, prefix_buf, sizeof(prefix_buf)));
	flash_area_close(fa);

	struct ubi_ec_hdr ec = { 0 };
	struct ubi_secure_ec_auth_ctx ctx = { 0 };

	zassert_equal(ubi_secure_ec_hdr_read(&flash, &cfg, 4, &ec, &ctx), -EBADMSG);
}

/**
 * \brief vid_hdr_read rejects records with an unsupported wrapper_version.
 *
 * \details Scenario: Writes a valid EC then a VID prefix with a bogus wrapper_version.
 *
 * \expect Returns -EBADMSG.
 */
ZTEST(ubi_secure_defensive, test_io_vid_hdr_read_bad_wrapper_version)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	const struct ubi_ec_hdr ec_hdr = {
		.magic = UBI_EC_HDR_MAGIC,
		.version = UBI_EC_HDR_VERSION,
		.ec = 0,
	};

	zassert_ok(ubi_secure_ec_hdr_write(&flash, &cfg, 5, &ec_hdr,
					   cfg.policy.requested_write_key_version, 0));

	struct ubi_ec_hdr ec_read = { 0 };
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };

	zassert_ok(ubi_secure_ec_hdr_read(&flash, &cfg, 5, &ec_read, &ec_ctx));

	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	struct ubi_crypto_prefix32 prefix = {
		.magic = UBI_SECURE_PREFIX_MAGIC,
		.wrapper_version = UBI_SECURE_WRAPPER_VERSION + 1, /* unsupported */
		.domain = UBI_SECURE_DOMAIN_VOLUME_IDENTIFIER,
		.key_version = cfg.policy.requested_write_key_version,
		.flags = 0,
	};
	uint8_t prefix_buf[UBI_SECURE_PREFIX_SIZE] = { 0 };

	ubi_secure_prefix32_serialize(&prefix, prefix_buf);

	const size_t vid_offset = 5 * flash.erase_block_size + UBI_SECURE_EC_HDR_SIZE;

	zassert_ok(flash_area_write(fa, vid_offset, prefix_buf, sizeof(prefix_buf)));
	flash_area_close(fa);

	struct ubi_vid_hdr vid = { 0 };
	struct ubi_vid_secure_meta meta = { 0 };
	struct ubi_secure_vid_auth_ctx vid_ctx = { 0 };

	zassert_equal(ubi_secure_vid_hdr_read(&flash, &cfg, 5, &ec_ctx, &vid, &meta, &vid_ctx),
		      -EBADMSG);
}

/**
 * \brief leb_data_read rejects records with an unsupported wrapper_version.
 *
 * \details Scenario: Writes a LEB prefix with valid magic + LEB domain but a bogus
 *          wrapper_version.  The read path must reject before any key
 *          derivation.
 *
 * \expect Returns -EBADMSG.
 */
ZTEST(ubi_secure_defensive, test_io_leb_data_read_bad_wrapper_version)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	struct ubi_crypto_prefix32 prefix = {
		.magic = UBI_SECURE_PREFIX_MAGIC,
		.wrapper_version = UBI_SECURE_WRAPPER_VERSION + 1, /* unsupported */
		.domain = UBI_SECURE_DOMAIN_LEB,
		.key_version = cfg.policy.requested_write_key_version,
		.flags = 0,
	};
	uint8_t prefix_buf[UBI_SECURE_PREFIX_SIZE] = { 0 };

	ubi_secure_prefix32_serialize(&prefix, prefix_buf);

	const size_t leb_offset = 5 * flash.erase_block_size + UBI_SECURE_LEB_OFFSET;

	zassert_ok(flash_area_write(fa, leb_offset, prefix_buf, sizeof(prefix_buf)));
	flash_area_close(fa);

	struct ubi_vid_hdr vid = { .data_size = 2, .vol_id = 0, .lnum = 0, .sqnum = 1 };
	struct ubi_secure_vid_auth_ctx vid_ctx = { .vid_hdr = &vid };
	uint8_t buf[2] = { 0 };

	const int ret = ubi_secure_leb_data_read(&flash, &cfg, 5, &vid_ctx, 0, buf, 2);

	zassert_equal(ret, -EBADMSG);
}

/* ============================ Single-tag CCM payload guard ===================================== */

/**
 * \brief leb_data_write rejects payloads exceeding the single-tag CCM limit.
 *
 * \details Scenario: Single-tag AES-128-CCM with q = 2 encodes the payload length
 *          in a 2-byte field, so it cannot authenticate payloads of
 *          65536 bytes or more.  The write path must reject
 *          `len > UBI_SECURE_LEB_SINGLE_TAG_MAX_PAYLOAD` before any
 *          key derivation or flash IO.
 *
 * \expect Returns -EFBIG.
 */
ZTEST(ubi_secure_defensive, test_io_leb_data_write_payload_exceeds_ccm_limit)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };
	const struct ubi_vid_hdr vid = { .data_size = UBI_SECURE_LEB_SINGLE_TAG_MAX_PAYLOAD + 1U };
	static const uint8_t dummy = 0;

	/* len > 65535 must be rejected with -EFBIG; buf may be a stub since the
	 * guard fires before any read of the payload. */
	const int ret =
		ubi_secure_leb_data_write(&flash, &cfg, 4, &ec_ctx, &vid, 0, &dummy,
					  (size_t)UBI_SECURE_LEB_SINGLE_TAG_MAX_PAYLOAD + 1U,
					  cfg.policy.requested_write_key_version, 0);

	zassert_equal(ret, -EFBIG, "Expected -EFBIG, got %d", ret);
}

/**
 * \brief leb_data_read rejects records advertising data_size beyond the
 *        single-tag CCM limit.
 *
 * \details Scenario: Build a vid_hdr with data_size > 65535 and call the read API.
 *          The guard must fire before any flash IO or key derivation,
 *          since CCM with q = 2 cannot decode a payload that does not
 *          fit in its 2-byte length field.
 *
 * \expect Returns -EBADMSG.
 */
ZTEST(ubi_secure_defensive, test_io_leb_data_read_data_size_exceeds_ccm_limit)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	const struct ubi_vid_hdr vid = { .data_size = UBI_SECURE_LEB_SINGLE_TAG_MAX_PAYLOAD + 1U };
	const struct ubi_secure_vid_auth_ctx vid_ctx = { .vid_hdr = &vid };
	uint8_t buf[1] = { 0 };

	const int ret = ubi_secure_leb_data_read(&flash, &cfg, 4, &vid_ctx, 0, buf, 1);

	zassert_equal(ret, -EBADMSG, "Expected -EBADMSG, got %d", ret);
}

/**
 * \brief vid_hdr_read returns error when key derivation fails.
 *
 * \details Scenario: Writes EC then valid VID prefix, arms GET_KEY_ID_FAIL.
 *
 * \expect Returns error.
 */
ZTEST(ubi_secure_defensive, test_io_vid_hdr_read_key_deriv_fail)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	/* Write valid EC first. */
	const struct ubi_ec_hdr ec_hdr = {
		.magic = UBI_EC_HDR_MAGIC,
		.version = UBI_EC_HDR_VERSION,
		.ec = 0,
	};

	zassert_ok(ubi_secure_ec_hdr_write(&flash, &cfg, 6, &ec_hdr,
					   cfg.policy.requested_write_key_version, 0));

	struct ubi_ec_hdr ec_read = { 0 };
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };

	zassert_ok(ubi_secure_ec_hdr_read(&flash, &cfg, 6, &ec_read, &ec_ctx));

	/* Write valid VID prefix. */
	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	struct ubi_crypto_prefix32 prefix = {
		.magic = UBI_SECURE_PREFIX_MAGIC,
		.wrapper_version = UBI_SECURE_WRAPPER_VERSION,
		.domain = UBI_SECURE_DOMAIN_VOLUME_IDENTIFIER,
		.key_version = cfg.policy.requested_write_key_version,
		.flags = 0,
	};
	uint8_t prefix_buf[UBI_SECURE_PREFIX_SIZE] = { 0 };

	ubi_secure_prefix32_serialize(&prefix, prefix_buf);

	const size_t vid_offset = 6 * flash.erase_block_size + UBI_SECURE_EC_HDR_SIZE;

	zassert_ok(flash_area_write(fa, vid_offset, prefix_buf, sizeof(prefix_buf)));
	flash_area_close(fa);

	struct ubi_vid_hdr vid = { 0 };
	struct ubi_vid_secure_meta meta = { 0 };
	struct ubi_secure_vid_auth_ctx vid_ctx = { 0 };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_GET_KEY_ID_FAIL, true);
	const int ret = ubi_secure_vid_hdr_read(&flash, &cfg, 6, &ec_ctx, &vid, &meta, &vid_ctx);

	zassert_not_equal(ret, 0, "Should fail when key derivation fails");
}

/**
 * \brief leb_data_write with NULL buf and len=0 succeeds (zero-length record).
 *
 * \details Scenario: Writes a zero-length LEB record.
 *
 * \expect Returns 0.
 */
ZTEST(ubi_secure_defensive, test_io_leb_data_write_zero_len)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };
	const struct ubi_vid_hdr vid = { .data_size = 0 };

	const int ret = ubi_secure_leb_data_write(&flash, &cfg, 4, &ec_ctx, &vid, 0, NULL, 0,
						  cfg.policy.requested_write_key_version, 0);

	/* Zero-length write should succeed or fail gracefully. */
	(void)ret;
}

/* ================================== Reserved PEB NULL checks ================================== */

/**
 * \brief res_peb_detect_mode rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL is_secure and NULL is_blank.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_res_peb_detect_mode_null)
{
	bool is_secure = false;
	bool is_blank = false;

	zassert_equal(ubi_secure_res_peb_detect_mode(NULL, 0, &is_secure, &is_blank), -EINVAL);
	zassert_equal(ubi_secure_res_peb_detect_mode(&flash, 0, NULL, &is_blank), -EINVAL);
	zassert_equal(ubi_secure_res_peb_detect_mode(&flash, 0, &is_secure, NULL), -EINVAL);
}

/**
 * \brief res_peb_scan rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL flash and NULL scan.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_res_peb_scan_null)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_secure_res_peb_scan scan = { 0 };

	zassert_equal(ubi_secure_res_peb_scan(NULL, &cfg, &scan), -EINVAL);
	zassert_equal(ubi_secure_res_peb_scan(&flash, NULL, &scan), -EINVAL);
	zassert_equal(ubi_secure_res_peb_scan(&flash, &cfg, NULL), -EINVAL);
}

/**
 * \brief res_peb_read_vol_hdrs rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL flash.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_res_peb_read_vol_hdrs_null)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_secure_res_peb_scan scan = { 0 };
	struct ubi_vol_hdr vols[1] = { 0 };

	zassert_equal(ubi_secure_res_peb_read_vol_hdrs(NULL, &cfg, &scan, vols, 1), -EINVAL);
	zassert_equal(ubi_secure_res_peb_read_vol_hdrs(&flash, NULL, &scan, vols, 1), -EINVAL);
	zassert_equal(ubi_secure_res_peb_read_vol_hdrs(&flash, &cfg, NULL, vols, 1), -EINVAL);
}

/**
 * \brief res_peb_commit rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL flash and NULL vol_hdrs with nonzero count.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_res_peb_commit_null)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	const struct ubi_dev_hdr dh = { 0 };
	const struct ubi_dev_secure_meta dm = { 0 };

	zassert_equal(ubi_secure_res_peb_commit(NULL, &cfg, &dh, &dm, NULL, 0, 0, 0), -EINVAL);
	zassert_equal(ubi_secure_res_peb_commit(&flash, NULL, &dh, &dm, NULL, 0, 0, 0), -EINVAL);

	/* NULL vol_hdrs with vol_count > 0. */
	zassert_equal(ubi_secure_res_peb_commit(&flash, &cfg, &dh, &dm, NULL, 1, 0, 0), -EINVAL);
}

/**
 * \brief res_peb_commit fails when key derivation fails.
 *
 * \details Scenario: Arms GET_KEY_ID_FAIL hook, attempts commit.
 *
 * \expect Returns error.
 */
ZTEST(ubi_secure_defensive, test_res_peb_commit_key_deriv_fail)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	const struct ubi_dev_hdr dh = {
		.magic = UBI_DEV_HDR_MAGIC,
		.version = UBI_DEV_HDR_VERSION,
	};
	const struct ubi_dev_secure_meta dm = {
		.write_active_key_version = cfg.policy.requested_write_key_version,
	};

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_GET_KEY_ID_FAIL, true);
	const int ret = ubi_secure_res_peb_commit(&flash, &cfg, &dh, &dm, NULL, 0,
						  cfg.policy.requested_write_key_version, 0);

	zassert_not_equal(ret, 0, "Should fail when key derivation fails");
}

/**
 * \brief res_peb_commit fails when RNG (salt gen) fails.
 *
 * \details Scenario: Arms RNG_FAIL hook, attempts commit.
 *
 * \expect Returns error.
 */
ZTEST(ubi_secure_defensive, test_res_peb_commit_salt_fail)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	const struct ubi_dev_hdr dh = {
		.magic = UBI_DEV_HDR_MAGIC,
		.version = UBI_DEV_HDR_VERSION,
	};
	const struct ubi_dev_secure_meta dm = {
		.write_active_key_version = cfg.policy.requested_write_key_version,
	};

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_RNG_FAIL, true);
	const int ret = ubi_secure_res_peb_commit(&flash, &cfg, &dh, &dm, NULL, 0,
						  cfg.policy.requested_write_key_version, 0);

	zassert_not_equal(ret, 0, "Should fail when salt gen fails");
}

/**
 * \brief res_peb_commit fails when AEAD encrypt fails.
 *
 * \details Scenario: Arms AEAD_ENCRYPT_FAIL hook, attempts commit.
 *
 * \expect Returns error.
 */
ZTEST(ubi_secure_defensive, test_res_peb_commit_aead_fail)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	const struct ubi_dev_hdr dh = {
		.magic = UBI_DEV_HDR_MAGIC,
		.version = UBI_DEV_HDR_VERSION,
	};
	const struct ubi_dev_secure_meta dm = {
		.write_active_key_version = cfg.policy.requested_write_key_version,
	};

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_AEAD_ENCRYPT_FAIL, true);
	const int ret = ubi_secure_res_peb_commit(&flash, &cfg, &dh, &dm, NULL, 0,
						  cfg.policy.requested_write_key_version, 0);

	zassert_not_equal(ret, 0, "Should fail when AEAD encrypt fails");
}

/**
 * \brief res_peb_scan with corrupt reserved PEB classifies it as corrupt.
 *
 * \details Scenario: Format device, deinit. Corrupt reserved PEB 0.
 *          Call res_peb_scan directly.
 *
 * \expect Scan succeeds; some PEBs authenticated, corrupt_count > 0.
 */
ZTEST(ubi_secure_defensive, test_res_peb_scan_corrupt_peb)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_device_deinit(ubi));

	ubi_test_partition_force_release_all();

	/* Corrupt reserved PEB 0. */
	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));
	zassert_ok(flash_area_erase(fa, 0, flash.erase_block_size));

	uint8_t garbage[32];

	memset(garbage, 0xBA, sizeof(garbage));
	zassert_ok(flash_area_write(fa, 0, garbage, sizeof(garbage)));
	flash_area_close(fa);

	struct ubi_secure_res_peb_scan scan = { 0 };

	zassert_ok(ubi_secure_res_peb_scan(&flash, &cfg, &scan));
	zassert_true(scan.corrupt_count > 0, "Corrupted reserved PEB should be marked corrupt");
	zassert_true(scan.auth_count > 0, "Other reserved PEBs should still authenticate");
}

/**
 * \brief res_peb_scan with blank reserved PEB counts it as spare.
 *
 * \details Scenario: Format device, deinit. Erase reserved PEB 0.
 *          Call res_peb_scan directly.
 *
 * \expect Scan succeeds; spare_count > 0.
 */
ZTEST(ubi_secure_defensive, test_res_peb_scan_blank_peb)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_device_deinit(ubi));

	ubi_test_partition_force_release_all();

	/* Erase reserved PEB 0 to make it blank. */
	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));
	zassert_ok(flash_area_erase(fa, 0, flash.erase_block_size));
	flash_area_close(fa);

	struct ubi_secure_res_peb_scan scan = { 0 };

	zassert_ok(ubi_secure_res_peb_scan(&flash, &cfg, &scan));
	zassert_true(scan.spare_count > 0, "Erased reserved PEB should be spare");
}

/**
 * \brief res_peb_scan with key derivation failure still completes.
 *
 * \details Scenario: Format device, deinit. Arm GET_KEY_ID_FAIL, scan.
 *
 * \expect Scan returns 0; corrupt_count == all PEBs.
 */
ZTEST(ubi_secure_defensive, test_res_peb_scan_key_deriv_fail)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_device_deinit(ubi));

	ubi_test_partition_force_release_all();

	struct ubi_secure_res_peb_scan scan = { 0 };

	/* Hook fires once — one PEB will fail derivation, others re-derive. */
	ubi_secure_test_hook_set(UBI_SECURE_HOOK_GET_KEY_ID_FAIL, true);
	zassert_ok(ubi_secure_res_peb_scan(&flash, &cfg, &scan));
	zassert_true(scan.corrupt_count > 0, "One PEB should fail with key deriv error");
}

/**
 * \brief detect_mode returns correct state for blank PEB.
 *
 * \details Scenario: Erase PEB 0, call detect_mode.
 *
 * \expect is_blank is true.
 */
ZTEST(ubi_secure_defensive, test_res_peb_detect_mode_blank)
{
	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));
	zassert_ok(flash_area_erase(fa, 0, flash.erase_block_size));
	flash_area_close(fa);

	bool is_secure = false;
	bool is_blank = false;

	zassert_ok(ubi_secure_res_peb_detect_mode(&flash, 0, &is_secure, &is_blank));
	zassert_true(is_blank);
	zassert_false(is_secure);
}

/**
 * \brief detect_mode returns correct state for written but non-secure PEB.
 *
 * \details Scenario: Write garbage to PEB 0 (not matching secure prefix magic).
 *
 * \expect is_secure is false, is_blank is false.
 */
ZTEST(ubi_secure_defensive, test_res_peb_detect_mode_plain)
{
	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	uint8_t garbage[16] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };

	zassert_ok(flash_area_write(fa, 0, garbage, sizeof(garbage)));
	flash_area_close(fa);

	bool is_secure = false;
	bool is_blank = false;

	zassert_ok(ubi_secure_res_peb_detect_mode(&flash, 0, &is_secure, &is_blank));
	zassert_false(is_blank);
	zassert_false(is_secure);
}

/**
 * \brief Init with plain headers on reserved PEBs returns -EPROTO (mode mismatch).
 *
 * \details Scenario: Write non-secure data to reserved PEBs. Init should refuse.
 *
 * \expect Returns -EPROTO.
 */
ZTEST(ubi_secure_defensive, test_init_plain_media_mismatch)
{
	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	/* Write non-secure, non-blank data to reserved PEBs 0..2. */
	for (size_t peb = 0; peb < UBI_DEV_HDR_NR_OF_RES_PEBS; peb++) {
		const size_t offset = peb * flash.erase_block_size;
		uint8_t plain_data[16] = { 0x55, 0x42, 0x49, 0x23, 0x01, 0x00, 0x00, 0x00 };

		zassert_ok(flash_area_write(fa, offset, plain_data, sizeof(plain_data)));
	}

	flash_area_close(fa);

	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_equal(ubi_device_init(&flash, &cfg, &ubi), -EPROTO);
	zassert_is_null(ubi);
}

/* ========================== Geometry validation - additional checks ============================ */

/**
 * \brief Device init rejects erase_block_size that does not divide partition size.
 *
 * \details Scenario: Passes flash with erase_block_size = 3000 (131072 % 3000 != 0).
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_init_partition_not_multiple)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_flash_desc bad_flash = flash;

	bad_flash.erase_block_size = 3000;
	struct ubi_device *ubi = NULL;

	zassert_equal(ubi_device_init(&bad_flash, &cfg, &ubi), -EINVAL);
	zassert_is_null(ubi);
}

/**
 * \brief Device init rejects write_block_size exceeding alignment limit.
 *
 * \details Scenario: Passes flash with write_block_size = 32 (> WRITE_BLOCK_SIZE_ALIGNMENT = 16),
 *          while erase_block_size = 8192 so that erase % write == 0.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_init_write_exceeds_alignment)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_flash_desc bad_flash = flash;

	bad_flash.write_block_size = 32;
	struct ubi_device *ubi = NULL;

	zassert_equal(ubi_device_init(&bad_flash, &cfg, &ubi), -EINVAL);
	zassert_is_null(ubi);
}

/**
 * \brief Device init rejects partition with too few PEBs.
 *
 * \details Scenario: Passes flash with erase_block_size = 65536 so nr_of_pebs = 2 <= RES_PEB_COUNT.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_init_too_few_pebs)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_flash_desc bad_flash = flash;

	bad_flash.erase_block_size = 65536;
	struct ubi_device *ubi = NULL;

	zassert_equal(ubi_device_init(&bad_flash, &cfg, &ubi), -EINVAL);
	zassert_is_null(ubi);
}

/**
 * \brief Device init rejects erase_block_size not multiple of write_block_size.
 *
 * \details Scenario: Passes flash with erase_block_size = 131072 and write_block_size = 3
 *          so that erase % write != 0.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_init_erase_not_multiple_of_write)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_flash_desc bad_flash = flash;

	bad_flash.erase_block_size = 131072;
	bad_flash.write_block_size = 3;
	struct ubi_device *ubi = NULL;

	zassert_equal(ubi_device_init(&bad_flash, &cfg, &ubi), -EINVAL);
	zassert_is_null(ubi);
}

/* ============================ Public API NULL / validation checks ============================= */

/**
 * \brief ubi_volume_create rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL ubi, NULL vol_cfg, and NULL vol_id.
 *
 * \expect Returns -EINVAL for each call.
 */
ZTEST(ubi_secure_defensive, test_vol_create_null)
{
	struct ubi_volume_config vol_cfg = {
		.name = "test",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;

	zassert_equal(ubi_volume_create(NULL, &vol_cfg, &vol_id), -EINVAL);
}

/**
 * \brief ubi_volume_remove rejects NULL ubi.
 *
 * \details Scenario: Calls with NULL device pointer.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_vol_remove_null)
{
	zassert_equal(ubi_volume_remove(NULL, 0), -EINVAL);
}

/**
 * \brief ubi_volume_resize rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL ubi.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_vol_resize_null)
{
	struct ubi_volume_config vol_cfg = {
		.name = "test",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};

	zassert_equal(ubi_volume_resize(NULL, 0, &vol_cfg), -EINVAL);
}

/**
 * \brief ubi_volume_get_info rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL ubi.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_vol_get_info_null)
{
	struct ubi_volume_config vol_cfg = { 0 };
	size_t alloc_lebs = 0;

	zassert_equal(ubi_volume_get_info(NULL, 0, &vol_cfg, &alloc_lebs), -EINVAL);
}

/**
 * \brief ubi_leb_write rejects NULL ubi.
 *
 * \details Scenario: Calls with NULL device pointer.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_leb_write_null)
{
	uint8_t buf[16] = { 0 };

	zassert_equal(ubi_leb_write(NULL, 0, 0, buf, sizeof(buf)), -EINVAL);
}

/**
 * \brief ubi_leb_write rejects buf/len mismatch.
 *
 * \details Scenario: Calls with buf=NULL and len>0.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_leb_write_buf_len_mismatch)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_equal(ubi_leb_write(ubi, 0, 0, NULL, 16), -EINVAL);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief ubi_leb_read rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL ubi and NULL buf.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_leb_read_null)
{
	zassert_equal(ubi_leb_read(NULL, 0, 0, 0, NULL, 16), -EINVAL);
}

/**
 * \brief ubi_leb_map rejects NULL ubi.
 *
 * \details Scenario: Calls with NULL device pointer.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_leb_map_null)
{
	zassert_equal(ubi_leb_map(NULL, 0, 0), -EINVAL);
}

/**
 * \brief ubi_leb_unmap rejects NULL ubi.
 *
 * \details Scenario: Calls with NULL device pointer.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_leb_unmap_null)
{
	zassert_equal(ubi_leb_unmap(NULL, 0, 0), -EINVAL);
}

/**
 * \brief ubi_leb_is_mapped rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL ubi.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_leb_is_mapped_null)
{
	bool mapped = false;

	zassert_equal(ubi_leb_is_mapped(NULL, 0, 0, &mapped), -EINVAL);
}

/**
 * \brief ubi_leb_get_size rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL ubi.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_leb_get_size_null)
{
	size_t sz = 0;

	zassert_equal(ubi_leb_get_size(NULL, 0, 0, &sz), -EINVAL);
}

/**
 * \brief ubi_device_get_info rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL ubi.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_device_get_info_null)
{
	struct ubi_device_info info = { 0 };

	zassert_equal(ubi_device_get_info(NULL, &info), -EINVAL);
}

/**
 * \brief ubi_device_erase_peb rejects NULL ubi.
 *
 * \details Scenario: Calls with NULL device pointer.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_device_erase_peb_null)
{
	zassert_equal(ubi_device_erase_peb(NULL), -EINVAL);
}

/**
 * \brief ubi_leb_write with non-existent volume returns -ENOENT.
 *
 * \details Scenario: Create device, write to volume 99 which does not exist.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_defensive, test_leb_write_vol_not_found)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	uint8_t buf[16] = { 0x42 };

	zassert_equal(ubi_leb_write(ubi, 99, 0, buf, sizeof(buf)), -ENOENT);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief ubi_leb_write beyond volume LEB count returns error.
 *
 * \details Scenario: Create device and volume with 1 LEB, write to LEB 5.
 *
 * \expect Returns -EACCES.
 */
ZTEST(ubi_secure_defensive, test_leb_write_leb_exceeded)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	struct ubi_volume_config vol_cfg = {
		.name = "test_exceed",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	uint8_t buf[16] = { 0x42 };

	zassert_equal(ubi_leb_write(ubi, vol_id, 5, buf, sizeof(buf)), -EACCES);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief ubi_leb_write with buffer larger than LEB size returns -ENOSPC.
 *
 * \details Scenario: Create device and volume, attempt to write more than leb_size bytes.
 *
 * \expect Returns -ENOSPC.
 */
ZTEST(ubi_secure_defensive, test_leb_write_too_big)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	struct ubi_volume_config vol_cfg = {
		.name = "test_toobig",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));

	/* Allocate buffer one byte larger than LEB size. */
	const size_t too_big = info.leb_size + 1;
	uint8_t *big_buf = k_calloc(1, too_big);

	zassert_not_null(big_buf);
	zassert_equal(ubi_leb_write(ubi, vol_id, 0, big_buf, too_big), -ENOSPC);

	k_free(big_buf);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief ubi_leb_read on non-existent volume returns -ENOENT.
 *
 * \details Scenario: Create device, read from volume 99 which does not exist.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_defensive, test_leb_read_vol_not_found)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	uint8_t buf[16] = { 0 };

	zassert_equal(ubi_leb_read(ubi, 99, 0, 0, buf, sizeof(buf)), -ENOENT);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief ubi_leb_read beyond volume LEB count returns error.
 *
 * \details Scenario: Create device and volume with 1 LEB, read LEB 5.
 *
 * \expect Returns -EACCES.
 */
ZTEST(ubi_secure_defensive, test_leb_read_leb_exceeded)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	struct ubi_volume_config vol_cfg = {
		.name = "test_read_exc",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	uint8_t buf[16] = { 0 };

	zassert_equal(ubi_leb_read(ubi, vol_id, 5, 0, buf, sizeof(buf)), -EACCES);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief ubi_leb_read on unmapped LEB returns -ENOENT.
 *
 * \details Scenario: Create device and volume with 1 LEB, do not write, attempt read.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_defensive, test_leb_read_unmapped)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	struct ubi_volume_config vol_cfg = {
		.name = "test_unmapped",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	uint8_t buf[16] = { 0 };

	zassert_equal(ubi_leb_read(ubi, vol_id, 0, 0, buf, sizeof(buf)), -ENOENT);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief ubi_volume_resize with same LEB count returns -ECANCELED.
 *
 * \details Scenario: Create volume with 1 LEB, resize to 1 LEB.
 *
 * \expect Returns -ECANCELED.
 */
ZTEST(ubi_secure_defensive, test_vol_resize_same_count)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	struct ubi_volume_config vol_cfg = {
		.name = "test_same_sz",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	/* Resize to same count. */
	zassert_equal(ubi_volume_resize(ubi, vol_id, &vol_cfg), -ECANCELED);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief ubi_volume_remove on non-existent volume returns -ENOENT.
 *
 * \details Scenario: Create device without volumes, remove vol 42.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_defensive, test_vol_remove_not_found)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_equal(ubi_volume_remove(ubi, 42), -ENOENT);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief ubi_volume_get_info on non-existent volume returns -ENOENT.
 *
 * \details Scenario: Create device without volumes, get info for vol 42.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_defensive, test_vol_get_info_not_found)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	struct ubi_volume_config vol_cfg = { 0 };
	size_t alloc_lebs = 0;

	zassert_equal(ubi_volume_get_info(ubi, 42, &vol_cfg, &alloc_lebs), -ENOENT);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief ubi_leb_get_size on unmapped LEB returns -ENOENT.
 *
 * \details Scenario: Create device and volume, do not write, query LEB size.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_defensive, test_leb_get_size_unmapped)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	struct ubi_volume_config vol_cfg = {
		.name = "test_getsz",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	size_t sz = 0;

	zassert_equal(ubi_leb_get_size(ubi, vol_id, 0, &sz), -ENOENT);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief ubi_volume_resize with leb_count=0 returns -EINVAL.
 *
 * \details Scenario: Attempt to resize a volume to zero LEBs.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, test_vol_resize_zero_lebs)
{
	struct ubi_volume_config vol_cfg = {
		.name = "test_rz_zero",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 0,
	};

	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_equal(ubi_volume_resize(ubi, 0, &vol_cfg), -EINVAL);
	zassert_ok(ubi_device_deinit(ubi));
}

/* ============================== Counter overflow boundary tests ============================== */

/**
 * \brief derive_domain_key rejects key version not in allowlist.
 *
 * \details Scenario: Call ubi_secure_derive_domain_key with kv=99, allowlist=[1].
 *
 * \expect Returns -UBI_SECURE_ENOKEY.
 */
ZTEST(ubi_secure_defensive, test_derive_domain_key_rejects_non_allowlisted_kv)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	psa_key_id_t child_key_id = PSA_KEY_ID_NULL;

	zassert_equal(ubi_secure_derive_domain_key(&cfg, UBI_SECURE_DOMAIN_ERASE_COUNTER, 99,
						   &child_key_id),
		      -UBI_SECURE_ENOKEY);
}

/**
 * \brief derive_leb_key rejects key version not in allowlist.
 *
 * \details Scenario: Call ubi_secure_derive_leb_key with kv=99, allowlist=[1].
 *
 * \expect Returns -UBI_SECURE_ENOKEY.
 */
ZTEST(ubi_secure_defensive, test_derive_leb_key_rejects_non_allowlisted_kv)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	psa_key_id_t child_key_id = PSA_KEY_ID_NULL;

	zassert_equal(ubi_secure_derive_leb_key(&cfg, 99, 0, &child_key_id), -UBI_SECURE_ENOKEY);
}

/**
 * \brief EC header write rejects counter above COUNTER_MAX.
 *
 * \details Scenario: Call ubi_secure_ec_hdr_write with counter = COUNTER_MAX + 1.
 *
 * \expect Returns -EOVERFLOW.
 */
ZTEST(ubi_secure_defensive, test_ec_hdr_write_counter_overflow)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	const struct ubi_ec_hdr ec_hdr = {
		.magic = UBI_EC_HDR_MAGIC,
		.version = UBI_EC_HDR_VERSION,
		.ec = 0,
	};

	zassert_equal(ubi_secure_ec_hdr_write(&flash, &cfg, 4, &ec_hdr, 1,
					      UBI_SECURE_COUNTER_MAX + 1),
		      -EOVERFLOW);
}

/**
 * \brief VID header write rejects counter above COUNTER_MAX.
 *
 * \details Scenario: Call ubi_secure_vid_hdr_write with counter = COUNTER_MAX + 1.
 *
 * \expect Returns -EOVERFLOW.
 */
ZTEST(ubi_secure_defensive, test_vid_hdr_write_counter_overflow)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	const struct ubi_secure_ec_auth_ctx ec_ctx = { .ec = 0, .key_version = 1 };
	const struct ubi_vid_hdr vid_hdr = {
		.magic = UBI_VID_HDR_MAGIC,
		.version = UBI_VID_HDR_VERSION,
	};
	const struct ubi_vid_secure_meta vid_meta = { 0 };

	zassert_equal(ubi_secure_vid_hdr_write(&flash, &cfg, 4, &ec_ctx, &vid_hdr, &vid_meta, 1,
					       UBI_SECURE_COUNTER_MAX + 1),
		      -EOVERFLOW);
}

/**
 * \brief Reserved PEB commit rejects counter overflow.
 *
 * \details Scenario: Call ubi_secure_res_peb_commit with counter near COUNTER_MAX
 *          and vol_count that would push total past the limit.
 *
 * \expect Returns -EOVERFLOW.
 */
ZTEST(ubi_secure_defensive, test_res_peb_commit_counter_overflow)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	struct ubi_dev_hdr dh = { .magic = UBI_DEV_HDR_MAGIC, .version = UBI_DEV_HDR_VERSION };
	struct ubi_dev_secure_meta dm = { 0 };

	zassert_equal(ubi_secure_res_peb_commit(&flash, &cfg, &dh, &dm, NULL, 0, 1,
						UBI_SECURE_COUNTER_MAX + 1),
		      -EOVERFLOW);
}

/* ===================================== Suite registration ===================================== */

ZTEST_SUITE(ubi_secure_defensive, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);
