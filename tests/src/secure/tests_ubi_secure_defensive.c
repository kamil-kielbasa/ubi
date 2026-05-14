/**
 * \file    tests_ubi_secure_defensive.c
 * \author  Kamil Kielbasa
 *
 * \brief   Defensive-check NULL guards and corruption paths (core).
 *
 * \details Covers serialization, crypto, I/O and scan defensive checks plus
 *          init-time geometry validation.  Split out of the original
 *          monolithic defensive suite for navigability; sibling files
 *          cover IO hook-based fault paths and reserved-PEB / public-API
 *          / counter-overflow boundaries.
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>
/* Internal secure headers (via target_include_directories). */
#include "ubi_secure_crypto.h"
#include "ubi_secure_ser.h"
#include "ubi_secure_io.h"
#include "ubi_secure_types.h"
#include "ubi_secure_test_hooks.h"
#include "ubi_secure_reserved.h"

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
	ubi_secure_test_hook_reset();
}

/* ================================= Serialization NULL checks ================================== */

/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_secure_defensive, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);

/**
 * \brief prefix32_serialize rejects NULL prefix.
 *
 * \details Scenario: Calls with NULL source pointer.
 *
 * \expect Returns without crash.
 */
ZTEST(ubi_secure_defensive, ser_prefix32_serialize_null)
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
ZTEST(ubi_secure_defensive, ser_prefix32_deserialize_null)
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
ZTEST(ubi_secure_defensive, ser_aad_builders_null)
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
ZTEST(ubi_secure_defensive, ser_dev_meta_null)
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
ZTEST(ubi_secure_defensive, ser_vid_meta_null)
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
ZTEST(ubi_secure_defensive, ser_counter48_null)
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
ZTEST(ubi_secure_defensive, crypto_aead_encrypt_null)
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
ZTEST(ubi_secure_defensive, crypto_aead_decrypt_null)
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
ZTEST(ubi_secure_defensive, crypto_generate_salt_null)
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
ZTEST(ubi_secure_defensive, crypto_build_nonce_null)
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
ZTEST(ubi_secure_defensive, crypto_derive_domain_key_null)
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
ZTEST(ubi_secure_defensive, crypto_derive_leb_key_null)
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
ZTEST(ubi_secure_defensive, io_ec_hdr_read_null)
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
ZTEST(ubi_secure_defensive, io_vid_hdr_read_null)
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
ZTEST(ubi_secure_defensive, io_vid_hdr_write_null)
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
ZTEST(ubi_secure_defensive, io_leb_data_read_null)
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
ZTEST(ubi_secure_defensive, io_leb_data_write_null)
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
ZTEST(ubi_secure_defensive, io_ec_hdr_write_null)
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
ZTEST(ubi_secure_defensive, io_ec_hdr_read_bad_magic)
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
ZTEST(ubi_secure_defensive, io_vid_hdr_read_bad_magic)
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
ZTEST(ubi_secure_defensive, io_leb_data_read_zero_datasize)
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
ZTEST(ubi_secure_defensive, io_leb_data_read_out_of_bounds)
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
ZTEST(ubi_secure_defensive, init_zero_write_block)
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
ZTEST(ubi_secure_defensive, init_zero_erase_block)
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
ZTEST(ubi_secure_defensive, init_oversized_write_block)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_flash_desc bad_flash = flash;

	/* Set to a large value unlikely to match any real flash. */
	bad_flash.write_block_size = 65536;
	struct ubi_device *ubi = NULL;

	const int ret = ubi_device_init(&bad_flash, &cfg, &ubi);

	zassert_equal(ret, -EINVAL);
}

/**
 * \brief Device init rejects erase block too small for headers.
 *
 * \details Scenario: Passes flash with very small erase_block_size.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, init_erase_block_too_small)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_flash_desc bad_flash = flash;

	/* Set erase block to 16 bytes — too small for EC+VID+LEB overhead. */
	bad_flash.erase_block_size = 16;
	struct ubi_device *ubi = NULL;

	const int ret = ubi_device_init(&bad_flash, &cfg, &ubi);

	zassert_equal(ret, -EINVAL, "init must reject erase_block_size=16 with -EINVAL, got %d",
		      ret);
	zassert_is_null(ubi, "device handle must remain NULL on -EINVAL");
}

/**
 * \brief Device init rejects write key version not in allowlist.
 *
 * \details Scenario: Passes crypto config with mismatched write key version.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive, init_bad_write_key_version)
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
ZTEST(ubi_secure_defensive, scan_corrupt_ec_marks_bad)
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
ZTEST(ubi_secure_defensive, scan_erased_vid_dirty_leb)
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
	zassert_true(info_after.dirty_peb_count > 0,
		     "Erased-VID + written-LEB PEB must be classified dirty: dirty=%zu bad=%zu",
		     info_after.dirty_peb_count, info_after.bad_peb_count);

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
ZTEST(ubi_secure_defensive, scan_orphan_classification)
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
ZTEST(ubi_secure_defensive, io_leb_data_read_zero_len_zero_data)
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
ZTEST(ubi_secure_defensive, io_vid_region_is_erased_null)
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
ZTEST(ubi_secure_defensive, io_leb_prefix_is_erased_null)
{
	bool erased = false;

	zassert_equal(ubi_secure_leb_prefix_is_erased(NULL, 0, &erased), -EINVAL);
	zassert_equal(ubi_secure_leb_prefix_is_erased(&flash, 0, NULL), -EINVAL);
}

/**
 * \brief `ubi_secure_aead_encrypt` rejects `aad == NULL` with non-zero `aad_len`.
 *
 * \details Scenario: Pass a valid nonce / ciphertext buffers but `aad = NULL`
 *          and `aad_len = 16`.  This is the second NULL-checking branch
 *          inside `ubi_secure_aead_encrypt` (after the nonce/ct/ct_len
 *          guard) and is otherwise unreachable from production callers
 *          which always pair AAD pointer + length consistently.
 *
 * \expect Returns `-EINVAL` without performing any PSA call.
 *
 * \oracle `ret == -EINVAL`.
 *
 * \trace `LOG_ERR("aead_encrypt: NULL aad with non-zero aad_len")` in
 *        `lib/src/secure/ubi_secure_crypto.c`.
 *
 * \precondition None.
 */
ZTEST(ubi_secure_defensive, crypto_aead_encrypt_null_aad_with_len)
{
	const uint8_t nonce[UBI_SECURE_NONCE_SIZE] = { 0 };
	uint8_t ct[32] = { 0 };
	size_t ct_len = 0;

	zassert_equal(-EINVAL, ubi_secure_aead_encrypt(0, nonce, NULL, 16, NULL, 0, ct, sizeof(ct),
						       &ct_len));
}

/**
 * \brief `ubi_secure_aead_decrypt` rejects `aad == NULL` with non-zero `aad_len`.
 *
 * \details Scenario: Mirror of `crypto_aead_encrypt_null_aad_with_len` for
 *          the decrypt path. Same defensive branch: AAD pointer + length
 *          must be consistent.
 *
 * \expect Returns `-EINVAL` without performing any PSA call.
 *
 * \oracle `ret == -EINVAL`.
 *
 * \trace `LOG_ERR("aead_decrypt: NULL aad with non-zero aad_len")` in
 *        `lib/src/secure/ubi_secure_crypto.c`.
 *
 * \precondition None.
 */
ZTEST(ubi_secure_defensive, crypto_aead_decrypt_null_aad_with_len)
{
	const uint8_t nonce[UBI_SECURE_NONCE_SIZE] = { 0 };
	uint8_t pt[32] = { 0 };
	size_t pt_len = 0;

	zassert_equal(-EINVAL, ubi_secure_aead_decrypt(0, nonce, NULL, 16, NULL, 0, pt, sizeof(pt),
						       &pt_len));
}

/**
 * \brief `ubi_secure_aead_encrypt` returns `-EIO` when PSA rejects the operation.
 *
 * \details Scenario: Call `ubi_secure_aead_encrypt` with a non-existent key
 *          id (`PSA_KEY_ID_NULL`), valid nonce and a tiny ciphertext buffer.
 *          PSA will return a non-success status (`PSA_ERROR_INVALID_HANDLE`
 *          or similar) and the helper must translate it into `-EIO` and
 *          log `"AEAD encrypt failed"`.
 *
 * \expect Returns `-EIO`.
 *
 * \oracle `ret == -EIO`.
 *
 * \trace `LOG_ERR("AEAD encrypt failed: %d")` branch in
 *        `lib/src/secure/ubi_secure_crypto.c`.
 *
 * \precondition PSA initialised by `ubi_test_secure_suite_setup_impl`.
 */
ZTEST(ubi_secure_defensive, crypto_aead_encrypt_propagates_psa_error)
{
	const uint8_t nonce[UBI_SECURE_NONCE_SIZE] = { 0 };
	const uint8_t pt[16] = { 0 };
	uint8_t ct[64] = { 0 };
	size_t ct_len = 0;

	zassert_equal(-EIO, ubi_secure_aead_encrypt(PSA_KEY_ID_NULL, nonce, NULL, 0, pt, sizeof(pt),
						    ct, sizeof(ct), &ct_len));
}

/**
 * \brief `ubi_secure_aead_decrypt` returns `-EIO` when PSA rejects the operation.
 *
 * \details Scenario: Mirror of `crypto_aead_encrypt_propagates_psa_error`
 *          for the decrypt path. PSA returns a non-success status and the
 *          helper must surface it as `-EIO`.
 *
 * \expect Returns `-EIO`.
 *
 * \oracle `ret == -EIO`.
 *
 * \trace `LOG_ERR("AEAD decrypt failed: %d")` branch in
 *        `lib/src/secure/ubi_secure_crypto.c`.
 *
 * \precondition PSA initialised by `ubi_test_secure_suite_setup_impl`.
 */
ZTEST(ubi_secure_defensive, crypto_aead_decrypt_propagates_psa_error)
{
	const uint8_t nonce[UBI_SECURE_NONCE_SIZE] = { 0 };
	const uint8_t ct[32] = { 0 };
	uint8_t pt[64] = { 0 };
	size_t pt_len = 0;

	zassert_equal(-EIO, ubi_secure_aead_decrypt(PSA_KEY_ID_NULL, nonce, NULL, 0, ct, sizeof(ct),
						    pt, sizeof(pt), &pt_len));
}

/**
 * \brief `ubi_secure_leb_data_read` rejects `buf == NULL` with non-zero `len`.
 *
 * \details Scenario: Provide a valid `vid_ctx` (non-NULL `vid_hdr` with
 *          `data_size = 4`) and pass `buf = NULL` with `len = 4`.  This
 *          trips the second NULL guard inside `ubi_secure_leb_data_read`
 *          (after the flash/cfg/vid_ctx checks), which is otherwise
 *          unreachable from public callers that always pair buffer pointer
 *          and length consistently.
 *
 * \expect Returns `-EINVAL` without performing any flash I/O.
 *
 * \oracle `ret == -EINVAL`.
 *
 * \trace `LOG_ERR("leb_data_read: NULL buf with len %zu")` in
 *        `lib/src/secure/ubi_secure_io.c`.
 *
 * \precondition None.
 */
ZTEST(ubi_secure_defensive, io_leb_data_read_null_buf_with_len)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	struct ubi_vid_hdr vid = { .data_size = 4 };
	struct ubi_secure_vid_auth_ctx vid_ctx = { .vid_hdr = &vid };

	zassert_equal(-EINVAL, ubi_secure_leb_data_read(&flash, &cfg, 3, &vid_ctx, 0, NULL, 4));
}

/**
 * \brief `ubi_secure_leb_data_write` rejects `buf == NULL` with non-zero `len`.
 *
 * \details Scenario: Provide a valid `ec_ctx` and `vid_hdr`, pass `buf = NULL`
 *          with `len = 4`.  This trips the second NULL guard inside
 *          `ubi_secure_leb_data_write` (after the flash/cfg/ec_ctx/vid_hdr
 *          checks), which is otherwise unreachable from public callers.
 *
 * \expect Returns `-EINVAL` without performing any flash I/O.
 *
 * \oracle `ret == -EINVAL`.
 *
 * \trace `LOG_ERR("leb_data_write: NULL buf with len %zu")` in
 *        `lib/src/secure/ubi_secure_io.c`.
 *
 * \precondition None.
 */
ZTEST(ubi_secure_defensive, io_leb_data_write_null_buf_with_len)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };
	struct ubi_vid_hdr vid = { 0 };

	zassert_equal(-EINVAL,
		      ubi_secure_leb_data_write(&flash, &cfg, 3, &ec_ctx, &vid, 0, NULL, 4, 0, 0));
}
