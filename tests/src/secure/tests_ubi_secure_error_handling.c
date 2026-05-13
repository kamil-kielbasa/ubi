/**
 * \file    tests_ubi_secure_error_handling.c
 * \author  Kamil Kielbasa
 *
 * \brief   Parity tests for secure backend: API error handling and edge cases.
 *
 * \details Mirrors every test from tests_ubi_error_handling.c against the
 *          secure backend to ensure identical contract enforcement when
 *          crypto_config is provided.
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
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/sys/crc.h>

#include <errno.h>
#include <string.h>

#include "ubi_api_contract.h"

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
static struct ubi_device *g_ubi;

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
	g_ubi = NULL;
}

static void ztest_testcase_teardown(void *ctx)
{
	(void)ctx;
	if (g_ubi) {
		(void)ubi_device_deinit(g_ubi);
		g_ubi = NULL;
	}
}

static struct ubi_device *sec_init(void)
{
	struct ubi_device *const ubi = ubi_test_secure_init(&flash);

	g_ubi = ubi;
	return ubi;
}

/* Module interface function definitions -------------------------------------------------------- */
/**
 * \brief Verify that ubi_device_init() rejects a NULL flash descriptor.
 *
 * \details Scenario: Call ubi_device_init() with flash=NULL and a valid crypto config.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling, init_null_mtd)
{
	static struct ubi_crypto_config cfg;
	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_equal(-EINVAL, ubi_device_init(NULL, &cfg, &ubi));
}

/**
 * \brief Verify that ubi_device_init() rejects a NULL output pointer.
 *
 * \details Scenario: Call ubi_device_init() with ubi=NULL and a valid crypto config.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling, init_null_ubi)
{
	static struct ubi_crypto_config cfg;
	cfg = ubi_test_mock_crypto_config();

	zassert_equal(-EINVAL, ubi_device_init(&flash, &cfg, NULL));
}

/**
 * \brief Verify that ubi_device_deinit() rejects a NULL device pointer.
 *
 * \details Scenario: Call ubi_device_deinit() with NULL.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling, deinit_null)
{
	zassert_equal(-EINVAL, ubi_device_deinit(NULL));
}

/**
 * \brief Verify that ubi_device_get_info() rejects a NULL device pointer.
 *
 * \details Scenario: Call ubi_device_get_info() with ubi=NULL.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling, get_info_null_device)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_get_info_null_device(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that ubi_device_get_info() rejects a NULL info buffer.
 *
 * \details Scenario: Initialize a secure device, then call ubi_device_get_info()
 *          with info=NULL.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling, get_info_null_info)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_get_info_null_info(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that ubi_device_erase_peb() rejects a NULL device pointer.
 *
 * \details Scenario: Call ubi_device_erase_peb() with NULL.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling, erase_peb_null)
{
	zassert_equal(-EINVAL, ubi_device_erase_peb(NULL));
}

ZTEST_SUITE(ubi_secure_error_handling, NULL, ztest_suite_setup, ztest_suite_before,
	    ztest_testcase_teardown, NULL);
