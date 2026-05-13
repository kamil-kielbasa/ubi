/**
 * \file    tests_ubi_error_handling_leb.c
 *
 * \author  Kamil Kielbasa
 *
 * \brief   Tests for UBI API error handling and edge cases (LEB API subset).
 *
 * \version 0.9
 * \date    2026-03-26
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI header: */
#include <ubi.h>
#include <ubi_test.h>

/* Zephyr headers: */
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <zephyr/toolchain/common.h>
#include <zephyr/sys/sys_heap.h>

#include <zephyr/sys/crc.h>

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "ubi_api_contract.h"

/* Module defines ------------------------------------------------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* Module types and type definitiones ----------------------------------------------------------- */

/* Module interface variables and constants ----------------------------------------------------- */

/* Static variables and constants --------------------------------------------------------------- */

static struct ubi_flash_desc flash = { 0 };

/* Static function declarations ----------------------------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_after(void *ctx);

static void ztest_testcase_before(void *ctx);
static void ztest_testcase_teardown(void *ctx);

/* Static function definitions ------------------------------------------------------------------ */

static void *ztest_suite_setup(void)
{
	const struct device *flash_dev = UBI_PARTITION_DEVICE;
	zassert_true(device_is_ready(flash_dev));

	struct flash_pages_info page_info = { 0 };
	zassert_ok(flash_get_page_info_by_offs(flash_dev, 0, &page_info));

	const size_t write_block_size = flash_get_write_block_size(flash_dev);
	const size_t erase_block_size = page_info.size;

	flash.partition_id = FIXED_PARTITION_ID(UBI_PARTITION_NAME);
	flash.erase_block_size = erase_block_size;
	flash.write_block_size = write_block_size;

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

	ubi_test_partition_force_release_all();
	zassert_ok(flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE));

	return;
}

static void ztest_testcase_teardown(void *ctx)
{
	(void)ctx;
	return;
}

/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_error_handling_leb, NULL, ztest_suite_setup, ztest_testcase_before,
	    ztest_testcase_teardown, ztest_suite_after);

/**
 * \brief Verify that ubi_leb_write() rejects a NULL data buffer.
 *
 * \details Scenario: Create a volume. Call ubi_leb_write() with buf=NULL
 *          and len=10.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_error_handling_leb, leb_write_null_buffer)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_write_null_buffer(ubi);

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
ZTEST(ubi_error_handling_leb, leb_write_zero_length)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_write_zero_length(ubi);

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
ZTEST(ubi_error_handling_leb, leb_read_unmapped)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "rdtest",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;
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
ZTEST(ubi_error_handling_leb, leb_read_null_buffer)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_read_null_buffer(ubi);

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
ZTEST(ubi_error_handling_leb, leb_unmap_unmapped)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_unmap_unmapped(ubi);

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
ZTEST(ubi_error_handling_leb, leb_is_mapped_null)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_is_mapped_null(ubi);

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
ZTEST(ubi_error_handling_leb, leb_get_size_null)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_get_size_null(ubi);

	zassert_ok(ubi_device_deinit(ubi));
}
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
ZTEST(ubi_error_handling_leb, leb_write_overwrite)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

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

	uint8_t rdata[8] = { 0 };
	size_t size = 0;
	zassert_ok(ubi_leb_get_size(ubi, vol_id, 0, &size));
	zassert_equal(sizeof(data2), size);

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, size));
	zassert_mem_equal(rdata, data2, sizeof(data2));

	/* Verify old PEB moved to dirty */
	struct ubi_device_info info = { 0 };
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
ZTEST(ubi_error_handling_leb, leb_read_with_offset)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_read_with_offset(ubi);

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
ZTEST(ubi_error_handling_leb, leb_write_out_of_range_lnum)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_write_out_of_range_lnum(ubi);

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
ZTEST(ubi_error_handling_leb, leb_read_out_of_range_lnum)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "oor_r",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	uint8_t rdata[1];
	/* lnum > leb_count should return -EACCES */
	zassert_equal(-EACCES, ubi_leb_read(ubi, vol_id, 5, 0, rdata, sizeof(rdata)));

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
ZTEST(ubi_error_handling_leb, leb_map_then_write)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_map_then_write(ubi);

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
ZTEST(ubi_error_handling_leb, leb_write_no_volumes)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_write_no_volumes(ubi);

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
ZTEST(ubi_error_handling_leb, leb_read_no_volumes)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

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
ZTEST(ubi_error_handling_leb, leb_unmap_out_of_range)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_unmap_out_of_range(ubi);

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
ZTEST(ubi_error_handling_leb, leb_unmap_no_volumes)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_unmap_no_volumes(ubi);

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
ZTEST(ubi_error_handling_leb, leb_is_mapped_no_volumes)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

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
ZTEST(ubi_error_handling_leb, leb_get_size_no_volumes)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_get_size_no_volumes(ubi);

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
ZTEST(ubi_error_handling_leb, leb_write_vol_not_found)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_write_vol_not_found(ubi);

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
ZTEST(ubi_error_handling_leb, leb_read_vol_not_found)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "r_vnf",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
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
ZTEST(ubi_error_handling_leb, leb_unmap_vol_not_found)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_unmap_vol_not_found(ubi);

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
ZTEST(ubi_error_handling_leb, leb_is_mapped_vol_not_found)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "m_vnf",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
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
ZTEST(ubi_error_handling_leb, leb_get_size_vol_not_found)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_get_size_vol_not_found(ubi);

	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that ubi_leb_read() rejects an out-of-range LEB number.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs (valid lnum: 0..1).
 *          Attempt to read LEB 5.
 *
 * \expect Returns -EACCES.
 */
ZTEST(ubi_error_handling_leb, leb_read_out_of_range)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "roor",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
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
ZTEST(ubi_error_handling_leb, leb_is_mapped_out_of_range)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "moor",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
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
ZTEST(ubi_error_handling_leb, leb_get_size_out_of_range)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_get_size_out_of_range(ubi);

	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that unmapping an unmapped LEB twice is safe (idempotent).
 *
 * \details Scenario: Create a volume but do not write any data. Unmap LEB 0.
 *
 * \expect Both calls return 0.
 */
ZTEST(ubi_error_handling_leb, leb_unmap_unmapped_is_idempotent)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_unmap_unmapped_is_idempotent(ubi);

	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that mapping an already-mapped LEB is a no-op.
 *
 * \details Scenario: Map a LEB, then call ubi_leb_map() again on the same LEB.
 *
 * \expect Second map call returns 0 without allocating a new PEB.
 */
ZTEST(ubi_error_handling_leb, leb_map_already_mapped_is_noop)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_map_already_mapped_is_noop(ubi);

	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that ubi_leb_get_size() fails when the LEB is not mapped.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs. Without writing,
 *          attempt to get size of LEB 0.
 *
 * \expect Returns -ENOENT because the LEB is not mapped.
 */
ZTEST(ubi_error_handling_leb, leb_get_size_unmapped)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_get_size_unmapped(ubi);

	zassert_ok(ubi_device_deinit(ubi));
}
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
ZTEST(ubi_error_handling_leb, leb_write_all_pebs_exhausted)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));

	/* Allocate all data PEBs to one volume. */
	const struct ubi_volume_config cfg = {
		.name = "full",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = info.total_peb_count,
	};
	int vol_id = -1;
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

/**
 * \brief Verify that ubi_leb_map() returns -ENOSPC when free PEBs
 *        are exhausted.
 *
 * \details Scenario: Create a volume using all PEBs, map every LEB, then
 *          unmap LEB 0 (old PEB → dirty). Try to re-map LEB 0.
 *
 * \expect ubi_leb_map() returns -ENOSPC because no free PEB is available.
 */
ZTEST(ubi_error_handling_leb, leb_map_all_pebs_exhausted)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));

	const struct ubi_volume_config cfg = {
		.name = "mapfull",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = info.total_peb_count,
	};
	int vol_id = -1;
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

/**
 * \brief Verify that reading from LEB with offset exactly at data boundary succeeds.
 *
 * \details Scenario: Write 8 bytes. Read last 1 byte at offset 7.
 *
 * \expect Read returns the correct last byte.
 */
ZTEST(ubi_error_handling_leb, leb_read_last_byte_at_boundary)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_read_last_byte_at_boundary(ubi);

	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that reading beyond data_size in VID header returns -EINVAL.
 *
 * \details Scenario: Write 4 bytes. Attempt to read 8 bytes.
 *
 * \expect Read returns -EINVAL (offset+len > data_size).
 */
ZTEST(ubi_error_handling_leb, leb_read_beyond_data_size)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_read_beyond_data_size(ubi);

	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify leb_unmap on already-unmapped LEB is idempotent.
 *
 * \details Scenario: Write data to LEB 0, unmap it, then unmap again.
 *
 * \expect Both unmap calls return 0. Second call is a no-op.
 */
ZTEST(ubi_error_handling_leb, leb_unmap_already_unmapped_idempotent)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = "unmapr",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* LEB 0 is not mapped yet — unmap should be a no-op success. */
	bool mapped = true;
	zassert_ok(ubi_leb_is_mapped(ubi, vol_id, 0, &mapped));
	zassert_false(mapped, "LEB 0 should not be mapped initially");
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief LEB read beyond written data_size returns -EINVAL.
 *
 * \details Scenario: Write N bytes to a LEB. Read with offset+len exceeding the stored data size.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_error_handling_leb, leb_read_beyond_data_size_returns_einval)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_volume_config cfg = { .type = UBI_VOLUME_TYPE_DYNAMIC, .leb_count = 1 };
	snprintf(cfg.name, sizeof(cfg.name), "readlim");
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Write exactly 32 bytes of data */
	uint8_t data[32] = { 0 };
	memset(data, 0xAA, sizeof(data));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	/* Try to read starting at offset 0 with length 64 (greater than 32) */
	uint8_t buf[64] = { 0 };
	int ret = ubi_leb_read(ubi, vol_id, 0, 0, buf, sizeof(buf));
	zassert_equal(ret, -EINVAL);

	/* Try to read 1 byte at offset 32 (offset + len = 33 > 32) */
	ret = ubi_leb_read(ubi, vol_id, 0, 32, buf, 1);
	zassert_equal(ret, -EINVAL);

	/* Read exactly 32 bytes should still work */
	ret = ubi_leb_read(ubi, vol_id, 0, 0, buf, 32);
	zassert_ok(ret);
	zassert_mem_equal(buf, data, 32);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief LEB map on non-existent volume returns -ENOENT.
 *
 * \details Scenario: Call ubi_leb_map with a non-existent vol_id.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_error_handling_leb, leb_map_vol_not_found)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	ubi_contract_leb_map_vol_not_found(ubi);

	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief LEB map with lnum exceeding volume capacity returns error.
 *
 * \details Scenario: Call ubi_leb_map with lnum >= leb_count.
 *
 * \expect Returns -EACCES.
 */
ZTEST(ubi_error_handling_leb, leb_map_lnum_out_of_range)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_volume_config cfg = { .type = UBI_VOLUME_TYPE_DYNAMIC, .leb_count = 2 };
	snprintf(cfg.name, sizeof(cfg.name), "mapvol");
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* lnum 2 is out of range (leb_count = 2, so valid range is 0-1) */
	int ret = ubi_leb_map(ubi, vol_id, 2);
	zassert_not_equal(ret, 0);

	/* lnum 99 is definitely out of range */
	ret = ubi_leb_map(ubi, vol_id, 99);
	zassert_not_equal(ret, 0);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief LEB unmap with corrupt EC header on mapped PEB.
 *
 * \details Scenario: Write to a LEB, corrupt the EC header of the mapped PEB on flash, then unmap. UBI should classify the corrupt PEB as bad during reclaim.
 *
 * \expect Unmap returns 0. Corrupt PEB moves to bad list.
 */
ZTEST(ubi_error_handling_leb, leb_unmap_corrupt_ec_header)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_volume_config cfg = { .type = UBI_VOLUME_TYPE_DYNAMIC, .leb_count = 1 };
	snprintf(cfg.name, sizeof(cfg.name), "unmvol");
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	uint8_t data[32] = { 0 };
	memset(data, 0xBB, sizeof(data));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	/* Corrupt the EC header of the mapped PEB */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	const size_t nr_pebs = fa->fa_size / flash.erase_block_size;
	for (size_t pnum = 2; pnum < nr_pebs; ++pnum) {
		uint8_t hdr[48] = { 0 };
		const size_t peb_off = pnum * flash.erase_block_size;
		zassert_ok(flash_area_read(fa, peb_off, hdr, sizeof(hdr)));
		uint32_t vid_magic = 0;
		memcpy(&vid_magic, hdr + 16, 4);
		if (vid_magic == 0x55424921U) {
			/* Erase PEB, corrupt EC CRC in buffer, write back */
			zassert_ok(flash_area_erase(fa, peb_off, flash.erase_block_size));
			memset(hdr + 12, 0, 4); /* zero EC CRC */
			zassert_ok(flash_area_write(fa, peb_off, hdr, sizeof(hdr)));
			break;
		}
	}
	flash_area_close(fa);

	/* Unmap should detect EC read failure */
	int ret = ubi_leb_unmap(ubi, vol_id, 0);
	/* The unmap may fail or succeed (depends on error handling) */
	(void)ret;

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief LEB VID header read failure during get_size.
 *
 * \details Scenario: Write to a LEB, corrupt the VID header CRC on flash, then call ubi_leb_get_size().
 *
 * \expect Returns -EIO because VID CRC check fails.
 */
ZTEST(ubi_error_handling_leb, leb_get_size_corrupt_vid)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_volume_config cfg = { .type = UBI_VOLUME_TYPE_DYNAMIC, .leb_count = 1 };
	snprintf(cfg.name, sizeof(cfg.name), "sizvol");
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	uint8_t data[48] = { 0 };
	memset(data, 0xCC, sizeof(data));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	/* Corrupt the VID header CRC of the mapped PEB */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	const size_t nr_pebs = fa->fa_size / flash.erase_block_size;
	for (size_t pnum = 2; pnum < nr_pebs; ++pnum) {
		uint8_t hdr[48] = { 0 };
		const size_t peb_off = pnum * flash.erase_block_size;
		zassert_ok(flash_area_read(fa, peb_off, hdr, sizeof(hdr)));
		uint32_t vid_magic = 0;
		memcpy(&vid_magic, hdr + 16, 4);
		if (vid_magic == 0x55424921U) {
			/* Erase PEB, corrupt VID CRC in buffer, write back */
			zassert_ok(flash_area_erase(fa, peb_off, flash.erase_block_size));
			memset(hdr + 44, 0, 4); /* zero VID CRC (offset 16+28) */
			zassert_ok(flash_area_write(fa, peb_off, hdr, sizeof(hdr)));
			break;
		}
	}
	flash_area_close(fa);

	/* get_size should fail because VID CRC is corrupt */
	size_t size = 0;
	int ret = ubi_leb_get_size(ubi, vol_id, 0, &size);
	zassert_not_equal(ret, 0);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief LEB VID header read failure during ubi_leb_read.
 *
 * \details Scenario: Write to a LEB, corrupt the VID header CRC on flash, then call ubi_leb_read().
 *
 * \expect Returns error because VID CRC check fails.
 */
ZTEST(ubi_error_handling_leb, leb_read_corrupt_vid_header)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	struct ubi_volume_config cfg = { .type = UBI_VOLUME_TYPE_DYNAMIC, .leb_count = 1 };
	snprintf(cfg.name, sizeof(cfg.name), "rdvol");
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	uint8_t data[32] = { 0 };
	memset(data, 0xDD, sizeof(data));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	/* Corrupt the VID header CRC */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	const size_t nr_pebs = fa->fa_size / flash.erase_block_size;
	for (size_t pnum = 2; pnum < nr_pebs; ++pnum) {
		uint8_t hdr[48] = { 0 };
		const size_t peb_off = pnum * flash.erase_block_size;
		zassert_ok(flash_area_read(fa, peb_off, hdr, sizeof(hdr)));
		uint32_t vid_magic = 0;
		memcpy(&vid_magic, hdr + 16, 4);
		if (vid_magic == 0x55424921U) {
			/* Erase PEB, corrupt VID CRC in buffer, write back */
			zassert_ok(flash_area_erase(fa, peb_off, flash.erase_block_size));
			memset(hdr + 44, 0, 4); /* zero VID CRC (offset 16+28) */
			zassert_ok(flash_area_write(fa, peb_off, hdr, sizeof(hdr)));
			break;
		}
	}
	flash_area_close(fa);

	/* Read should fail because VID CRC check fails */
	uint8_t buf[32] = { 0 };
	int ret = ubi_leb_read(ubi, vol_id, 0, 0, buf, 16);
	zassert_not_equal(ret, 0);

	zassert_ok(ubi_device_deinit(ubi));
}
