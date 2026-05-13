/**
 * \file    tests_ubi_secure_attach.c
 * \author  Kamil Kielbasa
 *
 * \brief   Tests for secure backend mode detection, format, and attach.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

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
static void *ztest_suite_setup(void)
{
	ubi_test_secure_suite_setup_impl(&flash);
	return NULL;
}

static void ztest_suite_before(void *ctx)
{
	(void)ctx;
	ubi_test_secure_before_impl();
}

/* Module interface function definitions -------------------------------------------------------- */
static enum ubi_crypto_rollback_verdict
mock_reject_freshness(const struct ubi_crypto_freshness *freshness, void *user_data)
{
	(void)freshness;
	(void)user_data;
	return UBI_CRYPTO_ROLLBACK_REJECT;
}

/**
 * \brief Blank flash + secure config → format succeeds.
 *
 * \details Scenario: Erase the full partition, then call ubi_device_init with a valid
 *          crypto_cfg. The backend detects blank media and formats in secure mode.
 *
 * \expect ubi_device_init returns 0, device handle is non-NULL.
 */
ZTEST(ubi_secure_attach, format_blank_device)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_not_null(ubi);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Format + deinit + re-init → attach succeeds.
 *
 * \details Scenario: Format a blank device in secure mode, deinit, then re-init.
 *          The second init must attach to the existing secure metadata.
 *
 * \expect Both ubi_device_init calls return 0.
 */
ZTEST(ubi_secure_attach, attach_after_format)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	/* First init: format on blank. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_not_null(ubi);
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Second init: attach to existing secure media. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_not_null(ubi);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Plain-formatted media + secure config → mode mismatch (-EPROTO).
 *
 * \details Scenario: Format a device as plain (crypto_cfg == NULL), then attempt
 *          to re-init with a secure crypto_cfg. The backend must reject
 *          the mixed-mode attach.
 *
 * \expect Second ubi_device_init returns -EPROTO, device handle is NULL.
 */
ZTEST(ubi_secure_attach, plain_then_secure_mismatch)
{
	struct ubi_device *ubi = NULL;

	/* Format as plain. */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	zassert_not_null(ubi);
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Try to attach as secure → mismatch. */
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	int ret = ubi_device_init(&flash, &cfg, &ubi);

	zassert_equal(ret, -EPROTO, "Expected -EPROTO for mode mismatch, got %d", ret);
	zassert_is_null(ubi);
}

/**
 * \brief Secure-formatted media + plain config → mode mismatch.
 *
 * \details Scenario: Format a device in secure mode, then attempt to re-init with
 *          crypto_cfg == NULL. The plain backend must reject the non-standard
 *          on-flash magic.
 *
 * \expect Second ubi_device_init returns non-zero, device handle is NULL.
 */
ZTEST(ubi_secure_attach, secure_then_plain_mismatch)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	/* Format as secure. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_not_null(ubi);
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Try to attach as plain → mismatch. */
	const int ret = ubi_device_init(&flash, NULL, &ubi);

	/* Plain backend sees non-standard magic and fails. */
	zassert_true(ret != 0, "Expected error for secure→plain mismatch, got %d", ret);
	zassert_is_null(ubi);
}

/**
 * \brief Freshness check rejecting → attach fails with -EACCES.
 *
 * \details Scenario: Format a device, deinit, then re-init with a check_freshness
 *          callback that always returns UBI_CRYPTO_ROLLBACK_REJECT.
 *
 * \expect ubi_device_init returns -EACCES, device handle is NULL.
 */
ZTEST(ubi_secure_attach, freshness_reject)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	/* Format on blank. */
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Override check_freshness to reject. */
	cfg.check_freshness = mock_reject_freshness;

	int ret = ubi_device_init(&flash, &cfg, &ubi);
	zassert_equal(ret, -EACCES, "Expected -EACCES for rollback rejection, got %d", ret);
	zassert_is_null(ubi);
}

/**
 * \brief NULL callbacks in crypto config → -EINVAL.
 *
 * \details Scenario: Pass a crypto_cfg with get_key_id set to NULL.
 *
 * \expect ubi_device_init returns -EINVAL, device handle is NULL.
 */
ZTEST(ubi_secure_attach, null_callback_rejected)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	cfg.get_key_id = NULL;
	int ret = ubi_device_init(&flash, &cfg, &ubi);
	zassert_equal(ret, -EINVAL, "Expected -EINVAL for NULL get_key_id, got %d", ret);
	zassert_is_null(ubi);
}

/**
 * \brief Empty allowlist → -EINVAL.
 *
 * \details Scenario: Pass a crypto_cfg with allowed_key_versions_len == 0.
 *
 * \expect ubi_device_init returns -EINVAL, device handle is NULL.
 */
ZTEST(ubi_secure_attach, empty_allowlist_rejected)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	cfg.policy.allowed_key_versions_len = 0;
	int ret = ubi_device_init(&flash, &cfg, &ubi);
	zassert_equal(ret, -EINVAL, "Expected -EINVAL for empty allowlist, got %d", ret);
	zassert_is_null(ubi);
}

/**
 * \brief Write key version not in allowlist → -EINVAL.
 *
 * \details Scenario: Pass a crypto_cfg whose requested_write_key_version (99)
 *          is absent from the allowed_key_versions array.
 *
 * \expect ubi_device_init returns -EINVAL, device handle is NULL.
 */
ZTEST(ubi_secure_attach, write_key_version_not_in_allowlist)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	cfg.policy.requested_write_key_version = 99;
	int ret = ubi_device_init(&flash, &cfg, &ubi);
	zassert_equal(ret, -EINVAL, "Expected -EINVAL for bad write kv, got %d", ret);
	zassert_is_null(ubi);
}

/**
 * \brief Allowlist with duplicate entries — rejected.
 *
 * \details Scenario: Each key version slot tracks independent refcount and budget
 *          bookkeeping; a duplicate entry would waste a slot and create
 *          ambiguity in operator-visible state.  `validate_crypto_cfg`
 *          rejects duplicates with -EINVAL before any flash access.
 *          Audit §10.1 (former #7).
 *
 * \expect ubi_device_init returns -EINVAL, device handle is NULL.
 */
ZTEST(ubi_secure_attach, allowlist_duplicates_rejected)
{
	static const uint8_t allowed_with_dup[] = { 1, 1, 2 };
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	cfg.policy.allowed_key_versions = allowed_with_dup;
	cfg.policy.allowed_key_versions_len = sizeof(allowed_with_dup);
	cfg.policy.requested_write_key_version = 1;

	int ret = ubi_device_init(&flash, &cfg, &ubi);
	zassert_equal(ret, -EINVAL, "Expected -EINVAL for allowlist with duplicates, got %d", ret);
	zassert_is_null(ubi);
}

/**
 * \brief Reattach with `requested_write_key_version` below the on-flash
 *        `write_active_key_version` — rejected (downgrade / wrap-around guard).
 *
 * \details Scenario: Key versions are monotonically non-decreasing for the lifetime
 *          of the device.  A reattach that requests a lower kv would reuse
 *          a uint8_t slot that may already have been retired and would
 *          invalidate the freshness and budget invariants built around
 *          monotonic key progression — including a hard wrap-around at the
 *          uint8_t boundary (255 → 0).  `secure_attach` rejects with
 *          -EINVAL after authenticating the on-flash device header.
 *          Audit §4.5 ("Zakaz wrap-around key_version").
 *
 * \expect First init (kv=2) succeeds; second init with kv=1 returns
 *           -EINVAL and leaves the handle NULL.
 */
ZTEST(ubi_secure_attach, requested_write_kv_downgrade_rejected)
{
	static const uint8_t allowed[] = { 1, 2 };

	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	cfg.policy.allowed_key_versions = allowed;
	cfg.policy.allowed_key_versions_len = sizeof(allowed);
	cfg.policy.requested_write_key_version = 2;

	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_not_null(ubi);
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	/* Reattach with a lower requested_write_key_version — must fail. */
	cfg.policy.requested_write_key_version = 1;
	int ret = ubi_device_init(&flash, &cfg, &ubi);
	zassert_equal(ret, -EINVAL, "Expected -EINVAL for kv downgrade, got %d", ret);
	zassert_is_null(ubi);

	/* Sanity: same kv (=2) still attaches successfully. */
	cfg.policy.requested_write_key_version = 2;
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_not_null(ubi);
	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST_SUITE(ubi_secure_attach, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);
