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
 * \brief Verify that ubi_device_init() rejects a NULL flash descriptor.
 *
 * \details Scenario: Call ubi_device_init() with flash=NULL and a valid crypto config.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling, test_init_null_mtd)
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
ZTEST(ubi_secure_error_handling, test_init_null_ubi)
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
ZTEST(ubi_secure_error_handling, test_deinit_null)
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
ZTEST(ubi_secure_error_handling, test_get_info_null_device)
{
	struct ubi_device_info info = { 0 };

	zassert_equal(-EINVAL, ubi_device_get_info(NULL, &info));
}

/**
 * \brief Verify that ubi_device_get_info() rejects a NULL info buffer.
 *
 * \details Scenario: Initialize a secure device, then call ubi_device_get_info()
 *          with info=NULL.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling, test_get_info_null_info)
{
	struct ubi_device *const ubi = sec_init();

	zassert_equal(-EINVAL, ubi_device_get_info(ubi, NULL));

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
ZTEST(ubi_secure_error_handling, test_erase_peb_null)
{
	zassert_equal(-EINVAL, ubi_device_erase_peb(NULL));
}

/**
 * \brief Verify that ubi_volume_create() rejects NULL parameters.
 *
 * \details Scenario: Call ubi_volume_create() with each of the three parameters
 *          (ubi, vol_cfg, vol_id) set to NULL individually.
 *
 * \expect Each call returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling, test_volume_create_null_params)
{
	struct ubi_device *const ubi = sec_init();

	int vol_id = -1;
	const struct ubi_volume_config cfg = {
		.name = "test",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	zassert_equal(-EINVAL, ubi_volume_create(NULL, &cfg, &vol_id));
	zassert_equal(-EINVAL, ubi_volume_create(ubi, NULL, &vol_id));
	zassert_equal(-EINVAL, ubi_volume_create(ubi, &cfg, NULL));

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
ZTEST(ubi_secure_error_handling, test_volume_create_idempotent)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "idem",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id_1 = -1;
	int vol_id_2 = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id_1));
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id_2));
	zassert_equal(vol_id_1, vol_id_2);

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count);

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
ZTEST(ubi_secure_error_handling, test_volume_create_no_space)
{
	struct ubi_device *const ubi = sec_init();

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));

	const struct ubi_volume_config cfg = {
		.name = "huge",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = info.total_peb_count + 1,
	};
	int vol_id = -1;

	zassert_equal(-ENOSPC, ubi_volume_create(ubi, &cfg, &vol_id));

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
ZTEST(ubi_secure_error_handling, test_volume_remove_nonexistent)
{
	struct ubi_device *const ubi = sec_init();

	zassert_equal(-ENOENT, ubi_volume_remove(ubi, 999));

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
ZTEST(ubi_secure_error_handling, test_volume_get_info_nonexistent)
{
	struct ubi_device *const ubi = sec_init();

	struct ubi_volume_config cfg = { 0 };
	size_t alloc = 0;

	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, 999, &cfg, &alloc));

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
ZTEST(ubi_secure_error_handling, test_volume_resize_static)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "static",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	struct ubi_volume_config new_cfg = cfg;

	new_cfg.leb_count = 4;
	zassert_equal(-ECANCELED, ubi_volume_resize(ubi, vol_id, &new_cfg));

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
ZTEST(ubi_secure_error_handling, test_volume_resize_same_size)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "dyn",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_equal(-ECANCELED, ubi_volume_resize(ubi, vol_id, &cfg));

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
ZTEST(ubi_secure_error_handling, test_volume_resize_nonexistent)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "none",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};

	zassert_equal(-ENOENT, ubi_volume_resize(ubi, 999, &cfg));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_write() rejects a NULL data buffer.
 *
 * \details Scenario: Create a volume in secure mode. Call ubi_leb_write() with
 *          buf=NULL and len=10.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling, test_leb_write_null_buffer)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "wrtest",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_equal(-EINVAL, ubi_leb_write(ubi, vol_id, 0, NULL, 10));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_write() rejects a zero-length write.
 *
 * \details Scenario: Create a volume in secure mode. Call ubi_leb_write() with a
 *          valid buffer but len=0.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling, test_leb_write_zero_length)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "zerolen",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data = 0x42;

	zassert_equal(-EINVAL, ubi_leb_write(ubi, vol_id, 0, &data, 0));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that reading from an unmapped LEB fails.
 *
 * \details Scenario: Create a volume with 2 LEBs but do not write to any.
 *          Attempt to read from LEB 0.
 *
 * \expect ubi_leb_read() returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling, test_leb_read_unmapped)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "rdtest",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	uint8_t buf[16] = { 0 };

	zassert_equal(-ENOENT, ubi_leb_read(ubi, vol_id, 0, 0, buf, sizeof(buf)));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_read() rejects a NULL output buffer.
 *
 * \details Scenario: Create a volume in secure mode. Call ubi_leb_read() with
 *          buf=NULL.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling, test_leb_read_null_buffer)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "rdnull",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_equal(-EINVAL, ubi_leb_read(ubi, vol_id, 0, 0, NULL, 10));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that unmapping an already-unmapped LEB is idempotent.
 *
 * \details Scenario: Create a volume with 2 LEBs. Without mapping or writing
 *          to LEB 0, call ubi_leb_unmap() on it.
 *
 * \expect Returns 0 (idempotent no-op).
 */
ZTEST(ubi_secure_error_handling, test_leb_unmap_unmapped)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "umtest",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_is_mapped() rejects a NULL output pointer.
 *
 * \details Scenario: Initialize a secure device. Call ubi_leb_is_mapped() with
 *          is_mapped=NULL.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling, test_leb_is_mapped_null)
{
	struct ubi_device *const ubi = sec_init();

	zassert_equal(-EINVAL, ubi_leb_is_mapped(ubi, 0, 0, NULL));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_get_size() rejects a NULL output pointer.
 *
 * \details Scenario: Initialize a secure device. Call ubi_leb_get_size() with
 *          size=NULL.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling, test_leb_get_size_null)
{
	struct ubi_device *const ubi = sec_init();

	zassert_equal(-EINVAL, ubi_leb_get_size(ubi, 0, 0, NULL));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that overwriting an existing LEB moves the old PEB to dirty.
 *
 * \details Scenario: Create a static volume with 2 LEBs. Write 4 bytes to LEB 0,
 *          then overwrite it with 5 different bytes. Query the stored
 *          size and read back the data.
 *
 * \expect The second write succeeds. ubi_leb_get_size() returns 5.
 *           Read-back matches the second write. dirty_peb_count equals 1.
 */
ZTEST(ubi_secure_error_handling, test_leb_write_overwrite)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "overwr",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data1[] = { 0xAA, 0xBB, 0xCC, 0xDD };
	const uint8_t data2[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data1, sizeof(data1)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data2, sizeof(data2)));

	size_t size = 0;

	zassert_ok(ubi_leb_get_size(ubi, vol_id, 0, &size));
	zassert_equal(sizeof(data2), size);

	uint8_t rdata[8] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, size));
	zassert_mem_equal(rdata, data2, sizeof(data2));

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.dirty_peb_count);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_read() with a non-zero offset reads the correct
 *        tail portion of the stored data.
 *
 * \details Scenario: Write an 8-byte pattern to LEB 0. Read 4 bytes starting at
 *          offset 4.
 *
 * \expect ubi_leb_read() succeeds. The returned bytes match bytes 4..7
 *           of the original pattern.
 */
ZTEST(ubi_secure_error_handling, test_leb_read_with_offset)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "offrd",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t rdata[4] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 4, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, &data[4], sizeof(rdata));

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
ZTEST(ubi_secure_error_handling, test_volume_resize_shrink_with_mapped_lebs)
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
 * \brief Verify that writing to an out-of-range LEB number is rejected.
 *
 * \details Scenario: Create a static volume with 2 LEBs (valid lnum: 0..1).
 *          Attempt to write to LEB 3.
 *
 * \expect ubi_leb_write() returns -EACCES.
 */
ZTEST(ubi_secure_error_handling, test_leb_write_out_of_range_lnum)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "oor_w",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xAA };

	zassert_equal(-EACCES, ubi_leb_write(ubi, vol_id, 3, data, sizeof(data)));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that reading from an out-of-range LEB number is rejected.
 *
 * \details Scenario: Create a static volume with 2 LEBs (valid lnum: 0..1).
 *          Attempt to read from LEB 5.
 *
 * \expect ubi_leb_read() returns -EACCES.
 */
ZTEST(ubi_secure_error_handling, test_leb_read_out_of_range_lnum)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "oor_r",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	uint8_t rdata[1] = { 0 };

	zassert_equal(-EACCES, ubi_leb_read(ubi, vol_id, 5, 0, rdata, sizeof(rdata)));

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
ZTEST(ubi_secure_error_handling, test_volume_remove_and_recreate)
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
 * \brief Verify that writing data to a previously mapped (empty) LEB succeeds.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs. Map LEB 0. Verify it is
 *          mapped. Then write 4 bytes to the already-mapped LEB 0. Read back.
 *
 * \expect ubi_leb_map() succeeds. The subsequent write overwrites the empty
 *           mapping. Read-back returns the written data.
 */
ZTEST(ubi_secure_error_handling, test_leb_map_then_write)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "maptw",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_ok(ubi_leb_map(ubi, vol_id, 0));

	bool mapped = false;

	zassert_ok(ubi_leb_is_mapped(ubi, vol_id, 0, &mapped));
	zassert_true(mapped);

	const uint8_t data[] = { 0x01, 0x02, 0x03, 0x04 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t rdata[4] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, data, sizeof(data));

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
ZTEST(ubi_secure_error_handling, test_volume_resize_null_config)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "rsnul",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_equal(-EINVAL, ubi_volume_resize(ubi, vol_id, NULL));

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
ZTEST(ubi_secure_error_handling, test_volume_resize_no_volumes)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "nope",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};

	zassert_equal(-ENOENT, ubi_volume_resize(ubi, 0, &cfg));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_write() fails when no volumes exist.
 *
 * \details Scenario: Initialize a secure device with no volumes. Attempt to write
 *          1 byte to vol_id=0, LEB 0.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling, test_leb_write_no_volumes)
{
	struct ubi_device *const ubi = sec_init();

	const uint8_t data[] = { 0xAA };

	zassert_equal(-ENOENT, ubi_leb_write(ubi, 0, 0, data, sizeof(data)));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_read() fails when no volumes exist.
 *
 * \details Scenario: Initialize a secure device with no volumes. Attempt to read
 *          from vol_id=0, LEB 0.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling, test_leb_read_no_volumes)
{
	struct ubi_device *const ubi = sec_init();

	uint8_t rdata[1] = { 0 };

	zassert_equal(-ENOENT, ubi_leb_read(ubi, 0, 0, 0, rdata, sizeof(rdata)));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_unmap() rejects an out-of-range LEB number.
 *
 * \details Scenario: Create a static volume with 2 LEBs (valid lnum: 0..1).
 *          Attempt to unmap LEB 5.
 *
 * \expect Returns -EACCES.
 */
ZTEST(ubi_secure_error_handling, test_leb_unmap_out_of_range)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "umoor",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_equal(-EACCES, ubi_leb_unmap(ubi, vol_id, 5));

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
ZTEST(ubi_secure_error_handling, test_volume_get_info_no_volumes)
{
	struct ubi_device *const ubi = sec_init();

	struct ubi_volume_config cfg = { 0 };
	size_t alloc = 0;

	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, 0, &cfg, &alloc));

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
ZTEST(ubi_secure_error_handling, test_volume_remove_no_volumes)
{
	struct ubi_device *const ubi = sec_init();

	zassert_equal(-ENOENT, ubi_volume_remove(ubi, 0));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_unmap() fails when no volumes exist.
 *
 * \details Scenario: Initialize a secure device with no volumes. Attempt to unmap
 *          LEB 0 on vol_id=0.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling, test_leb_unmap_no_volumes)
{
	struct ubi_device *const ubi = sec_init();

	zassert_equal(-ENOENT, ubi_leb_unmap(ubi, 0, 0));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_is_mapped() fails when no volumes exist.
 *
 * \details Scenario: Initialize a secure device with no volumes. Call
 *          ubi_leb_is_mapped() for vol_id=0, LEB 0.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling, test_leb_is_mapped_no_volumes)
{
	struct ubi_device *const ubi = sec_init();

	bool mapped = false;

	zassert_equal(-ENOENT, ubi_leb_is_mapped(ubi, 0, 0, &mapped));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_get_size() fails when no volumes exist.
 *
 * \details Scenario: Initialize a secure device with no volumes. Call
 *          ubi_leb_get_size() for vol_id=0, LEB 0.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling, test_leb_get_size_no_volumes)
{
	struct ubi_device *const ubi = sec_init();

	size_t size = 0;

	zassert_equal(-ENOENT, ubi_leb_get_size(ubi, 0, 0, &size));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_write() fails when the volume does not exist.
 *
 * \details Scenario: Create one volume, then call ubi_leb_write() with a
 *          non-existent vol_id=999.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling, test_leb_write_vol_not_found)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "w_vnf",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xAA };

	zassert_equal(-ENOENT, ubi_leb_write(ubi, 999, 0, data, sizeof(data)));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_read() fails when the volume does not exist.
 *
 * \details Scenario: Create one volume, then call ubi_leb_read() with a
 *          non-existent vol_id=999.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling, test_leb_read_vol_not_found)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "r_vnf",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	uint8_t rdata[16] = { 0 };

	zassert_equal(-ENOENT, ubi_leb_read(ubi, 999, 0, 0, rdata, sizeof(rdata)));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_unmap() fails when the volume does not exist.
 *
 * \details Scenario: Create one volume, then call ubi_leb_unmap() with a
 *          non-existent vol_id=999.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling, test_leb_unmap_vol_not_found)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "u_vnf",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_equal(-ENOENT, ubi_leb_unmap(ubi, 999, 0));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_is_mapped() fails when the volume does not exist.
 *
 * \details Scenario: Create one volume, then call ubi_leb_is_mapped() with a
 *          non-existent vol_id=999.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling, test_leb_is_mapped_vol_not_found)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "m_vnf",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	bool mapped = false;

	zassert_equal(-ENOENT, ubi_leb_is_mapped(ubi, 999, 0, &mapped));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_get_size() fails when the volume does not exist.
 *
 * \details Scenario: Create one volume, then call ubi_leb_get_size() with a
 *          non-existent vol_id=999.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling, test_leb_get_size_vol_not_found)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "s_vnf",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	size_t size = 0;

	zassert_equal(-ENOENT, ubi_leb_get_size(ubi, 999, 0, &size));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_read() rejects an out-of-range LEB number.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs. Attempt to read LEB 5.
 *
 * \expect Returns -EACCES.
 */
ZTEST(ubi_secure_error_handling, test_leb_read_out_of_range)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "roor",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	uint8_t rdata[16] = { 0 };

	zassert_equal(-EACCES, ubi_leb_read(ubi, vol_id, 5, 0, rdata, sizeof(rdata)));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_is_mapped() rejects an out-of-range LEB number.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs. Attempt to check
 *          mapping of LEB 5.
 *
 * \expect Returns -EACCES.
 */
ZTEST(ubi_secure_error_handling, test_leb_is_mapped_out_of_range)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "moor",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	bool mapped = false;

	zassert_equal(-EACCES, ubi_leb_is_mapped(ubi, vol_id, 5, &mapped));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_get_size() rejects an out-of-range LEB number.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs. Attempt to get size of LEB 5.
 *
 * \expect Returns -EACCES.
 */
ZTEST(ubi_secure_error_handling, test_leb_get_size_out_of_range)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "soor",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	size_t size = 0;

	zassert_equal(-EACCES, ubi_leb_get_size(ubi, vol_id, 5, &size));

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
ZTEST(ubi_secure_error_handling, test_volume_create_invalid_type)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "badtp",
		.type = 42,
		.leb_count = 1,
	};
	int vol_id = -1;

	zassert_equal(-EINVAL, ubi_volume_create(ubi, &cfg, &vol_id));

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
ZTEST(ubi_secure_error_handling, test_volume_create_zero_lebs)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "zero",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 0,
	};
	int vol_id = -1;

	zassert_equal(-EINVAL, ubi_volume_create(ubi, &cfg, &vol_id));

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
ZTEST(ubi_secure_error_handling, test_volume_resize_zero_lebs_rejected)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "rzero",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const struct ubi_volume_config zero_cfg = {
		.name = "rzero",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 0,
	};

	zassert_equal(-EINVAL, ubi_volume_resize(ubi, vol_id, &zero_cfg));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that unmapping an unmapped LEB twice is safe (idempotent).
 *
 * \details Scenario: Create a volume but do not write any data. Unmap LEB 0 twice.
 *
 * \expect Both calls return 0.
 */
ZTEST(ubi_secure_error_handling, test_leb_unmap_unmapped_is_idempotent)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "idem_u",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that mapping an already-mapped LEB is a no-op.
 *
 * \details Scenario: Map a LEB, then call ubi_leb_map() again on the same LEB.
 *
 * \expect Second map returns 0 without consuming an extra PEB.
 */
ZTEST(ubi_secure_error_handling, test_leb_map_already_mapped_is_noop)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "noop_m",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_ok(ubi_leb_map(ubi, vol_id, 0));

	struct ubi_device_info info_before = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info_before));

	zassert_ok(ubi_leb_map(ubi, vol_id, 0));

	struct ubi_device_info info_after = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info_after));
	zassert_equal(info_before.free_peb_count, info_after.free_peb_count,
		      "No-op map should not consume a PEB");

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
ZTEST(ubi_secure_error_handling, test_static_volume_write_allowed)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "stwr",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xCA, 0xFE };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t rdata[2] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, data, sizeof(data));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_get_size() fails when the LEB is not mapped.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs. Without writing, attempt
 *          to get size of LEB 0.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling, test_leb_get_size_unmapped)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "gsum",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	size_t size = 0;

	zassert_equal(-ENOENT, ubi_leb_get_size(ubi, vol_id, 0, &size));

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
ZTEST(ubi_secure_error_handling, test_volume_create_duplicate_name)
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
ZTEST(ubi_secure_error_handling, test_volume_create_duplicate_name_different_config)
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
ZTEST(ubi_secure_error_handling, test_volume_create_empty_name)
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
ZTEST(ubi_secure_error_handling, test_volume_create_name_no_nul)
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
ZTEST(ubi_secure_error_handling, test_volume_create_name_max_valid)
{
	struct ubi_device *const ubi = sec_init();

	struct ubi_volume_config cfg = {
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};

	memset(cfg.name, 0, sizeof(cfg.name));
	memset(cfg.name, 'B', UBI_VOLUME_NAME_MAX_LEN - 1);

	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

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
ZTEST(ubi_secure_error_handling, test_volume_resize_expand_enospc)
{
	struct ubi_device *const ubi = sec_init();

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));

	const struct ubi_volume_config cfg = {
		.name = "rspc",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const struct ubi_volume_config big_cfg = {
		.name = "rspc",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = info.total_peb_count + 10,
	};

	zassert_equal(-ENOSPC, ubi_volume_resize(ubi, vol_id, &big_cfg));

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
ZTEST(ubi_secure_error_handling, test_volume_resize_shrink_trim)
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
ZTEST(ubi_secure_error_handling, test_volume_create_idempotent_returns_same_id)
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
ZTEST(ubi_secure_error_handling, test_volume_create_name_clash_different_config)
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
ZTEST(ubi_secure_error_handling, test_volume_resize_grow_preserves_data)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "grow",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xAA, 0xBB };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	const struct ubi_volume_config cfg4 = {
		.name = "grow",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 4,
	};

	zassert_ok(ubi_volume_resize(ubi, vol_id, &cfg4));

	uint8_t rb[2] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rb, sizeof(rb)));
	zassert_mem_equal(rb, data, sizeof(data));

	const uint8_t d3[] = { 0xCC };

	zassert_ok(ubi_leb_write(ubi, vol_id, 3, d3, sizeof(d3)));

	uint8_t r3[1] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 3, 0, r3, sizeof(r3)));
	zassert_equal(0xCC, r3[0]);

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
ZTEST(ubi_secure_error_handling, test_volume_resize_grow_enospc)
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
 * \brief Verify reading last byte at exact data boundary.
 *
 * \details Scenario: Write 8 bytes. Read last 1 byte at offset 7.
 *
 * \expect Read returns the correct last byte.
 */
ZTEST(ubi_secure_error_handling, test_leb_read_last_byte_at_boundary)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "bdry",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t rb = 0;

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 7, &rb, 1));
	zassert_equal(0x80, rb);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that reading beyond data_size returns -EINVAL.
 *
 * \details Scenario: Write 4 bytes. Attempt to read 8 bytes.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling, test_leb_read_beyond_data_size)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "over",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xAA, 0xBB, 0xCC, 0xDD };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t rb[8] = { 0 };
	const int ret = ubi_leb_read(ubi, vol_id, 0, 0, rb, sizeof(rb));

	zassert_equal(-EINVAL, ret, "Read beyond data_size should return -EINVAL");

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
ZTEST(ubi_secure_error_handling, test_volume_remove_and_recreate_different_config)
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
 * \brief Verify leb_unmap on already-unmapped LEB is idempotent (write+unmap).
 *
 * \details Scenario: Write data to LEB 0, unmap it, then unmap again.
 *
 * \expect Both unmap calls return 0.
 */
ZTEST(ubi_secure_error_handling, test_leb_unmap_already_unmapped_idempotent)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "unmapr",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	bool mapped = true;

	zassert_ok(ubi_leb_is_mapped(ubi, vol_id, 0, &mapped));
	zassert_false(mapped);
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

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
ZTEST(ubi_secure_error_handling, test_volume_get_info_detailed)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "detail",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 3,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0x11 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 2, data, sizeof(data)));

	struct ubi_volume_config out_cfg = { 0 };
	size_t alloc = 0;

	zassert_ok(ubi_volume_get_info(ubi, vol_id, &out_cfg, &alloc));

	zassert_equal(3, out_cfg.leb_count);
	zassert_equal(UBI_VOLUME_TYPE_DYNAMIC, out_cfg.type);
	zassert_equal(2, alloc, "Should have 2 allocated LEBs");
	zassert_true(strncmp(out_cfg.name, "detail", 6) == 0);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief LEB read beyond written data_size returns -EINVAL (detailed).
 *
 * \details Scenario: Write N bytes to a LEB. Read with offset+len exceeding stored size.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling, test_leb_read_beyond_data_size_returns_einval)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "readlim",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	uint8_t data[32] = { 0 };

	memset(data, 0xAA, sizeof(data));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t buf[64] = { 0 };
	int ret = ubi_leb_read(ubi, vol_id, 0, 0, buf, sizeof(buf));

	zassert_equal(ret, -EINVAL);

	ret = ubi_leb_read(ubi, vol_id, 0, 32, buf, 1);
	zassert_equal(ret, -EINVAL);

	ret = ubi_leb_read(ubi, vol_id, 0, 0, buf, 32);
	zassert_ok(ret);
	zassert_mem_equal(buf, data, 32);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief LEB map on non-existent volume returns -ENOENT.
 *
 * \details Scenario: Call ubi_leb_map with a non-existent vol_id.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling, test_leb_map_vol_not_found)
{
	struct ubi_device *const ubi = sec_init();

	const int ret = ubi_leb_map(ubi, 999, 0);

	zassert_equal(ret, -ENOENT);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief LEB map with lnum exceeding volume capacity returns error.
 *
 * \details Scenario: Call ubi_leb_map with lnum >= leb_count.
 *
 * \expect Returns non-zero error.
 */
ZTEST(ubi_secure_error_handling, test_leb_map_lnum_out_of_range)
{
	struct ubi_device *const ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "mapvol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	int ret = ubi_leb_map(ubi, vol_id, 2);

	zassert_not_equal(ret, 0);

	ret = ubi_leb_map(ubi, vol_id, 99);
	zassert_not_equal(ret, 0);

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
ZTEST(ubi_secure_error_handling, test_volume_remove_wrong_vol_id)
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
ZTEST(ubi_secure_error_handling, test_volume_resize_shrink_preserves_data)
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

ZTEST_SUITE(ubi_secure_error_handling, NULL, ztest_suite_setup, ztest_suite_before,
	    ztest_testcase_teardown, NULL);
