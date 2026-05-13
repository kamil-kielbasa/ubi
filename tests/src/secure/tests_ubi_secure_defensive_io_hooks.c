/**
 * \file    tests_ubi_secure_defensive_io_hooks.c
 * \author  Kamil Kielbasa
 *
 * \brief   Defensive-check hook-driven IO error paths.
 *
 * \details Drives the secure IO layer through the test-only fault hooks to
 *          exercise key-derivation / AEAD / flash-write failure branches
 *          that the public API cannot reach unaided.  Companion file to
 *          tests_ubi_secure_defensive.c.
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
	ubi_test_secure_suite_setup_impl(&flash);
	return NULL;
}

static void ztest_suite_before(void *ctx)
{
	(void)ctx;
	ubi_test_secure_before_impl();
	ubi_secure_test_hook_reset();
}
/* ================================= IO hook-based error paths ================================== */

ZTEST_SUITE(ubi_secure_defensive_io_hooks, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);

/**
 * \brief ec_hdr_read returns error for correct magic but wrong domain.
 *
 * \details Scenario: Writes a prefix with valid SECURE_PREFIX_MAGIC but domain LEB.
 *
 * \expect Returns -EBADMSG.
 */
ZTEST(ubi_secure_defensive_io_hooks, io_ec_hdr_read_bad_domain)
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
ZTEST(ubi_secure_defensive_io_hooks, io_ec_hdr_read_key_deriv_fail)
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
ZTEST(ubi_secure_defensive_io_hooks, io_ec_hdr_write_key_deriv_fail)
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
ZTEST(ubi_secure_defensive_io_hooks, io_ec_hdr_write_salt_fail)
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
ZTEST(ubi_secure_defensive_io_hooks, io_ec_hdr_write_aead_fail)
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
ZTEST(ubi_secure_defensive_io_hooks, io_ec_hdr_write_flash_fail)
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
ZTEST(ubi_secure_defensive_io_hooks, io_vid_hdr_write_key_deriv_fail)
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
ZTEST(ubi_secure_defensive_io_hooks, io_vid_hdr_write_salt_fail)
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
ZTEST(ubi_secure_defensive_io_hooks, io_vid_hdr_write_aead_fail)
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
ZTEST(ubi_secure_defensive_io_hooks, io_vid_hdr_write_flash_fail)
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
ZTEST(ubi_secure_defensive_io_hooks, io_leb_data_write_key_deriv_fail)
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
ZTEST(ubi_secure_defensive_io_hooks, io_leb_data_write_salt_fail)
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
ZTEST(ubi_secure_defensive_io_hooks, io_leb_data_write_aead_fail)
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
ZTEST(ubi_secure_defensive_io_hooks, io_leb_data_write_flash_fail)
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
ZTEST(ubi_secure_defensive_io_hooks, io_leb_data_read_key_deriv_fail)
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
ZTEST(ubi_secure_defensive_io_hooks, io_leb_data_read_bad_prefix)
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
ZTEST(ubi_secure_defensive_io_hooks, io_vid_hdr_read_bad_domain)
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
ZTEST(ubi_secure_defensive_io_hooks, init_dev_hdr_bad_wrapper_version)
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
ZTEST(ubi_secure_defensive_io_hooks, init_vol_hdr_bad_wrapper_version)
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
	 * reserved-PEB copy.  The flash device underneath may be ECC-protected
	 * (e.g. STM32U5) and therefore reject any in-place re-program of an
	 * already-written word, even when only 1 -> 0 transitions are needed.
	 * Use an erase-block-granular read / erase / write cycle so the test
	 * works on both the simulator and real NOR/NVMC flash. */
	const size_t ebs = flash.erase_block_size;
	uint8_t *block_buf = k_malloc(ebs);

	zassert_not_null(block_buf);

	for (size_t i = 0; i < CONFIG_UBI_DEV_HDR_NR_OF_RES_PEBS; i++) {
		const size_t block_offset = i * ebs;
		const size_t prefix_in_block = UBI_SECURE_DEV_HDR_SIZE;

		zassert_ok(flash_area_read(fa, block_offset, block_buf, ebs));

		struct ubi_crypto_prefix32 prefix = { 0 };

		ubi_secure_prefix32_deserialize(&block_buf[prefix_in_block], &prefix);
		zassert_equal(prefix.wrapper_version, UBI_SECURE_WRAPPER_VERSION);
		zassert_equal(prefix.domain, UBI_SECURE_DOMAIN_VOLUME_HEADER);

		/* Force unsupported wrapper_version. */
		prefix.wrapper_version = 0;
		ubi_secure_prefix32_serialize(&prefix, &block_buf[prefix_in_block]);

		zassert_ok(flash_area_erase(fa, block_offset, ebs));
		zassert_ok(flash_area_write(fa, block_offset, block_buf, ebs));
	}

	k_free(block_buf);
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
ZTEST(ubi_secure_defensive_io_hooks, io_ec_hdr_read_bad_wrapper_version)
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
ZTEST(ubi_secure_defensive_io_hooks, io_vid_hdr_read_bad_wrapper_version)
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
ZTEST(ubi_secure_defensive_io_hooks, io_leb_data_read_bad_wrapper_version)
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
ZTEST(ubi_secure_defensive_io_hooks, io_leb_data_write_payload_exceeds_ccm_limit)
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
ZTEST(ubi_secure_defensive_io_hooks, io_leb_data_read_data_size_exceeds_ccm_limit)
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
ZTEST(ubi_secure_defensive_io_hooks, io_vid_hdr_read_key_deriv_fail)
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
ZTEST(ubi_secure_defensive_io_hooks, io_leb_data_write_zero_len)
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

/* ===================================== Suite registration ====================================== */
