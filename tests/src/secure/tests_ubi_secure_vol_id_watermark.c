/**
 * \file    tests_ubi_secure_vol_id_watermark.c
 * \author  Kamil Kielbasa
 *
 * \brief   Parity tests for secure backend: persistent vol_id high-watermark.
 *
 * \details Mirrors every test from tests_ubi_vol_id_watermark.c against the
 *          secure backend.
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
 * \brief vol_id is not reused after remove within the same boot.
 *
 * \details Scenario: Create vol A (id=0), remove it, create vol B.
 *
 * \expect Vol B gets id > id_a.
 */
ZTEST(ubi_secure_vol_id_watermark, test_volume_id_not_reused_after_remove_same_boot)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config cfg_a = {
		.name = "vol_a",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int id_a = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg_a, &id_a));
	zassert_equal(0, id_a);

	zassert_ok(ubi_volume_remove(ubi, id_a));

	const struct ubi_volume_config cfg_b = {
		.name = "vol_b",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int id_b = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg_b, &id_b));
	zassert_true(id_b > id_a, "vol_id must not be reused (got %d, prev %d)", id_b, id_a);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief vol_id is not reused after remove + reinit (simulated reboot).
 *
 * \details Scenario: Create vol A, remove, deinit, reinit, create vol B.
 *
 * \expect Vol B gets id > id_a.
 */
ZTEST(ubi_secure_vol_id_watermark, test_volume_id_not_reused_after_remove_and_reinit)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config cfg_a = {
		.name = "vol_a",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int id_a = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg_a, &id_a));
	zassert_equal(0, id_a);

	zassert_ok(ubi_volume_remove(ubi, id_a));
	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));

	ubi = sec_init();

	const struct ubi_volume_config cfg_b = {
		.name = "vol_b",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int id_b = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg_b, &id_b));
	zassert_true(id_b > id_a, "vol_id must survive reboot (got %d, prev %d)", id_b, id_a);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Volume slot re-indexing after remove does not change vol_ids.
 *
 * \details Scenario: Create 3 volumes, remove middle, verify others keep ids.
 *
 * \expect After reinit, new volume gets id=3, not id=1.
 */
ZTEST(ubi_secure_vol_id_watermark, test_volume_slot_reindex_does_not_change_ids)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config cfg0 = {
		.name = "vol_0",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	const struct ubi_volume_config cfg1 = {
		.name = "vol_1",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	const struct ubi_volume_config cfg2 = {
		.name = "vol_2",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};

	int id0 = -1, id1 = -1, id2 = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg0, &id0));
	zassert_ok(ubi_volume_create(ubi, &cfg1, &id1));
	zassert_ok(ubi_volume_create(ubi, &cfg2, &id2));

	zassert_equal(0, id0);
	zassert_equal(1, id1);
	zassert_equal(2, id2);

	zassert_ok(ubi_volume_remove(ubi, id1));

	struct ubi_volume_config read_cfg = { 0 };
	size_t alloc = 0;
	zassert_ok(ubi_volume_get_info(ubi, id0, &read_cfg, &alloc));
	zassert_equal(0, strncmp("vol_0", read_cfg.name, strlen("vol_0")));

	zassert_ok(ubi_volume_get_info(ubi, id2, &read_cfg, &alloc));
	zassert_equal(0, strncmp("vol_2", read_cfg.name, strlen("vol_2")));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));

	ubi = sec_init();

	zassert_ok(ubi_volume_get_info(ubi, id0, &read_cfg, &alloc));
	zassert_ok(ubi_volume_get_info(ubi, id2, &read_cfg, &alloc));

	const struct ubi_volume_config cfg3 = {
		.name = "vol_3",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int id3 = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg3, &id3));
	zassert_equal(3, id3);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST_SUITE(ubi_secure_vol_id_watermark, NULL, ztest_suite_setup, ztest_suite_before,
	    ztest_testcase_teardown, NULL);
