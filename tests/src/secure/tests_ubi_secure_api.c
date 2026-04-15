/**
 * \file    tests_ubi_secure_api.c
 * \author  Kamil Kielbasa
 *
 * \brief   Tests for secure API types, init rejection, and crypto_config validation.
 *
 * \copyright Copyright (c) 2026
 */

/* --------------------------------------- Include files --------------------------------------- */
#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>

#include "ubi_test_fixture.h"
#include "ubi_test_secure_fixture.h"

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

#include <psa/crypto.h>

#include <errno.h>
#include <string.h>

/* -------------------------------------- Module defines --------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* ------------------------------------- Static variables -------------------------------------- */

static struct ubi_mtd mtd = { 0 };

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

	zassert_equal(psa_crypto_init(), PSA_SUCCESS, "psa_crypto_init failed");
	ubi_test_import_root_key();

	return NULL;
}

static void ztest_suite_before(void *ctx)
{
	ARG_UNUSED(ctx);
	ubi_test_partition_force_release_all();
	zassert_ok(flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE));
}

/* ------------------------------------------- Tests ------------------------------------------- */

/**
 * \brief Secure init succeeds on blank flash (format-on-first-use).
 *
 * \details Initialize the secure backend on a fully erased partition.
 *          The backend must detect blank media and format it in secure mode.
 *
 * \expected ubi_device_init returns 0, device handle is non-NULL.
 */
ZTEST(ubi_secure_api, test_secure_format_on_blank)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));
	zassert_not_null(ubi);
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Plain init still works when secure types are included.
 *
 * \details Initialize the plain backend (crypto_cfg == NULL) while
 *          CONFIG_UBI_CRYPTO=y is enabled in the build. Validates that
 *          including ubi_crypto.h does not break the plain code path.
 *
 * \expected ubi_device_init returns 0, device info shows > 0 PEBs.
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
 * \details Verify struct sizes, enum ranges, and verdict values for all
 *          public crypto types defined in ubi_crypto.h.
 *
 * \expected freshness is 16 bytes, event types span 0..9, verdict enums
 *           match their documented values.
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

/* ------------------------------------ Suite registration ------------------------------------- */

ZTEST_SUITE(ubi_secure_api, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);
