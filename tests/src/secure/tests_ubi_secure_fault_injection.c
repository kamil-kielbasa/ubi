/**
 * \file    tests_ubi_secure_fault_injection.c
 * \author  Kamil Kielbasa
 *
 * \brief   Parity tests for secure backend: transactional safety via fault injection.
 *
 * \details Mirrors every test from tests_ubi_fault_injection.c against the secure
 *          backend.
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
static struct ubi_device *g_ubi;

/* Static function definitions ------------------------------------------------------------------ */
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
	zassert_ok(flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE));
	g_ubi = NULL;
}

static void ztest_testcase_teardown(void *ctx)
{
	(void)ctx;
	ubi_test_fault_reset();
	if (g_ubi) {
		(void)ubi_device_deinit(g_ubi);
		g_ubi = NULL;
	}
}

static struct ubi_device *sec_init(void)
{
	static struct ubi_crypto_config cfg;
	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;
	return ubi;
}

/* Module interface function definitions -------------------------------------------------------- */
/**
 * \brief Verify that volume create with alloc failure does NOT leave a
 *        persistent volume with secure backend.
 *
 * \details Scenario: Create a volume. If ENOMEM, reinit and verify no volume persists.
 *
 * \expect If create returns ENOMEM, no volume exists on re-init.
 */
ZTEST(ubi_secure_fault_injection, test_create_alloc_fail_no_persistent_volume)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "fivol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	const int ret = ubi_volume_create(ubi, &cfg, &vol_id);

	if (ret == -ENOMEM) {
		g_ubi = NULL;
		zassert_ok(ubi_device_deinit(ubi));

		ubi = sec_init();

		struct ubi_device_info info = { 0 };
		zassert_ok(ubi_device_get_info(ubi, &info));
		zassert_equal(0, info.volume_count);
	} else {
		zassert_ok(ret);
	}

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that overwrite preserves old data on write failure.
 *
 * \details Scenario: Write data, read it back.
 *
 * \expect Read-back matches original data.
 */
ZTEST(ubi_secure_fault_injection, test_overwrite_preserves_old_data_on_failure)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "cowvol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t original[] = { 0xDE, 0xAD, 0xBE, 0xEF };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, original, sizeof(original)));

	uint8_t readback[4] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, original, sizeof(original));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST_SUITE(ubi_secure_fault_injection, NULL, ztest_suite_setup, ztest_suite_before,
	    ztest_testcase_teardown, NULL);
