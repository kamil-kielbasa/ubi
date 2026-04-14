/**
 * \file    tests_ubi_secure_attach.c
 *
 * \brief   Tests for secure backend mode detection, format, and attach.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files ------------------------------------------------------------------------------- */
#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>

#include "ubi_test_fixture.h"
#include "ubi_test_secure_fixture.h"

#include <psa/crypto.h>

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------ */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* Static variables ---------------------------------------------------------------------------- */

static struct ubi_mtd mtd = { 0 };

/* Suite setup / teardown ---------------------------------------------------------------------- */

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
	ubi_test_partition_force_release_all();
	zassert_ok(flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE));
}

/* Tests --------------------------------------------------------------------------------------- */

static enum ubi_crypto_rollback_verdict
mock_reject_freshness(const struct ubi_crypto_freshness *freshness, void *user_data)
{
	ARG_UNUSED(freshness);
	ARG_UNUSED(user_data);
	return UBI_CRYPTO_ROLLBACK_REJECT;
}

/**
 * \brief Blank flash + secure config → format succeeds.
 */
ZTEST(ubi_secure_attach, test_format_blank_device)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));
	zassert_not_null(ubi);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Format + deinit + re-init → attach succeeds.
 */
ZTEST(ubi_secure_attach, test_attach_after_format)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	/* First init: format on blank. */
	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));
	zassert_not_null(ubi);
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Second init: attach to existing secure media. */
	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));
	zassert_not_null(ubi);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Plain-formatted media + secure config → mode mismatch (-EPROTO).
 */
ZTEST(ubi_secure_attach, test_plain_then_secure_mismatch)
{
	struct ubi_device *ubi = NULL;

	/* Format as plain. */
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Try to attach as secure → mismatch. */
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	int ret = ubi_device_init(&mtd, &cfg, &ubi);

	zassert_equal(ret, -EPROTO, "Expected -EPROTO for mode mismatch, got %d", ret);
	zassert_is_null(ubi);
}

/**
 * \brief Secure-formatted media + plain config → mode mismatch.
 */
ZTEST(ubi_secure_attach, test_secure_then_plain_mismatch)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	/* Format as secure. */
	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));
	zassert_not_null(ubi);
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Try to attach as plain → mismatch. */
	const int ret = ubi_device_init(&mtd, NULL, &ubi);

	/* Plain backend sees non-standard magic and fails. */
	zassert_true(ret != 0, "Expected error for secure→plain mismatch, got %d", ret);
	zassert_is_null(ubi);
}

/**
 * \brief Freshness check rejecting → attach fails with -EACCES.
 */
ZTEST(ubi_secure_attach, test_freshness_reject)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	/* Format on blank. */
	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Override check_freshness to reject. */
	cfg.check_freshness = mock_reject_freshness;

	int ret = ubi_device_init(&mtd, &cfg, &ubi);
	zassert_equal(ret, -EACCES, "Expected -EACCES for rollback rejection, got %d", ret);
	zassert_is_null(ubi);
}

/**
 * \brief NULL callbacks in crypto config → -EINVAL.
 */
ZTEST(ubi_secure_attach, test_null_callback_rejected)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	cfg.get_key_id = NULL;
	int ret = ubi_device_init(&mtd, &cfg, &ubi);
	zassert_equal(ret, -EINVAL, "Expected -EINVAL for NULL get_key_id, got %d", ret);
	zassert_is_null(ubi);
}

/**
 * \brief Empty allowlist → -EINVAL.
 */
ZTEST(ubi_secure_attach, test_empty_allowlist_rejected)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	cfg.policy.allowed_key_versions_len = 0;
	int ret = ubi_device_init(&mtd, &cfg, &ubi);
	zassert_equal(ret, -EINVAL, "Expected -EINVAL for empty allowlist, got %d", ret);
	zassert_is_null(ubi);
}

/**
 * \brief Write key version not in allowlist → -EINVAL.
 */
ZTEST(ubi_secure_attach, test_write_key_version_not_in_allowlist)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	cfg.policy.requested_write_key_version = 99;
	int ret = ubi_device_init(&mtd, &cfg, &ubi);
	zassert_equal(ret, -EINVAL, "Expected -EINVAL for bad write kv, got %d", ret);
	zassert_is_null(ubi);
}

/* Suite registration -------------------------------------------------------------------------- */

ZTEST_SUITE(ubi_secure_attach, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);
