/**
 * \file    tests_ubi_secure_api.c
 *
 * \brief   Tests for secure API types, init rejection, and crypto_config validation.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files ------------------------------------------------------------------------------- */
#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>

#include "ubi_test_fixture.h"
#include "ubi_test_secure_fixture.h"

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

	return NULL;
}

static void ztest_suite_before(void *ctx)
{
	ARG_UNUSED(ctx);
	ubi_test_partition_force_release_all();
	zassert_ok(flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE));
}

/* Tests --------------------------------------------------------------------------------------- */

/**
 * \brief Secure init returns -ENOTSUP when the secure backend is not yet implemented.
 *
 * Even with CONFIG_UBI_CRYPTO=y, the backend ops are not registered yet.
 * This test verifies the facade correctly rejects the request.
 */
ZTEST(ubi_secure_api, test_secure_init_returns_enotsup)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	int ret = ubi_device_init(&mtd, &cfg, &ubi);
	zassert_equal(ret, -ENOTSUP, "Expected -ENOTSUP, got %d", ret);
	zassert_is_null(ubi);
}

/**
 * \brief Plain init still works when secure types are included.
 *
 * Ensures that including ubi_crypto.h and having CONFIG_UBI_CRYPTO=y does
 * not break the plain backend path (crypto_cfg == NULL).
 */
ZTEST(ubi_secure_api, test_plain_init_unaffected_by_secure_types)
{
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	zassert_not_null(ubi);

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.total_peb_count > 0);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Crypto type sizes and layout are sane.
 *
 * Compile-time verification that the types from ubi_crypto.h have sensible
 * sizes and alignment.
 */
ZTEST(ubi_secure_api, test_crypto_type_sizes)
{
	zassert_equal(sizeof(struct ubi_crypto_freshness), 16,
		      "freshness should be 16 bytes (2 x uint64_t)");

	zassert_true(sizeof(struct ubi_crypto_policy) > 0);
	zassert_true(sizeof(struct ubi_crypto_event) > 0);
	zassert_true(sizeof(struct ubi_crypto_config) > 0);

	/* Event type enum spans 0..9 */
	zassert_equal(UBI_CRYPTO_EVENT_AUTH_FAILURE, 0);
	zassert_equal(UBI_CRYPTO_EVENT_KEY_RETIRABLE, 9);

	/* Verdict enums */
	zassert_equal(UBI_CRYPTO_ROLLBACK_ACCEPT, 0);
	zassert_equal(UBI_CRYPTO_ROLLBACK_REJECT, 1);
	zassert_equal(UBI_CRYPTO_EVENT_CONTINUE, 0);
	zassert_equal(UBI_CRYPTO_EVENT_ENTER_READ_ONLY, 1);
}

/* Suite registration -------------------------------------------------------------------------- */

ZTEST_SUITE(ubi_secure_api, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);
