/**
 * \file    tests_ubi_partial_write.c
 *
 * \brief   Tests for in-place partial LEB update (ubi_leb_write_at) — the
 *          offset-based, non-atomic, dynamic-volume write that mirrors Linux
 *          UBI's ubi_leb_write. Covers accumulation across offsets, unwritten
 *          regions reading 0xFF, persistence across reattach, and rejection on
 *          static volumes.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

#include <ubi.h>

#include "ubi_test_fixture.h"

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>

#include <stdint.h>
#include <string.h>

/* Static variables and constants --------------------------------------------------------------- */

static struct ubi_flash_desc flash = { 0 };

/* Two non-adjacent payloads written into one LEB, leaving a gap between them
 * that must read back as erased flash (0xFF). Lengths and offsets are chosen
 * to be safe for the native_sim 1-byte write granularity. */
#define OFF_A 0u
#define LEN_A 64u
#define OFF_B 128u
#define LEN_B 64u

static uint8_t payload_a[LEN_A];
static uint8_t payload_b[LEN_B];

/* Static function definitions ------------------------------------------------------------------ */

static void *suite_setup(void)
{
	ubi_test_setup_mtd(&flash);

	for (size_t i = 0; i < LEN_A; ++i) {
		payload_a[i] = (uint8_t)(0xA0 ^ i);
	}
	for (size_t i = 0; i < LEN_B; ++i) {
		payload_b[i] = (uint8_t)(0x50 ^ i);
	}

	return NULL;
}

static void testcase_before(void *ctx)
{
	(void)ctx;
	ubi_test_erase_partition();
}

static int make_dynamic_volume(struct ubi_device *ubi, int *vol_id)
{
	const struct ubi_volume_config cfg = {
		.name = { '/', 'u', 'b', 'i', '_', 'p' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 4,
	};
	return ubi_volume_create(ubi, &cfg, vol_id);
}

/* Verify the LEB image after two partial writes: A at OFF_A, 0xFF gap, B at
 * OFF_B. */
static void assert_leb_image(struct ubi_device *ubi, int vol_id, int lnum)
{
	uint8_t buf[OFF_B + LEN_B];

	memset(buf, 0, sizeof(buf));
	zassert_ok(ubi_leb_read(ubi, vol_id, lnum, 0, buf, sizeof(buf)));

	zassert_mem_equal(&buf[OFF_A], payload_a, LEN_A, "range A mismatch");
	zassert_mem_equal(&buf[OFF_B], payload_b, LEN_B, "range B mismatch");

	for (size_t i = OFF_A + LEN_A; i < OFF_B; ++i) {
		zassert_equal(buf[i], 0xFF, "gap byte %zu not erased (0x%02x)", i, buf[i]);
	}
}

/* Test definitions ----------------------------------------------------------------------------- */

ZTEST_SUITE(ubi_partial_write, NULL, suite_setup, testcase_before, NULL, NULL);

/* Two in-place writes at different offsets accumulate in the same LEB, and the
 * untouched gap between them reads back as erased flash. */
ZTEST(ubi_partial_write, two_ranges_accumulate_with_ff_gap)
{
	struct ubi_device *ubi = NULL;
	int vol_id = -1;
	const int lnum = 1;

	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	zassert_not_null(ubi);
	zassert_ok(make_dynamic_volume(ubi, &vol_id));

	zassert_ok(ubi_leb_write_at(ubi, vol_id, lnum, OFF_A, payload_a, LEN_A));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, lnum, OFF_B, payload_b, LEN_B));

	assert_leb_image(ubi, vol_id, lnum);

	zassert_ok(ubi_device_deinit(ubi));
}

/* First partial write maps the LEB; reads of not-yet-written regions return
 * 0xFF rather than failing. */
ZTEST(ubi_partial_write, unwritten_region_reads_ff)
{
	struct ubi_device *ubi = NULL;
	int vol_id = -1;
	const int lnum = 0;

	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	zassert_ok(make_dynamic_volume(ubi, &vol_id));

	zassert_ok(ubi_leb_write_at(ubi, vol_id, lnum, OFF_A, payload_a, LEN_A));

	uint8_t tail[LEN_B];

	memset(tail, 0, sizeof(tail));
	zassert_ok(ubi_leb_read(ubi, vol_id, lnum, OFF_B, tail, sizeof(tail)));
	for (size_t i = 0; i < sizeof(tail); ++i) {
		zassert_equal(tail[i], 0xFF, "tail byte %zu not erased (0x%02x)", i, tail[i]);
	}

	zassert_ok(ubi_device_deinit(ubi));
}

/* Partial writes survive a detach/attach cycle (in-place data is committed to
 * the mapped PEB, recovered by the attach scan). */
ZTEST(ubi_partial_write, persists_across_reattach)
{
	struct ubi_device *ubi = NULL;
	int vol_id = -1;
	const int lnum = 2;

	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	zassert_ok(make_dynamic_volume(ubi, &vol_id));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, lnum, OFF_A, payload_a, LEN_A));
	zassert_ok(ubi_leb_write_at(ubi, vol_id, lnum, OFF_B, payload_b, LEN_B));
	zassert_ok(ubi_device_deinit(ubi));

	/* Reattach and re-read. */
	ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	zassert_not_null(ubi);
	assert_leb_image(ubi, vol_id, lnum);
	zassert_ok(ubi_device_deinit(ubi));
}

/* Partial in-place update is a dynamic-volume operation; static volumes must
 * reject it (they are whole-LEB and integrity-checked). */
ZTEST(ubi_partial_write, rejected_on_static_volume)
{
	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(ubi_device_init(&flash, NULL, &ubi));

	const struct ubi_volume_config cfg = {
		.name = { '/', 'u', 'b', 'i', '_', 's' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_equal(-EACCES, ubi_leb_write_at(ubi, vol_id, 0, OFF_A, payload_a, LEN_A));

	zassert_ok(ubi_device_deinit(ubi));
}
