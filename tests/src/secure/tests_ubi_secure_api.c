/**
 * \file    tests_ubi_secure_api.c
 * \author  Kamil Kielbasa
 *
 * \brief   Tests for secure API types, init rejection, and crypto_config validation.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>
#include <ubi_secure.h>
#include <ubi_test.h>

/* Test fixtures: */
#include "ubi_test_fixture.h"
#include "ubi_test_secure_fixture.h"

/* Zephyr headers: */
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <psa/crypto.h>

/* Standard library headers: */
#include <errno.h>
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
}

/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_secure_api, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);

/**
 * \brief Secure init succeeds on blank flash (format-on-first-use).
 *
 * \details Scenario: Initialize the secure backend on a fully erased partition.
 *          The backend must detect blank media and format it in secure mode.
 *
 * \expect ubi_device_init returns 0, device handle is non-NULL.
 */
ZTEST(ubi_secure_api, secure_format_on_blank)
{
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_not_null(ubi);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Plain init still works when secure types are included.
 *
 * \details Scenario: Initialize the plain backend (secure_cfg == NULL) while
 *          CONFIG_UBI_SECURE=y is enabled in the build. Validates that
 *          including ubi_secure.h does not break the plain code path.
 *
 * \expect ubi_device_init returns 0, device info shows > 0 PEBs.
 */
ZTEST(ubi_secure_api, plain_init_unaffected_by_secure_types)
{
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	zassert_not_null(ubi);

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.total_peb_count > 0);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Crypto type sizes and layout are sane.
 *
 * \details Scenario: Verify struct sizes, enum ranges, and verdict values for all
 *          public crypto types defined in ubi_secure.h.
 *
 * \expect freshness is 16 bytes, event types span 0..9, verdict enums
 *           match their documented values.
 */
ZTEST(ubi_secure_api, crypto_type_sizes)
{
	zassert_equal(sizeof(struct ubi_secure_freshness), 16,
		      "freshness should be 16 bytes (2 x uint64_t)");

	zassert_true(sizeof(struct ubi_secure_policy) > 0);
	zassert_true(sizeof(struct ubi_secure_event) > 0);
	zassert_true(sizeof(struct ubi_secure_config) > 0);

	/* enum ubi_secure_event_type — exhaustive value pinning. */
	zassert_equal(UBI_SECURE_EVENT_AUTH_FAILURE, 0);
	zassert_equal(UBI_SECURE_EVENT_FORMAT_VIOLATION, 1);
	zassert_equal(UBI_SECURE_EVENT_KEY_VERSION_NOT_ALLOWLISTED, 2);
	zassert_equal(UBI_SECURE_EVENT_KEY_VERSION_UNAVAILABLE, 3);
	zassert_equal(UBI_SECURE_EVENT_ROLLBACK_POLICY_MISMATCH, 4);
	zassert_equal(UBI_SECURE_EVENT_FRESHNESS_SYNC_FAILURE, 5);
	zassert_equal(UBI_SECURE_EVENT_RNG_FAILURE, 6);
	zassert_equal(UBI_SECURE_EVENT_KEY_ROTATE_SOON, 7);
	zassert_equal(UBI_SECURE_EVENT_KEY_ROTATE_NOW, 8);
	zassert_equal(UBI_SECURE_EVENT_KEY_RETIRABLE, 9);

	/* enum ubi_secure_rollback_verdict — exhaustive value pinning. */
	zassert_equal(UBI_SECURE_ROLLBACK_ACCEPT, 0);
	zassert_equal(UBI_SECURE_ROLLBACK_REJECT, 1);

	/* enum ubi_secure_event_verdict — exhaustive value pinning. */
	zassert_equal(UBI_SECURE_EVENT_CONTINUE, 0);
	zassert_equal(UBI_SECURE_EVENT_ENTER_READ_ONLY, 1);
}

/**
 * \brief get_write_active_key_version rejects NULL arguments.
 *
 * \details Scenario: Both the device handle and the output pointer are required.
 *
 * \expect -EINVAL when either argument is NULL.
 */
ZTEST(ubi_secure_api, get_write_active_kv_null_args)
{
	uint8_t kv = 0xAA;
	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
	struct ubi_device *ubi = NULL;

	zassert_equal(ubi_secure_key_get_active_version(NULL, &kv), -EINVAL);

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_equal(ubi_secure_key_get_active_version(ubi, NULL), -EINVAL);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief get_write_active_key_version returns -ENOTSUP on plain-mode device.
 *
 * \expect -ENOTSUP when device was initialized with secure_cfg=NULL.
 *
 * \details Scenario: Initialize device with secure_cfg=NULL (plain mode). Call
 *          ubi_secure_key_get_active_version with the device handle and an output
 *          buffer. Deinit.
 */
ZTEST(ubi_secure_api, get_write_active_kv_plain_mode)
{
	struct ubi_device *ubi = NULL;
	uint8_t kv = 0xAA;

	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	zassert_equal(ubi_secure_key_get_active_version(ubi, &kv), -ENOTSUP);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief get_write_active_key_version returns the formatted key version.
 *
 * \details Scenario: After format-on-blank with requested kv=1, the getter must
 *          return 1. After reattach with rotation to kv=2 in the
 *          allowlist, the getter must return 2.
 *
 * \expect kv == 1 after format; kv == 2 after rotation reattach.
 */
ZTEST(ubi_secure_api, get_write_active_kv_after_format_and_rotation)
{
	struct ubi_secure_config cfg = ubi_test_mock_secure_config();
	struct ubi_device *ubi = NULL;
	uint8_t kv = 0;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_secure_key_get_active_version(ubi, &kv));
	zassert_equal(kv, 1, "expected formatted kv=1, got %u", kv);
	zassert_ok(ubi_device_deinit(ubi));

	/* Reattach with kv=2 requested and allowed. */
	static const uint8_t allowed_v12[] = { 1, 2 };

	cfg.policy.requested_write_key_version = 2;
	cfg.policy.allowed_key_versions = allowed_v12;
	cfg.policy.allowed_key_versions_len = 2;

	ubi = NULL;
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	kv = 0;
	zassert_ok(ubi_secure_key_get_active_version(ubi, &kv));
	zassert_equal(kv, 2, "expected post-rotation kv=2, got %u", kv);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Reserved-generation fit guard rejects too-small erase blocks.
 *
 * \details Scenario: One secure reserved generation must fit inside one reserved PEB:
 *          erase_block_size >= UBI_SECURE_DEV_HDR_SIZE +
 *                              CONFIG_UBI_MAX_NR_OF_VOLUMES * UBI_SECURE_VOL_HDR_SIZE
 *          (i.e. 96 + 96 * N).  Init must reject any geometry that violates
 *          this bound.
 *
 * \expect ubi_device_init returns -EINVAL when erase_block_size is below
 *           the fit threshold but above all earlier sanity bounds.
 */
ZTEST(ubi_secure_api, reserved_generation_fit_guard_rejects_small_eb)
{
	const size_t fit_threshold = 96U + (size_t)CONFIG_UBI_MAX_NR_OF_VOLUMES * 96U;

	/* Pick an erase block above the LEB-overhead minimum (208 bytes) but
	 * below the fit threshold.  512 satisfies that for typical
	 * CONFIG_UBI_MAX_NR_OF_VOLUMES values (default 10 -> 1056). */
	const size_t small_eb = 512U;

	if (small_eb >= fit_threshold) {
		ztest_test_skip();
		return;
	}

	struct ubi_flash_desc bad_flash = flash;

	bad_flash.erase_block_size = small_eb;
	bad_flash.write_block_size = 4U;

	const struct ubi_secure_config cfg = ubi_test_mock_secure_config();
	struct ubi_device *ubi = NULL;

	const int ret = ubi_device_init(&bad_flash, &cfg, &ubi);

	zassert_equal(ret, -EINVAL,
		      "expected -EINVAL for erase_block_size %zu < fit threshold %zu, got %d",
		      small_eb, fit_threshold, ret);
	zassert_is_null(ubi);
}
