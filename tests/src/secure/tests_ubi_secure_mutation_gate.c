/**
 * \file    tests_ubi_secure_mutation_gate.c
 * \author  Kamil Kielbasa
 *
 * \brief   Parity tests for secure backend: central mutation gate.
 *
 * \details Mirrors every test from tests_ubi_mutation_gate.c against the secure
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
#include <zephyr/storage/flash_map.h>

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
	ubi_test_secure_suite_setup_impl(&flash);
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
#if defined(CONFIG_UBI_TEST_API_ENABLE)
		ubi_test_set_write_shutdown(g_ubi, false);
#endif
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
 * \brief Verify that write shutdown blocks all public mutators with secure
 *        backend.
 *
 * \details Scenario: Enable write shutdown. Verify all mutators return -EROFS and all
 *          read-only operations still work. Disable and verify recovery.
 *
 * \expect All mutators return -EROFS. Readers return valid data.
 */
ZTEST(ubi_secure_mutation_gate, test_write_shutdown_blocks_all_mutators)
{
#if defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "gatevol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[16] = {
		0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
		0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
	};
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	for (int i = 0; i < 5; ++i) {
		(void)ubi_device_erase_peb(ubi);
	}

	ubi_test_set_write_shutdown(ubi, true);

	const struct ubi_volume_config new_cfg = {
		.name = "blocked",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int blocked_id = -1;
	zassert_equal(-EROFS, ubi_volume_create(ubi, &new_cfg, &blocked_id));

	const struct ubi_volume_config resize_cfg = {
		.name = "gatevol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 3,
	};
	zassert_equal(-EROFS, ubi_volume_resize(ubi, vol_id, &resize_cfg));
	zassert_equal(-EROFS, ubi_volume_remove(ubi, vol_id));

	const uint8_t new_data[16] = { 0xAA };
	zassert_equal(-EROFS, ubi_leb_write(ubi, vol_id, 1, new_data, sizeof(new_data)));
	zassert_equal(-EROFS, ubi_leb_map(ubi, vol_id, 1));
	zassert_equal(-EROFS, ubi_leb_unmap(ubi, vol_id, 0));
	zassert_equal(-EROFS, ubi_device_erase_peb(ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count);

	uint8_t readback[16] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, data, sizeof(data));

	bool is_mapped = false;
	zassert_ok(ubi_leb_is_mapped(ubi, vol_id, 0, &is_mapped));
	zassert_true(is_mapped);

	size_t size = 0;
	zassert_ok(ubi_leb_get_size(ubi, vol_id, 0, &size));
	zassert_equal(sizeof(data), size);

	ubi_test_set_write_shutdown(ubi, false);
	zassert_ok(ubi_leb_write(ubi, vol_id, 1, new_data, sizeof(new_data)));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}

ZTEST_SUITE(ubi_secure_mutation_gate, NULL, ztest_suite_setup, ztest_suite_before,
	    ztest_testcase_teardown, NULL);
