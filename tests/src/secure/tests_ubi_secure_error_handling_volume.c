/**
 * \file    tests_ubi_secure_error_handling_volume.c
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
 * \brief Verify that ubi_volume_create() rejects NULL parameters.
 *
 * \details Scenario: Call ubi_volume_create() with each of the three parameters
 *          (ubi, vol_cfg, vol_id) set to NULL individually.
 *
 * \expect Each call returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_create_null_params)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_volume_create_null_params(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that creating the same volume twice is idempotent.
 *
 * \details Scenario: Create a static volume named "idem". Call ubi_volume_create()
 *          again with the same configuration.
 *
 * \expect Both calls succeed. The returned vol_id is identical.
 *           ubi_device_get_info() reports volume_count=1.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_create_idempotent)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_volume_create_idempotent(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that creating a volume larger than available PEBs fails.
 *
 * \details Scenario: Query total_peb_count, then attempt to create a volume
 *          with leb_count = total_peb_count + 1.
 *
 * \expect ubi_volume_create() returns -ENOSPC.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_create_no_space)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_volume_create_no_space(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that removing a non-existent volume fails.
 *
 * \details Scenario: Initialize a secure device with no volumes. Attempt to remove
 *          vol_id=999.
 *
 * \expect ubi_volume_remove() returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_remove_nonexistent)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_volume_remove_nonexistent(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that querying info for a non-existent volume fails.
 *
 * \details Scenario: Initialize a secure device with no volumes. Call
 *          ubi_volume_get_info() for vol_id=999.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_get_info_nonexistent)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_volume_get_info_nonexistent(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that resizing a static volume is rejected.
 *
 * \details Scenario: Create a static volume with 2 LEBs. Attempt to resize
 *          it to 4 LEBs.
 *
 * \expect ubi_volume_resize() returns -ECANCELED.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_resize_static)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_volume_resize_static(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that resizing a volume to its current size is rejected.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs. Attempt to resize
 *          it to the same count.
 *
 * \expect ubi_volume_resize() returns -ECANCELED.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_resize_same_size)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_volume_resize_same_size(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that resizing a non-existent volume fails.
 *
 * \details Scenario: Initialize a secure device with no volumes. Attempt to resize
 *          vol_id=999.
 *
 * \expect ubi_volume_resize() returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_resize_nonexistent)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_volume_resize_nonexistent(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that shrinking a volume with mapped LEBs trims the excess.
 *
 * \details Scenario: Create a dynamic volume with 4 LEBs and write data to all four.
 *          Resize the volume down to 2 LEBs. Verify LEBs 0 and 1 are still
 *          accessible.
 *
 * \expect ubi_volume_resize() succeeds. LEBs 0..1 are readable with correct
 *           data. dirty_peb_count >= 2.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_resize_shrink_with_mapped_lebs)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "shrink",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 4,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xDE, 0xAD };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 1, data, sizeof(data)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 2, data, sizeof(data)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 3, data, sizeof(data)));

	struct ubi_volume_config new_cfg = cfg;

	new_cfg.leb_count = 2;
	zassert_ok(ubi_volume_resize(ubi, vol_id, &new_cfg));

	uint8_t rdata[2] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, data, sizeof(data));

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.dirty_peb_count >= 2);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that a removed volume can be recreated with a clean state.
 *
 * \details Scenario: Create a static volume, write data to LEB 0, then remove it.
 *          Recreate a new volume with the same name and config. Check the
 *          mapping state of LEB 0 in the new volume.
 *
 * \expect The new volume is created successfully. LEB 0 is not mapped.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_remove_and_recreate)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "rmcrt",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xDE, 0xAD };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_volume_remove(ubi, vol_id));

	int vol_id2 = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id2));

	bool mapped = true;

	zassert_ok(ubi_leb_is_mapped(ubi, vol_id2, 0, &mapped));
	zassert_false(mapped);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_volume_resize() rejects a NULL configuration pointer.
 *
 * \details Scenario: Create a dynamic volume. Call ubi_volume_resize() with
 *          vol_cfg=NULL.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_resize_null_config)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_volume_resize_null_config(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that ubi_volume_resize() fails when no volumes exist.
 *
 * \details Scenario: Initialize a secure device with no volumes. Attempt to resize
 *          vol_id=0.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_resize_no_volumes)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_volume_resize_no_volumes(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that ubi_volume_get_info() fails when no volumes exist.
 *
 * \details Scenario: Initialize a secure device with no volumes. Call
 *          ubi_volume_get_info() for vol_id=0.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_get_info_no_volumes)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_volume_get_info_no_volumes(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that ubi_volume_remove() fails when no volumes exist.
 *
 * \details Scenario: Initialize a secure device with no volumes. Attempt to remove
 *          vol_id=0.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_remove_no_volumes)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_volume_remove_no_volumes(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that creating a volume with an invalid type is rejected.
 *
 * \details Scenario: Call ubi_volume_create() with vol_type set to an invalid value.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_create_invalid_type)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_volume_create_invalid_type(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that creating a volume with leb_count == 0 is rejected.
 *
 * \details Scenario: Call ubi_volume_create() with leb_count set to 0.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_create_zero_lebs)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_volume_create_zero_lebs(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that resizing a volume to leb_count == 0 is rejected.
 *
 * \details Scenario: Call ubi_volume_resize() with the new leb_count set to 0.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_resize_zero_lebs_rejected)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_volume_resize_zero_lebs_rejected(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that writing to a static volume is allowed.
 *
 * \details Scenario: Create a static volume. Write data to LEB 0 and read it back.
 *
 * \expect Write and read-back succeed.
 */
ZTEST(ubi_secure_error_handling_volume, test_static_volume_write_allowed)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_static_volume_write_allowed(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that creating a volume with a duplicate name returns the
 *        existing volume's ID.
 *
 * \details Scenario: Create a volume "dup", then call ubi_volume_create again with
 *          the same name "dup".
 *
 * \expect Second call returns 0 with the same vol_id.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_create_duplicate_name)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "dup",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id_1 = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id_1));

	int vol_id_2 = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id_2));
	zassert_equal(vol_id_1, vol_id_2);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that creating a volume with the same name but different
 *        configuration returns -EEXIST.
 *
 * \details Scenario: Create a dynamic volume "dup2", then call ubi_volume_create with
 *          the same name but a different leb_count or type.
 *
 * \expect Returns -EEXIST.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_create_duplicate_name_different_config)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg1 = {
		.name = "dup2",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id_1 = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg1, &vol_id_1));

	const struct ubi_volume_config cfg2 = {
		.name = "dup2",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 3,
	};
	int vol_id_2 = -1;

	zassert_equal(-EEXIST, ubi_volume_create(ubi, &cfg2, &vol_id_2));

	const struct ubi_volume_config cfg3 = {
		.name = "dup2",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id_3 = -1;

	zassert_equal(-EEXIST, ubi_volume_create(ubi, &cfg3, &vol_id_3));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that creating a volume with an empty name returns -EINVAL.
 *
 * \details Scenario: Call ubi_volume_create() with an empty name.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_create_empty_name)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;

	zassert_equal(-EINVAL, ubi_volume_create(ubi, &cfg, &vol_id));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that creating a volume with a name filling the entire buffer
 *        (no NUL terminator) returns -EINVAL.
 *
 * \details Scenario: Fill cfg.name with non-NUL characters.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_create_name_no_nul)
{
	struct ubi_device *const ubi = sec_init();

	struct ubi_volume_config cfg = {
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};

	memset(cfg.name, 'A', UBI_VOLUME_NAME_MAX_LEN);

	int vol_id = -1;

	zassert_equal(-EINVAL, ubi_volume_create(ubi, &cfg, &vol_id));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that a volume with the maximum valid name length can be created.
 *
 * \details Scenario: Call ubi_volume_create() with a name occupying MAX_LEN-1 chars + NUL.
 *
 * \expect Create succeeds.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_create_name_max_valid)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_volume_create_name_max_valid(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that ubi_volume_resize() fails when expanding beyond
 *        available PEBs.
 *
 * \details Scenario: Create a volume, then resize it to exceed total PEB count.
 *
 * \expect Returns -ENOSPC.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_resize_expand_enospc)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_volume_resize_expand_enospc(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that volume_resize shrink preserves data on retained LEBs
 *        and the volume's leb_count is updated.
 *
 * \details Scenario: Create a dynamic volume with 4 LEBs, write to all, shrink to 2.
 *          Verify LEBs 0-1 are readable and leb_count == 2.
 *
 * \expect Resize succeeds. Data intact. volume_get_info reports 2.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_resize_shrink_trim)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "shrk",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 4,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	uint8_t wdata[64] = { 0 };

	memset(wdata, 0xBB, sizeof(wdata));

	for (size_t lnum = 0; lnum < 4; ++lnum) {
		zassert_ok(ubi_leb_write(ubi, vol_id, lnum, wdata, sizeof(wdata)));
		zassert_ok(ubi_device_erase_peb(ubi));
	}

	const struct ubi_volume_config shrink_cfg = {
		.name = "shrk",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};

	zassert_ok(ubi_volume_resize(ubi, vol_id, &shrink_cfg));

	struct ubi_volume_config after_cfg = { 0 };
	size_t alloc_lebs = 0;

	zassert_ok(ubi_volume_get_info(ubi, vol_id, &after_cfg, &alloc_lebs));
	zassert_equal(2, after_cfg.leb_count);

	uint8_t rdata[64] = { 0 };

	for (size_t lnum = 0; lnum < 2; ++lnum) {
		zassert_ok(ubi_leb_read(ubi, vol_id, lnum, 0, rdata, sizeof(rdata)));
		zassert_mem_equal(wdata, rdata, sizeof(wdata));
	}

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify volume_create with identical config returns existing vol_id.
 *
 * \details Scenario: Create a volume. Call create again with the same config.
 *
 * \expect Both succeed. vol_id identical. volume_count == 1.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_create_idempotent_returns_same_id)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "idem",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};

	int vol_id1 = -1;
	int vol_id2 = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id1));
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id2));
	zassert_equal(vol_id1, vol_id2, "Idempotent create should return same vol_id");

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count, "Only one volume should exist");

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify volume_create with same name but different config returns -EEXIST.
 *
 * \details Scenario: Create static vol "clash", then try dynamic vol "clash".
 *
 * \expect Returns -EEXIST.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_create_name_clash_different_config)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg1 = {
		.name = "clash",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg1, &vol_id));

	const struct ubi_volume_config cfg2 = {
		.name = "clash",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 4,
	};
	int vol_id2 = -1;

	zassert_equal(-EEXIST, ubi_volume_create(ubi, &cfg2, &vol_id2));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify volume_resize grow works and data is still readable.
 *
 * \details Scenario: Create dynamic volume (2 LEBs), write to LEB 0. Resize to 4.
 *          Verify old data intact. New LEBs usable.
 *
 * \expect Resize succeeds. Old data intact. New LEBs available.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_resize_grow_preserves_data)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_volume_resize_grow_preserves_data(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify volume_resize grow with insufficient PEBs returns -ENOSPC.
 *
 * \details Scenario: Create 2 volumes consuming most PEBs. Try to grow one beyond
 *          available capacity.
 *
 * \expect Resize returns -ENOSPC. Original volume unchanged.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_resize_grow_enospc)
{
	struct ubi_device *const ubi = sec_init();

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));

	const struct ubi_volume_config cfg1 = {
		.name = "big",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = info.total_peb_count - 4,
	};
	int vid1 = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg1, &vid1));

	const struct ubi_volume_config cfg2 = {
		.name = "small",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vid2 = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg2, &vid2));

	const struct ubi_volume_config grow = {
		.name = "small",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = info.total_peb_count,
	};

	zassert_equal(-ENOSPC, ubi_volume_resize(ubi, vid2, &grow));

	struct ubi_volume_config out_cfg = { 0 };
	size_t alloc = 0;

	zassert_ok(ubi_volume_get_info(ubi, vid2, &out_cfg, &alloc));
	zassert_equal(1, out_cfg.leb_count);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify volume remove followed by re-create with different config.
 *
 * \details Scenario: Create, remove, then create again with different type/leb_count.
 *
 * \expect Re-create succeeds. New volume is empty.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_remove_and_recreate_different_config)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg1 = {
		.name = "recycle",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id1 = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg1, &vol_id1));

	const uint8_t data[] = { 0x42 };

	zassert_ok(ubi_leb_write(ubi, vol_id1, 0, data, sizeof(data)));
	zassert_ok(ubi_volume_remove(ubi, vol_id1));

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));
	for (size_t i = 0; i < info.dirty_peb_count + 1; ++i) {
		zassert_ok(ubi_device_erase_peb(ubi));
	}

	const struct ubi_volume_config cfg2 = {
		.name = "recycle",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 3,
	};
	int vol_id2 = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg2, &vol_id2));

	bool is_mapped = true;

	zassert_ok(ubi_leb_is_mapped(ubi, vol_id2, 0, &is_mapped));
	zassert_false(is_mapped, "Re-created volume should be empty");

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify volume_get_info with detailed field checks.
 *
 * \details Scenario: Create a volume, write to 2 LEBs, then query via
 *          ubi_volume_get_info().
 *
 * \expect Config matches. alloc_lebs reflects mapped LEBs.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_get_info_detailed)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_volume_get_info_detailed(ubi);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Volume remove with nonexistent vol_id returns -ENOENT.
 *
 * \details Scenario: Create a volume so vol_count > 0. Try to remove vol_id=99.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_remove_wrong_vol_id)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "realvol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const int ret = ubi_volume_remove(ubi, 99);

	zassert_equal(ret, -ENOENT, "remove nonexistent vol_id should return -ENOENT");

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify volume_resize shrink preserves data on retained LEBs.
 *
 * \details Scenario: Create dynamic volume (4 LEBs), write to 0-1. Shrink to 2.
 *          Verify retained LEBs readable. Trimmed LEBs return -EACCES.
 *
 * \expect Resize succeeds. Old data preserved. Trimmed LEBs inaccessible.
 */
ZTEST(ubi_secure_error_handling_volume, test_volume_resize_shrink_preserves_data)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "shrink",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 4,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t d0[] = { 0xAA, 0xBB };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, d0, sizeof(d0)));

	const uint8_t d1[] = { 0xCC, 0xDD };

	zassert_ok(ubi_leb_write(ubi, vol_id, 1, d1, sizeof(d1)));

	const struct ubi_volume_config cfg2 = {
		.name = "shrink",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};

	zassert_ok(ubi_volume_resize(ubi, vol_id, &cfg2));

	uint8_t rb0[2] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rb0, sizeof(rb0)));
	zassert_mem_equal(rb0, d0, sizeof(d0));

	uint8_t rb1[2] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, rb1, sizeof(rb1)));
	zassert_mem_equal(rb1, d1, sizeof(d1));

	const uint8_t dummy[] = { 0xFF };

	zassert_equal(-EACCES, ubi_leb_write(ubi, vol_id, 2, dummy, sizeof(dummy)));
	zassert_equal(-EACCES, ubi_leb_write(ubi, vol_id, 3, dummy, sizeof(dummy)));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

ZTEST_SUITE(ubi_secure_error_handling_volume, NULL, ztest_suite_setup, ztest_suite_before,
	    ztest_testcase_teardown, NULL);
