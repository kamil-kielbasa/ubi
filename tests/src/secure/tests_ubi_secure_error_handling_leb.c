/**
 * \file    tests_ubi_secure_error_handling_leb.c
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

/* UBI headers: */
#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>
#include "ubi_api_contract.h"

/* Test fixtures: */
#include "ubi_test_fixture.h"
#include "ubi_test_secure_fixture.h"

/* Zephyr headers: */
#include <psa/crypto.h>
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/sys/crc.h>

/* Standard library headers: */
#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* Module types and type definitiones ----------------------------------------------------------- */

/* Module interface variables and constants ----------------------------------------------------- */

/* Static variables and constants --------------------------------------------------------------- */
static struct ubi_flash_desc flash = { 0 };
static struct ubi_device *g_ubi;

/* Static function declarations ----------------------------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_before(void *ctx);
static void ztest_testcase_teardown(void *ctx);
static struct ubi_device *sec_init(void);

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

ZTEST_SUITE(ubi_secure_error_handling_leb, NULL, ztest_suite_setup, ztest_suite_before,
	    ztest_testcase_teardown, NULL);

/**
 * \brief Verify that ubi_leb_write() rejects a NULL data buffer.
 *
 * \details Scenario: Create a volume in secure mode. Call ubi_leb_write() with
 *          buf=NULL and len=10.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling_leb, leb_write_null_buffer)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_leb_write_null_buffer(ubi);

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
ZTEST(ubi_secure_error_handling_leb, leb_write_zero_length)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_leb_write_zero_length(ubi);

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
ZTEST(ubi_secure_error_handling_leb, leb_read_unmapped)
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
ZTEST(ubi_secure_error_handling_leb, leb_read_null_buffer)
{
	struct ubi_device *const ubi = sec_init();

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
ZTEST(ubi_secure_error_handling_leb, leb_unmap_unmapped)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_leb_unmap_unmapped(ubi);

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
ZTEST(ubi_secure_error_handling_leb, leb_is_mapped_null)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_leb_is_mapped_null(ubi);

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
ZTEST(ubi_secure_error_handling_leb, leb_get_size_null)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_leb_get_size_null(ubi);

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
ZTEST(ubi_secure_error_handling_leb, leb_write_overwrite)
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
ZTEST(ubi_secure_error_handling_leb, leb_read_with_offset)
{
	struct ubi_device *const ubi = sec_init();

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
ZTEST(ubi_secure_error_handling_leb, leb_write_out_of_range_lnum)
{
	struct ubi_device *const ubi = sec_init();

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
ZTEST(ubi_secure_error_handling_leb, leb_read_out_of_range_lnum)
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
ZTEST(ubi_secure_error_handling_leb, leb_map_then_write)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_leb_map_then_write(ubi);

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
ZTEST(ubi_secure_error_handling_leb, leb_write_no_volumes)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_leb_write_no_volumes(ubi);

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
ZTEST(ubi_secure_error_handling_leb, leb_read_no_volumes)
{
	struct ubi_device *const ubi = sec_init();

	uint8_t rdata[1] = { 0 };

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
ZTEST(ubi_secure_error_handling_leb, leb_unmap_out_of_range)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_leb_unmap_out_of_range(ubi);

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
ZTEST(ubi_secure_error_handling_leb, leb_unmap_no_volumes)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_leb_unmap_no_volumes(ubi);

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
ZTEST(ubi_secure_error_handling_leb, leb_is_mapped_no_volumes)
{
	struct ubi_device *const ubi = sec_init();

	bool mapped = false;

	zassert_equal(-ENOENT, ubi_leb_is_mapped(ubi, 0, 0, &mapped));

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
ZTEST(ubi_secure_error_handling_leb, leb_get_size_no_volumes)
{
	struct ubi_device *const ubi = sec_init();

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
ZTEST(ubi_secure_error_handling_leb, leb_write_vol_not_found)
{
	struct ubi_device *const ubi = sec_init();

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
ZTEST(ubi_secure_error_handling_leb, leb_read_vol_not_found)
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
ZTEST(ubi_secure_error_handling_leb, leb_unmap_vol_not_found)
{
	struct ubi_device *const ubi = sec_init();

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
ZTEST(ubi_secure_error_handling_leb, leb_is_mapped_vol_not_found)
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
ZTEST(ubi_secure_error_handling_leb, leb_get_size_vol_not_found)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_leb_get_size_vol_not_found(ubi);

	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that ubi_leb_read() rejects an out-of-range LEB number.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs. Attempt to read LEB 5.
 *
 * \expect Returns -EACCES.
 */
ZTEST(ubi_secure_error_handling_leb, leb_read_out_of_range)
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
ZTEST(ubi_secure_error_handling_leb, leb_is_mapped_out_of_range)
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

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that ubi_leb_get_size() rejects an out-of-range LEB number.
 *
 * \details Scenario: Create a dynamic volume with 2 LEBs. Attempt to get size of LEB 5.
 *
 * \expect Returns -EACCES.
 */
ZTEST(ubi_secure_error_handling_leb, leb_get_size_out_of_range)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_leb_get_size_out_of_range(ubi);

	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that unmapping an unmapped LEB twice is safe (idempotent).
 *
 * \details Scenario: Create a volume but do not write any data. Unmap LEB 0 twice.
 *
 * \expect Both calls return 0.
 */
ZTEST(ubi_secure_error_handling_leb, leb_unmap_unmapped_is_idempotent)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_leb_unmap_unmapped_is_idempotent(ubi);

	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that mapping an already-mapped LEB is a no-op.
 *
 * \details Scenario: Map a LEB, then call ubi_leb_map() again on the same LEB.
 *
 * \expect Second map returns 0 without consuming an extra PEB.
 */
ZTEST(ubi_secure_error_handling_leb, leb_map_already_mapped_is_noop)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_leb_map_already_mapped_is_noop(ubi);

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
ZTEST(ubi_secure_error_handling_leb, leb_get_size_unmapped)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_leb_get_size_unmapped(ubi);

	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify reading last byte at exact data boundary.
 *
 * \details Scenario: Write 8 bytes. Read last 1 byte at offset 7.
 *
 * \expect Read returns the correct last byte.
 */
ZTEST(ubi_secure_error_handling_leb, leb_read_last_byte_at_boundary)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_leb_read_last_byte_at_boundary(ubi);

	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify that reading beyond data_size returns -EINVAL.
 *
 * \details Scenario: Write 4 bytes. Attempt to read 8 bytes.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling_leb, leb_read_beyond_data_size)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_leb_read_beyond_data_size(ubi);

	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief Verify leb_unmap on already-unmapped LEB is idempotent (write+unmap).
 *
 * \details Scenario: Write data to LEB 0, unmap it, then unmap again.
 *
 * \expect Both unmap calls return 0.
 */
ZTEST(ubi_secure_error_handling_leb, leb_unmap_already_unmapped_idempotent)
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

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief LEB read beyond written data_size returns -EINVAL (detailed).
 *
 * \details Scenario: Write N bytes to a LEB. Read with offset+len exceeding stored size.
 *
 * \expect Returns -EINVAL.
 */
ZTEST(ubi_secure_error_handling_leb, leb_read_beyond_data_size_returns_einval)
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

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief LEB map on non-existent volume returns -ENOENT.
 *
 * \details Scenario: Call ubi_leb_map with a non-existent vol_id.
 *
 * \expect Returns -ENOENT.
 */
ZTEST(ubi_secure_error_handling_leb, leb_map_vol_not_found)
{
	struct ubi_device *const ubi = sec_init();

	ubi_contract_leb_map_vol_not_found(ubi);

	zassert_ok(ubi_device_deinit(ubi));
}
/**
 * \brief LEB map with lnum exceeding volume capacity returns error.
 *
 * \details Scenario: Call ubi_leb_map with lnum >= leb_count.
 *
 * \expect Returns non-zero error.
 */
ZTEST(ubi_secure_error_handling_leb, leb_map_lnum_out_of_range)
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

	zassert_ok(ubi_device_deinit(ubi));
}
