/**
 * \file    tests_ubi_error_handling.c
 *
 * \author  Kamil Kielbasa
 *
 * \brief   Tests for UBI API error handling and edge cases.
 *
 * \version 0.9
 * \date    2026-03-26
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files ------------------------------------------------------------------------------- */

/* UBI header: */
#include <ubi.h>

/* Zephyr headers: */
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <zephyr/toolchain/common.h>
#include <zephyr/sys/sys_heap.h>

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------ */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* Module types and type definitiones ---------------------------------------------------------- */
/* Module interface variables and constants ---------------------------------------------------- */
/* Static variables and constants -------------------------------------------------------------- */

static struct ubi_mtd mtd = { 0 };

/* Static function declarations ---------------------------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_after(void *ctx);

static void ztest_testcase_before(void *ctx);
static void ztest_testcase_teardown(void *ctx);

/* Static function definitions ----------------------------------------------------------------- */

static void *ztest_suite_setup(void)
{
	const struct device *flash_dev = UBI_PARTITION_DEVICE;
	zassert_true(device_is_ready(flash_dev));

	struct flash_pages_info page_info = { 0 };
	zassert_ok(flash_get_page_info_by_offs(flash_dev, 0, &page_info));

	const size_t write_block_size = flash_get_write_block_size(flash_dev);
	const size_t erase_block_size = page_info.size;

	mtd.partition_id = FIXED_PARTITION_ID(UBI_PARTITION_NAME);
	mtd.erase_block_size = erase_block_size;
	mtd.write_block_size = write_block_size;

	return NULL;
}

static void ztest_suite_after(void *ctx)
{
	(void)ctx;

	return;
}

static void ztest_testcase_before(void *ctx)
{
	(void)ctx;

	zassert_ok(flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE));

	return;
}

static void ztest_testcase_teardown(void *ctx)
{
	(void)ctx;
	return;
}

/* Module interface function definitions ------------------------------------------------------- */

ZTEST_SUITE(ubi_error_handling, NULL, ztest_suite_setup, ztest_testcase_before,
	    ztest_testcase_teardown, ztest_suite_after);

/* --- Device init/deinit error paths --- */

/**
 * \brief Verify that ubi_device_init() rejects a NULL MTD descriptor.
 *
 * \details Scenario: Call ubi_device_init() with mtd=NULL.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_error_handling, init_null_mtd)
{
	struct ubi_device *ubi = NULL;
	zassert_equal(-EINVAL, ubi_device_init(NULL, &ubi));
}

/**
 * \brief Verify that ubi_device_init() rejects a NULL output pointer.
 *
 * \details Scenario: Call ubi_device_init() with ubi=NULL.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_error_handling, init_null_ubi)
{
	zassert_equal(-EINVAL, ubi_device_init(&mtd, NULL));
}

/**
 * \brief Verify that ubi_device_deinit() rejects a NULL device pointer.
 *
 * \details Scenario: Call ubi_device_deinit() with NULL.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_error_handling, deinit_null)
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
ZTEST(ubi_error_handling, get_info_null_device)
{
	struct ubi_device_info info;
	zassert_equal(-EINVAL, ubi_device_get_info(NULL, &info));
}

/**
 * \brief Verify that ubi_device_get_info() rejects a NULL info buffer.
 *
 * \details Scenario: Initialize a device, then call ubi_device_get_info()
 *          with info=NULL.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_error_handling, get_info_null_info)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	zassert_equal(-EINVAL, ubi_device_get_info(ubi, NULL));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_device_erase_peb() rejects a NULL device pointer.
 *
 * \details Scenario: Call ubi_device_erase_peb() with NULL.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_error_handling, erase_peb_null)
{
	zassert_equal(-EINVAL, ubi_device_erase_peb(NULL));
}

/* --- Volume error paths --- */

/**
 * \brief Verify that ubi_volume_create() rejects NULL parameters.
 *
 * \details Scenario: Call ubi_volume_create() with each of the three parameters
 *          (ubi, vol_cfg, vol_id) set to NULL individually.
 *
 * \expect Each call returns -EINVAL.
 */
ZTEST(ubi_error_handling, volume_create_null_params)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	int vol_id;
	const struct ubi_volume_config cfg = {
		.name = "test",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	zassert_equal(-EINVAL, ubi_volume_create(NULL, &cfg, &vol_id));
	zassert_equal(-EINVAL, ubi_volume_create(ubi, NULL, &vol_id));
	zassert_equal(-EINVAL, ubi_volume_create(ubi, &cfg, NULL));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that creating the same volume twice is idempotent.
 *
 * \details Scenario: Create a static volume named "idem". Call
 *          ubi_volume_create() again with the same configuration.
 *
 * \expect Both calls succeed. The returned vol_id is identical.
 *         ubi_device_get_info() reports volume_count=1.
 */
ZTEST(ubi_error_handling, volume_create_idempotent)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

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

	struct ubi_device_info info;
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count);

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
ZTEST(ubi_error_handling, volume_create_no_space)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	struct ubi_device_info info;
	zassert_ok(ubi_device_get_info(ubi, &info));

	const struct ubi_volume_config cfg = {
		.name = "huge",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = info.total_peb_count + 1,
	};
	int vol_id;

	zassert_equal(-ENOSPC, ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that removing a non-existent volume fails.
 *
 * \details Scenario: Initialize a device with no volumes. Attempt to remove
 *          vol_id=999.
 *
 * \expect ubi_volume_remove() returns -ENOENT.
 */
ZTEST(ubi_error_handling, volume_remove_nonexistent)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	zassert_equal(-ENOENT, ubi_volume_remove(ubi, 999));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that querying info for a non-existent volume fails.
 *
 * \details Scenario: Initialize a device with no volumes. Call
 *          ubi_volume_get_info() for vol_id=999.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_error_handling, volume_get_info_nonexistent)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	struct ubi_volume_config cfg;
	size_t alloc;
	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, 999, &cfg, &alloc));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that resizing a static volume is rejected.
 *
 * \details Scenario: Create a static volume with 2 LEBs. Attempt to resize
 *          it to 4 LEBs.
 *
 * \expect ubi_volume_resize() returns -ECANCELED. Static volumes are
 *         immutable in size by design.
 */
ZTEST(ubi_error_handling, volume_resize_static)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "static",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	struct ubi_volume_config new_cfg = cfg;
	new_cfg.leb_count = 4;
	zassert_equal(-ECANCELED, ubi_volume_resize(ubi, vol_id, &new_cfg));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that resizing a volume to its current size is rejected.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs. Attempt to resize
 *          it to the same count (2 LEBs).
 *
 * \expect ubi_volume_resize() returns -ECANCELED.
 */
ZTEST(ubi_error_handling, volume_resize_same_size)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "dyn",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_equal(-ECANCELED, ubi_volume_resize(ubi, vol_id, &cfg));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that resizing a non-existent volume fails.
 *
 * \details Scenario: Initialize a device with no volumes. Attempt to resize
 *          vol_id=999.
 *
 * \expect ubi_volume_resize() returns -ENOENT.
 */
ZTEST(ubi_error_handling, volume_resize_nonexistent)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "none",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	zassert_equal(-ENOENT, ubi_volume_resize(ubi, 999, &cfg));

	zassert_ok(ubi_device_deinit(ubi));
}

/* --- LEB I/O error paths --- */

/**
 * \brief Verify that ubi_leb_write() rejects a NULL data buffer.
 *
 * \details Scenario: Create a volume. Call ubi_leb_write() with buf=NULL
 *          and len=10.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_error_handling, leb_write_null_buffer)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "wrtest",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_equal(-EINVAL, ubi_leb_write(ubi, vol_id, 0, NULL, 10));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_write() rejects a zero-length write.
 *
 * \details Scenario: Create a volume. Call ubi_leb_write() with a valid
 *          buffer but len=0.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_error_handling, leb_write_zero_length)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "zerolen",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	uint8_t data = 0x42;
	zassert_equal(-EINVAL, ubi_leb_write(ubi, vol_id, 0, &data, 0));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that reading from an unmapped LEB fails.
 *
 * \details Scenario: Create a volume with 2 LEBs but do not write to any.
 *          Attempt to read from LEB 0.
 *
 * \expect ubi_leb_read() returns -ENOENT because no PEB is mapped for LEB 0.
 */
ZTEST(ubi_error_handling, leb_read_unmapped)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "rdtest",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	uint8_t buf[16];
	zassert_equal(-ENOENT, ubi_leb_read(ubi, vol_id, 0, 0, buf, sizeof(buf)));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_read() rejects a NULL output buffer.
 *
 * \details Scenario: Create a volume. Call ubi_leb_read() with buf=NULL.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_error_handling, leb_read_null_buffer)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "rdnull",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_equal(-EINVAL, ubi_leb_read(ubi, vol_id, 0, 0, NULL, 10));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that unmapping an already-unmapped LEB fails.
 *
 * \details Scenario: Create a volume with 2 LEBs. Without mapping or writing
 *          to LEB 0, call ubi_leb_unmap() on it.
 *
 * \expect Returns -EACCES because the LEB has no PEB mapping to remove.
 */
ZTEST(ubi_error_handling, leb_unmap_unmapped)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "umtest",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_equal(-EACCES, ubi_leb_unmap(ubi, vol_id, 0));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_is_mapped() rejects a NULL output pointer.
 *
 * \details Scenario: Initialize a device. Call ubi_leb_is_mapped() with
 *          is_mapped=NULL.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_error_handling, leb_is_mapped_null)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	zassert_equal(-EINVAL, ubi_leb_is_mapped(ubi, 0, 0, NULL));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_get_size() rejects a NULL output pointer.
 *
 * \details Scenario: Initialize a device. Call ubi_leb_get_size() with
 *          size=NULL.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_error_handling, leb_get_size_null)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	zassert_equal(-EINVAL, ubi_leb_get_size(ubi, 0, 0, NULL));

	zassert_ok(ubi_device_deinit(ubi));
}

/* --- Functional edge cases --- */

/**
 * \brief Verify that overwriting an existing LEB moves the old PEB to dirty.
 *
 * \details Scenario: Create a static volume with 2 LEBs. Write 4 bytes to
 *          LEB 0, then overwrite it with 5 different bytes. Query the stored
 *          size and read back the data.
 *
 * \expect The second write succeeds. ubi_leb_get_size() returns 5. Read-back
 *         matches the second write. dirty_peb_count equals 1 (the old PEB).
 */
ZTEST(ubi_error_handling, leb_write_overwrite)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "overwr",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data1[] = { 0xAA, 0xBB, 0xCC, 0xDD };
	const uint8_t data2[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data1, sizeof(data1)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data2, sizeof(data2)));

	uint8_t rdata[8] = { 0 };
	size_t size = 0;
	zassert_ok(ubi_leb_get_size(ubi, vol_id, 0, &size));
	zassert_equal(sizeof(data2), size);

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, size));
	zassert_mem_equal(rdata, data2, sizeof(data2));

	/* Verify old PEB moved to dirty */
	struct ubi_device_info info;
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.dirty_peb_count);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_read() with a non-zero offset reads the correct
 *        tail portion of the stored data.
 *
 * \details Scenario: Write an 8-byte pattern to LEB 0. Read 4 bytes starting
 *          at offset 4.
 *
 * \expect ubi_leb_read() succeeds. The returned bytes match bytes 4..7 of
 *         the original pattern.
 */
ZTEST(ubi_error_handling, leb_read_with_offset)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "offrd",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t rdata[4] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 4, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, &data[4], sizeof(rdata));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that shrinking a volume with mapped LEBs trims the excess.
 *
 * \details Scenario: Create a dynamic volume with 4 LEBs and write data to
 *          all four. Resize the volume down to 2 LEBs. Verify LEBs 0 and 1
 *          are still accessible.
 *
 * \expect ubi_volume_resize() succeeds. LEBs 0..1 are readable with correct
 *         data. dirty_peb_count >= 2 (the trimmed PEBs from LEBs 2..3).
 */
ZTEST(ubi_error_handling, volume_resize_shrink_with_mapped_lebs)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "shrink",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 4,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xDE, 0xAD };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 1, data, sizeof(data)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 2, data, sizeof(data)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 3, data, sizeof(data)));

	struct ubi_volume_config new_cfg = cfg;
	new_cfg.leb_count = 2;
	zassert_ok(ubi_volume_resize(ubi, vol_id, &new_cfg));

	/* LEBs 0,1 should still be accessible */
	uint8_t rdata[2];
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, data, sizeof(data));

	/* Dirty PEBs should exist from the trimmed LEBs */
	struct ubi_device_info info;
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.dirty_peb_count >= 2);

	zassert_ok(ubi_device_deinit(ubi));
}

/* --- Additional error handling and edge case tests --- */

/**
 * \brief Verify that writing to an out-of-range LEB number is rejected.
 *
 * \details Scenario: Create a static volume with 2 LEBs (valid lnum: 0..1).
 *          Attempt to write to LEB 3.
 *
 * \expect ubi_leb_write() returns -EACCES.
 */
ZTEST(ubi_error_handling, leb_write_out_of_range_lnum)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "oor_w",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xAA };
	/* lnum > leb_count should return -EACCES */
	zassert_equal(-EACCES, ubi_leb_write(ubi, vol_id, 3, data, sizeof(data)));

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
ZTEST(ubi_error_handling, leb_read_out_of_range_lnum)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "oor_r",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	uint8_t rdata[1];
	/* lnum > leb_count should return -EACCES */
	zassert_equal(-EACCES, ubi_leb_read(ubi, vol_id, 5, 0, rdata, sizeof(rdata)));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that a removed volume can be recreated with a clean state.
 *
 * \details Scenario: Create a static volume, write data to LEB 0, then remove
 *          the volume. Recreate a new volume with the same name and config.
 *          Check the mapping state of LEB 0 in the new volume.
 *
 * \expect The new volume is created successfully. LEB 0 is not mapped,
 *         confirming the new volume has no residual state from the old one.
 */
ZTEST(ubi_error_handling, volume_remove_and_recreate)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "rmcrt",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xDE, 0xAD };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	zassert_ok(ubi_volume_remove(ubi, vol_id));

	/* Recreate with the same name */
	int vol_id2;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id2));

	/* New volume should have no mapped LEBs */
	bool mapped = true;
	zassert_ok(ubi_leb_is_mapped(ubi, vol_id2, 0, &mapped));
	zassert_false(mapped);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that writing data to a previously mapped (empty) LEB succeeds.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs. Map LEB 0 (reserves
 *          a PEB with no user data). Verify it is mapped. Then write 4 bytes
 *          to the already-mapped LEB 0. Read back the data.
 *
 * \expect ubi_leb_map() succeeds. The subsequent write overwrites the empty
 *         mapping. Read-back returns the written data.
 */
ZTEST(ubi_error_handling, leb_map_then_write)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "maptw",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Map LEB 0 (reserves a PEB without data) */
	zassert_ok(ubi_leb_map(ubi, vol_id, 0));

	bool mapped = false;
	zassert_ok(ubi_leb_is_mapped(ubi, vol_id, 0, &mapped));
	zassert_true(mapped);

	/* Write data to already-mapped LEB 0 (should overwrite) */
	const uint8_t data[] = { 0x01, 0x02, 0x03, 0x04 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t rdata[4] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, data, sizeof(data));

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
ZTEST(ubi_error_handling, volume_resize_null_config)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "rsnul",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* NULL vol_cfg should return -EINVAL */
	zassert_equal(-EINVAL, ubi_volume_resize(ubi, vol_id, NULL));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_volume_resize() fails when no volumes exist.
 *
 * \details Scenario: Initialize a device with no volumes. Attempt to resize
 *          vol_id=0.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_error_handling, volume_resize_no_volumes)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "nope",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};

	/* No volumes exist, resize should return -ENOENT */
	zassert_equal(-ENOENT, ubi_volume_resize(ubi, 0, &cfg));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_write() fails when no volumes exist on the device.
 *
 * \details Scenario: Initialize a device with no volumes. Attempt to write
 *          1 byte to vol_id=0, LEB 0.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_error_handling, leb_write_no_volumes)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const uint8_t data[] = { 0xAA };
	/* No volumes => -ENOENT */
	zassert_equal(-ENOENT, ubi_leb_write(ubi, 0, 0, data, sizeof(data)));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_read() fails when no volumes exist on the device.
 *
 * \details Scenario: Initialize a device with no volumes. Attempt to read
 *          from vol_id=0, LEB 0.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_error_handling, leb_read_no_volumes)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	uint8_t rdata[1];
	/* No volumes => -ENOENT */
	zassert_equal(-ENOENT, ubi_leb_read(ubi, 0, 0, 0, rdata, sizeof(rdata)));

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
ZTEST(ubi_error_handling, leb_unmap_out_of_range)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "umoor",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* lnum > leb_count should return -EACCES */
	zassert_equal(-EACCES, ubi_leb_unmap(ubi, vol_id, 5));

	zassert_ok(ubi_device_deinit(ubi));
}

/* --- No-volumes error paths for remaining API functions --- */

/**
 * \brief Verify that ubi_volume_get_info() fails when no volumes exist.
 *
 * \details Scenario: Initialize a device with no volumes. Call
 *          ubi_volume_get_info() for vol_id=0.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_error_handling, volume_get_info_no_volumes)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	struct ubi_volume_config cfg;
	size_t alloc;
	zassert_equal(-ENOENT, ubi_volume_get_info(ubi, 0, &cfg, &alloc));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_volume_remove() fails when no volumes exist.
 *
 * \details Scenario: Initialize a device with no volumes. Attempt to remove
 *          vol_id=0.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_error_handling, volume_remove_no_volumes)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	zassert_equal(-ENOENT, ubi_volume_remove(ubi, 0));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_unmap() fails when no volumes exist.
 *
 * \details Scenario: Initialize a device with no volumes. Attempt to unmap
 *          LEB 0 on vol_id=0.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_error_handling, leb_unmap_no_volumes)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	zassert_equal(-ENOENT, ubi_leb_unmap(ubi, 0, 0));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_is_mapped() fails when no volumes exist.
 *
 * \details Scenario: Initialize a device with no volumes. Call
 *          ubi_leb_is_mapped() for vol_id=0, LEB 0.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_error_handling, leb_is_mapped_no_volumes)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	bool mapped;
	zassert_equal(-ENOENT, ubi_leb_is_mapped(ubi, 0, 0, &mapped));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_get_size() fails when no volumes exist.
 *
 * \details Scenario: Initialize a device with no volumes. Call
 *          ubi_leb_get_size() for vol_id=0, LEB 0.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_error_handling, leb_get_size_no_volumes)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	size_t size;
	zassert_equal(-ENOENT, ubi_leb_get_size(ubi, 0, 0, &size));

	zassert_ok(ubi_device_deinit(ubi));
}

/* --- Volume-not-found error paths for LEB operations --- */

/**
 * \brief Verify that ubi_leb_write() fails when the volume does not exist.
 *
 * \details Scenario: Create one volume, then call ubi_leb_write() with a
 *          non-existent vol_id=999.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_error_handling, leb_write_vol_not_found)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "w_vnf",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xAA };
	zassert_equal(-ENOENT, ubi_leb_write(ubi, 999, 0, data, sizeof(data)));

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
ZTEST(ubi_error_handling, leb_read_vol_not_found)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "r_vnf",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	uint8_t rdata[16];
	zassert_equal(-ENOENT, ubi_leb_read(ubi, 999, 0, 0, rdata, sizeof(rdata)));

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
ZTEST(ubi_error_handling, leb_unmap_vol_not_found)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "u_vnf",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_equal(-ENOENT, ubi_leb_unmap(ubi, 999, 0));

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
ZTEST(ubi_error_handling, leb_is_mapped_vol_not_found)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "m_vnf",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	bool mapped;
	zassert_equal(-ENOENT, ubi_leb_is_mapped(ubi, 999, 0, &mapped));

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
ZTEST(ubi_error_handling, leb_get_size_vol_not_found)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "s_vnf",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	size_t size;
	zassert_equal(-ENOENT, ubi_leb_get_size(ubi, 999, 0, &size));

	zassert_ok(ubi_device_deinit(ubi));
}

/* --- LEB limit exceeded for remaining functions --- */

/**
 * \brief Verify that ubi_leb_read() rejects an out-of-range LEB number.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs (valid lnum: 0..1).
 *          Attempt to read LEB 5.
 *
 * \expect Returns -EACCES.
 */
ZTEST(ubi_error_handling, leb_read_out_of_range)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "roor",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	uint8_t rdata[16];
	zassert_equal(-EACCES, ubi_leb_read(ubi, vol_id, 5, 0, rdata, sizeof(rdata)));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_is_mapped() rejects an out-of-range LEB number.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs (valid lnum: 0..1).
 *          Attempt to check mapping of LEB 5.
 *
 * \expect Returns -EACCES.
 */
ZTEST(ubi_error_handling, leb_is_mapped_out_of_range)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "moor",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	bool mapped;
	zassert_equal(-EACCES, ubi_leb_is_mapped(ubi, vol_id, 5, &mapped));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_get_size() rejects an out-of-range LEB number.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs (valid lnum: 0..1).
 *          Attempt to get size of LEB 5.
 *
 * \expect Returns -EACCES.
 */
ZTEST(ubi_error_handling, leb_get_size_out_of_range)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "soor",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	size_t size;
	zassert_equal(-EACCES, ubi_leb_get_size(ubi, vol_id, 5, &size));

	zassert_ok(ubi_device_deinit(ubi));
}

/* --- Unmapped LEB paths --- */

/**
 * \brief Verify that ubi_leb_get_size() fails when the LEB is not mapped.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs. Without writing,
 *          attempt to get size of LEB 0.
 *
 * \expect Returns -ENOENT because the LEB is not mapped.
 */
ZTEST(ubi_error_handling, leb_get_size_unmapped)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "gsum",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	size_t size;
	zassert_equal(-ENOENT, ubi_leb_get_size(ubi, vol_id, 0, &size));

	zassert_ok(ubi_device_deinit(ubi));
}

/* --- Volume create duplicate name --- */

/**
 * \brief Verify that creating a volume with a duplicate name returns the
 *        existing volume's ID instead of creating a new one.
 *
 * \details Scenario: Create a volume "dup", then call ubi_volume_create again
 *          with the same name "dup".
 *
 * \expect Second call returns 0 and sets vol_id to the existing volume's ID.
 */
ZTEST(ubi_error_handling, volume_create_duplicate_name)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "dup",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id_1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id_1));

	int vol_id_2;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id_2));
	zassert_equal(vol_id_1, vol_id_2);

	zassert_ok(ubi_device_deinit(ubi));
}

/* --- Volume resize to insufficient space --- */

/**
 * \brief Verify that ubi_volume_resize() fails when expanding beyond available
 *        PEBs.
 *
 * \details Scenario: Create a volume consuming most partition space, then
 *          resize it to exceed the total available PEBs.
 *
 * \expect Returns -ENOSPC.
 */
ZTEST(ubi_error_handling, volume_resize_expand_enospc)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	struct ubi_device_info info;
	zassert_ok(ubi_device_get_info(ubi, &info));

	const struct ubi_volume_config cfg = {
		.name = "rspc",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Try to resize to more LEBs than the partition can hold */
	const struct ubi_volume_config big_cfg = {
		.name = "rspc",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = info.total_peb_count + 10,
	};
	zassert_equal(-ENOSPC, ubi_volume_resize(ubi, vol_id, &big_cfg));

	zassert_ok(ubi_device_deinit(ubi));
}

/* --- Volume resize shrink with mapped LEBs --- */

/**
 * \brief Verify that ubi_volume_resize() can shrink a volume that has mapped
 *        LEBs in the trimmed range, and the data in those LEBs is discarded.
 *
 * \details Scenario: Create a volume with 4 LEBs, write data to LEBs 0-3,
 *          then resize down to 2 LEBs. Verify LEBs 0-1 remain readable
 *          and the volume's leb_count is now 2.
 *
 * \expect Resize succeeds. volume_get_info reports leb_count=2. LEBs 0-1
 *         are still readable.
 */
ZTEST(ubi_error_handling, volume_resize_shrink_trim)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "shrk",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 4,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Write data to all 4 LEBs */
	const uint8_t pattern = 0xBB;
	uint8_t wdata[64];
	memset(wdata, pattern, sizeof(wdata));

	for (size_t lnum = 0; lnum < 4; ++lnum) {
		zassert_ok(ubi_leb_write(ubi, vol_id, lnum, wdata, sizeof(wdata)));
		zassert_ok(ubi_device_erase_peb(ubi));
	}

	/* Shrink to 2 LEBs — trims LEBs 2-3 */
	const struct ubi_volume_config shrink_cfg = {
		.name = "shrk",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	zassert_ok(ubi_volume_resize(ubi, vol_id, &shrink_cfg));

	/* Verify volume info */
	struct ubi_volume_config after_cfg;
	size_t alloc_lebs;
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &after_cfg, &alloc_lebs));
	zassert_equal(2, after_cfg.leb_count);

	/* LEBs 0-1 should still be readable */
	uint8_t rdata[64];
	for (size_t lnum = 0; lnum < 2; ++lnum) {
		zassert_ok(ubi_leb_read(ubi, vol_id, lnum, 0, rdata, sizeof(rdata)));
		zassert_mem_equal(wdata, rdata, sizeof(wdata));
	}

	zassert_ok(ubi_device_deinit(ubi));
}

/* --- LEB write when all PEBs exhausted --- */

/**
 * \brief Verify that ubi_leb_write() returns -ENOSPC when all free PEBs
 *        are consumed.
 *
 * \details Scenario: Create a volume occupying all data PEBs. Write to every
 *          LEB so all free PEBs become allocated. Without erasing dirty PEBs,
 *          overwrite LEB 0 — its old PEB goes to dirty, but no free PEB is
 *          available for the new write.
 *
 * \expect The overwrite of LEB 0 returns -ENOSPC.
 */
ZTEST(ubi_error_handling, leb_write_all_pebs_exhausted)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	struct ubi_device_info info;
	zassert_ok(ubi_device_get_info(ubi, &info));

	/* Allocate all data PEBs to one volume. */
	const struct ubi_volume_config cfg = {
		.name = "full",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = info.total_peb_count,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Write to every LEB — consumes all free PEBs. */
	const uint8_t pattern = 0xAB;
	uint8_t wdata[32];
	memset(wdata, pattern, sizeof(wdata));

	for (size_t lnum = 0; lnum < info.total_peb_count; ++lnum) {
		zassert_ok(ubi_leb_write(ubi, vol_id, lnum, wdata, sizeof(wdata)));
	}

	/* All PEBs allocated, free count must be 0. */
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.free_peb_count);

	/* Overwrite LEB 0 — old PEB → dirty, but no free PEB → -ENOSPC. */
	const uint8_t new_data[] = { 0x01 };
	zassert_equal(-ENOSPC, ubi_leb_write(ubi, vol_id, 0, new_data, sizeof(new_data)));

	zassert_ok(ubi_device_deinit(ubi));
}

/* --- LEB map when all PEBs exhausted --- */

/**
 * \brief Verify that ubi_leb_map() returns -ENOSPC when free PEBs
 *        are exhausted.
 *
 * \details Scenario: Create a volume using all PEBs, map every LEB, then
 *          unmap LEB 0 (old PEB → dirty). Try to re-map LEB 0.
 *
 * \expect ubi_leb_map() returns -ENOSPC because no free PEB is available.
 */
ZTEST(ubi_error_handling, leb_map_all_pebs_exhausted)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, &ubi));

	struct ubi_device_info info;
	zassert_ok(ubi_device_get_info(ubi, &info));

	const struct ubi_volume_config cfg = {
		.name = "mapfull",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = info.total_peb_count,
	};
	int vol_id;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Map every LEB — exhausts all free PEBs. */
	for (size_t lnum = 0; lnum < info.total_peb_count; ++lnum) {
		zassert_ok(ubi_leb_map(ubi, vol_id, lnum));
	}

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.free_peb_count);

	/* Unmap LEB 0 — PEB goes to dirty, not free. */
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

	/* Re-map LEB 0 — no free PEB → -ENOSPC. */
	zassert_equal(-ENOSPC, ubi_leb_map(ubi, vol_id, 0));

	zassert_ok(ubi_device_deinit(ubi));
}
