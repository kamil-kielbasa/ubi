/**
 * \file    tests_ubi_erased_val.c
 *
 * \author Kamil Kielbasa
 *
 * \brief   Tests for erased-value abstraction helpers (Task C).
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

#include <ubi.h>
#include <ubi_test.h>

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

#include <stdint.h>
#include <string.h>

#include "ubi_test_fixture.h"
#include "ubi_test_memory.h"

/* Module defines ------------------------------------------------------------------------------- */

/* Module types and type definitiones ----------------------------------------------------------- */

/* Module interface variables and constants ----------------------------------------------------- */

/* Static variables and constants --------------------------------------------------------------- */

/* Static function declarations ----------------------------------------------------------------- */
static struct ubi_flash_desc flash = { 0 };
static struct ubi_device *g_ubi = NULL;

/* Static function definitions ------------------------------------------------------------------ */
static void *ztest_suite_setup(void)
{
	ubi_test_setup_mtd(&flash);
	return NULL;
}

static void ztest_testcase_before(void *ctx)
{
	(void)ctx;
	ubi_test_partition_force_release_all();
	ubi_test_fault_reset();
	ubi_test_erase_partition();
	g_ubi = NULL;
}

static void ztest_testcase_teardown(void *ctx)
{
	(void)ctx;
	ubi_test_fault_reset();
	if (g_ubi) {
		ubi_device_deinit(g_ubi);
		g_ubi = NULL;
	}
}

/* Module interface function definitions -------------------------------------------------------- */
/**
 * \brief ubi_test_buf_is_erased detects a buffer filled with 0xFF.
 *
 * \details Scenario: Allocate a 64-byte buffer, fill with 0xFF, call
 *          ubi_test_buf_is_erased with erased_val=0xFF.
 *
 * \expect Returns true; the buffer is classified as erased.
 */
ZTEST(ubi_erased_val, test_erased_buffer_check_0xff)
{
	uint8_t buf[64] = { 0 };
	memset(buf, 0xFF, sizeof(buf));

	zassert_true(ubi_test_buf_is_erased(buf, sizeof(buf), 0xFF),
		     "Expected all-0xFF buffer to be erased");
}

/**
 * \brief ubi_test_buf_is_erased detects a buffer filled with 0x00.
 *
 * \details Scenario: Allocate a 64-byte buffer, fill with 0x00, call
 *          ubi_test_buf_is_erased with erased_val=0x00.
 *
 * \expect Returns true; the buffer is classified as erased.
 */
ZTEST(ubi_erased_val, test_erased_buffer_check_0x00)
{
	uint8_t buf[64] = { 0 };
	memset(buf, 0x00, sizeof(buf));

	zassert_true(ubi_test_buf_is_erased(buf, sizeof(buf), 0x00),
		     "Expected all-0x00 buffer to be erased");
}

/**
 * \brief Mixed content is not classified as erased.
 *
 * \details Scenario: Allocate a 64-byte buffer, fill with 0xFF, set buf[32]=0x42, call
 *          ubi_test_buf_is_erased with erased_val=0xFF.
 *
 * \expect Returns false; mixed-content buffer not classified as erased.
 */
ZTEST(ubi_erased_val, test_erased_buffer_mixed_content_not_erased)
{
	uint8_t buf[64] = { 0 };
	memset(buf, 0xFF, sizeof(buf));
	buf[32] = 0x42;

	zassert_false(ubi_test_buf_is_erased(buf, sizeof(buf), 0xFF),
		      "Mixed-content buffer should not be erased");
}

/**
 * \brief Zero-length buffer is trivially erased.
 *
 * \details Scenario: Call ubi_test_buf_is_erased on a 0-byte range with erased_val=0xFF.
 *
 * \expect Returns true; zero-length range is trivially considered erased.
 */
ZTEST(ubi_erased_val, test_erased_buffer_zero_length)
{
	uint8_t buf[1] = { 0x42 };

	zassert_true(ubi_test_buf_is_erased(buf, 0, 0xFF),
		     "Zero-length buffer should be trivially erased");
}

/**
 * \brief ubi_test_get_erased_val returns the flash simulator's erased value (0xFF).
 *
 * \details Scenario: Call ubi_test_get_erased_val with the flash descriptor and an output
 *          parameter.
 *
 * \expect Returns 0; erased_val == 0xFF (flash simulator default).
 */
ZTEST(ubi_erased_val, test_get_erased_val_returns_flash_value)
{
	uint8_t erased_val = 0x00;
	int ret = ubi_test_get_erased_val(&flash, &erased_val);

	zassert_ok(ret, "ubi_test_get_erased_val should succeed");
	zassert_equal(erased_val, 0xFF, "Flash simulator erased value should be 0xFF");
}

/**
 * \brief Init correctly classifies erased PEBs as free (regression).
 *
 * This test verifies that after the erased-value abstraction change,
 * a freshly formatted device still has the expected number of free PEBs.
 *
 * \details Scenario: Initialize a fresh device on blank flash. Call ubi_device_get_info
 *          and inspect PEB classification fields.
 *
 * \expect free_peb_count == total_peb_count; dirty_peb_count == 0; bad_peb_count == 0.
 */
ZTEST(ubi_erased_val, test_init_classifies_erased_pebs_as_free)
{
	g_ubi = ubi_test_init_device(&flash);

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(g_ubi, &info));

	/* Fresh device: all data PEBs should be free. */
	zassert_equal(info.free_peb_count, info.total_peb_count,
		      "All data PEBs should be classified as free after fresh init");
	zassert_equal(info.dirty_peb_count, 0, "No dirty PEBs expected after fresh init");
	zassert_equal(info.bad_peb_count, 0, "No bad PEBs expected after fresh init");

	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;
}

ZTEST_SUITE(ubi_erased_val, NULL, ztest_suite_setup, ztest_testcase_before, ztest_testcase_teardown,
	    NULL);
