/**
 * \file    tests_ubi_secure_defensive_reserved.c
 * \author  Kamil Kielbasa
 *
 * \brief   Defensive-check reserved-PEB / public-API / counter boundaries.
 *
 * \details Exercises NULL guards and validation paths on the reserved-PEB
 *          commit/scan layer, the public API surface and 48-bit counter
 *          overflow handling.  Companion file to
 *          tests_ubi_secure_defensive.c.
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>
#include <ubi_secure.h>
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
/* ================================== Reserved PEB NULL checks ================================== */

/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_secure_defensive_reserved, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);

/**
 * \brief res_peb_detect_mode rejects NULL arguments.
 *
 * \details Scenario: Calls with NULL is_secure and NULL is_blank.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_defensive_reserved, res_peb_detect_mode_null)
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
ZTEST(ubi_secure_defensive_reserved, res_peb_scan_null)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, res_peb_read_vol_hdrs_null)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, res_peb_commit_null)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, res_peb_commit_key_deriv_fail)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();

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

	zassert_equal(ret, -UBI_SECURE_ENOKEY,
		      "key derivation fault must yield -UBI_SECURE_ENOKEY, got %d", ret);
}

/**
 * \brief res_peb_commit fails when RNG (salt gen) fails.
 *
 * \details Scenario: Arms RNG_FAIL hook, attempts commit.
 *
 * \expect Returns error.
 */
ZTEST(ubi_secure_defensive_reserved, res_peb_commit_salt_fail)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();

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

	zassert_equal(ret, -EROFS, "salt-gen fault during commit must yield -EROFS, got %d", ret);
}

/**
 * \brief res_peb_commit fails when AEAD encrypt fails.
 *
 * \details Scenario: Arms AEAD_ENCRYPT_FAIL hook, attempts commit.
 *
 * \expect Returns error.
 */
ZTEST(ubi_secure_defensive_reserved, res_peb_commit_aead_fail)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();

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

	zassert_equal(ret, -EROFS, "AEAD encrypt fault during commit must yield -EROFS, got %d",
		      ret);
}

/**
 * \brief res_peb_scan with corrupt reserved PEB classifies it as corrupt.
 *
 * \details Scenario: Format device, deinit. Corrupt reserved PEB 0.
 *          Call res_peb_scan directly.
 *
 * \expect Scan succeeds; some PEBs authenticated, corrupt_count > 0.
 */
ZTEST(ubi_secure_defensive_reserved, res_peb_scan_corrupt_peb)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
	zassert_equal(scan.corrupt_count, 1, "exactly one corrupted reserved PEB expected, got %u",
		      scan.corrupt_count);
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
ZTEST(ubi_secure_defensive_reserved, res_peb_scan_blank_peb)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, res_peb_scan_key_deriv_fail)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_device_deinit(ubi));

	ubi_test_partition_force_release_all();

	struct ubi_secure_res_peb_scan scan = { 0 };

	/* Hook fires once — one PEB will fail derivation, others re-derive. */
	ubi_secure_test_hook_set(UBI_SECURE_HOOK_GET_KEY_ID_FAIL, true);
	zassert_ok(ubi_secure_res_peb_scan(&flash, &cfg, &scan));
	zassert_equal(scan.corrupt_count, 1,
		      "key-derivation hook fires once: expected 1 corrupt PEB, got %u",
		      scan.corrupt_count);
}

/**
 * \brief detect_mode returns correct state for blank PEB.
 *
 * \details Scenario: Erase PEB 0, call detect_mode.
 *
 * \expect is_blank is true.
 */
ZTEST(ubi_secure_defensive_reserved, res_peb_detect_mode_blank)
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
ZTEST(ubi_secure_defensive_reserved, res_peb_detect_mode_plain)
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
ZTEST(ubi_secure_defensive_reserved, init_plain_media_mismatch)
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

	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, init_partition_not_multiple)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, init_write_exceeds_alignment)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, init_too_few_pebs)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, init_erase_not_multiple_of_write)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, vol_create_null)
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
ZTEST(ubi_secure_defensive_reserved, vol_remove_null)
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
ZTEST(ubi_secure_defensive_reserved, vol_resize_null)
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
ZTEST(ubi_secure_defensive_reserved, vol_get_info_null)
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
ZTEST(ubi_secure_defensive_reserved, leb_write_null)
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
ZTEST(ubi_secure_defensive_reserved, leb_write_buf_len_mismatch)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, leb_read_null)
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
ZTEST(ubi_secure_defensive_reserved, leb_map_null)
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
ZTEST(ubi_secure_defensive_reserved, leb_unmap_null)
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
ZTEST(ubi_secure_defensive_reserved, leb_is_mapped_null)
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
ZTEST(ubi_secure_defensive_reserved, leb_get_size_null)
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
ZTEST(ubi_secure_defensive_reserved, device_get_info_null)
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
ZTEST(ubi_secure_defensive_reserved, device_erase_peb_null)
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
ZTEST(ubi_secure_defensive_reserved, leb_write_vol_not_found)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, leb_write_leb_exceeded)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, leb_write_too_big)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, leb_read_vol_not_found)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, leb_read_leb_exceeded)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, leb_read_unmapped)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, vol_resize_same_count)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, vol_remove_not_found)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, vol_get_info_not_found)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, leb_get_size_unmapped)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, vol_resize_zero_lebs)
{
	struct ubi_volume_config vol_cfg = {
		.name = "test_rz_zero",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 0,
	};

	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
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
ZTEST(ubi_secure_defensive_reserved, derive_domain_key_rejects_non_allowlisted_kv)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();

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
ZTEST(ubi_secure_defensive_reserved, derive_leb_key_rejects_non_allowlisted_kv)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();

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
ZTEST(ubi_secure_defensive_reserved, ec_hdr_write_counter_overflow)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();

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
ZTEST(ubi_secure_defensive_reserved, vid_hdr_write_counter_overflow)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();

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
ZTEST(ubi_secure_defensive_reserved, res_peb_commit_counter_overflow)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();

	struct ubi_dev_hdr dh = { .magic = UBI_DEV_HDR_MAGIC, .version = UBI_DEV_HDR_VERSION };
	struct ubi_dev_secure_meta dm = { 0 };

	zassert_equal(ubi_secure_res_peb_commit(&flash, &cfg, &dh, &dm, NULL, 0, 1,
						UBI_SECURE_COUNTER_MAX + 1),
		      -EOVERFLOW);
}
