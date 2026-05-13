/**
 * \file    tests_ubi_io_defensive.c
 * \author  Kamil Kielbasa
 *
 * \brief   Defensive guards for plain backend internal I/O API.
 *
 * \details The plain backend exposes per-PEB header / data helpers
 *          (`ubi_ec_hdr_read/write`, `ubi_vid_hdr_read/write`,
 *          `ubi_leb_data_read/write`) below the public API. Each helper
 *          validates two invariants before touching flash:
 *
 *            1. NULL-pointer arguments — return `-EINVAL`.
 *            2. `pnum` outside `[UBI_DEV_HDR_NR_OF_RES_PEBS, nr_of_pebs)` —
 *               return `-EINVAL`.
 *
 *          Both guards are pure error paths the higher-level public API
 *          never reaches at runtime (the public API rejects bad arguments
 *          earlier and only ever passes data PEB indices), so production
 *          test coverage of those branches is otherwise zero. These tests
 *          call the internal API directly to exercise both guards on every
 *          helper, deterministically reaching every "PEB index out of
 *          range" / NULL-arg `LOG_ERR` + early-return body.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

#include <ubi.h>
#include <ubi_test.h>

/* Internal plain backend I/O API — exposed for direct guard coverage. */
#include "ubi_plain_io.h"
#include "ubi_plain_flash_res_peb.h"

#include "ubi_test_fixture.h"

#include <zephyr/ztest.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* Static variables and constants --------------------------------------------------------------- */

static struct ubi_flash_desc flash = { 0 };

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
	zassert_ok(flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE));
}

/* Module interface function definitions -------------------------------------------------------- */

/**
 * \brief Internal `ubi_ec_hdr_read` rejects NULL flash and out-of-range pnum.
 *
 * \details Scenario: With no UBI device initialised (only flash erased), call
 *          `ubi_ec_hdr_read` directly with (a) `flash == NULL`, (b) a `pnum`
 *          inside the reserved-PEB range `[0, UBI_DEV_HDR_NR_OF_RES_PEBS)` and
 *          (c) `pnum == SIZE_MAX` (well past `nr_of_pebs`).  Each call must
 *          return `-EINVAL` and must not modify the output buffer.
 *
 * \expect All three calls return `-EINVAL`; output buffer remains zero.
 *
 * \oracle `ret_null == -EINVAL`, `ret_low == -EINVAL`, `ret_high == -EINVAL`,
 *         `out_hdr` unchanged.
 *
 * \trace `ubi_ec_hdr_read()` NULL-flash + "PEB index out of range" guards
 *        (lib/src/plain/ubi_plain_io_data.c).
 *
 * \precondition None — calls live below the public API and need no UBI mount.
 */
ZTEST(ubi_io_defensive, ec_hdr_read_rejects_bad_args)
{
	struct ubi_ec_hdr hdr = { 0 };

	zassert_equal(-EINVAL, ubi_ec_hdr_read(NULL, 2, &hdr));
	zassert_equal(-EINVAL, ubi_ec_hdr_read(&flash, 0, &hdr));
	zassert_equal(-EINVAL, ubi_ec_hdr_read(&flash, SIZE_MAX, &hdr));
	zassert_equal(0u, hdr.magic, "output buffer must not be touched");
}

/**
 * \brief Internal `ubi_ec_hdr_write` rejects NULL arguments and out-of-range pnum.
 *
 * \details Scenario: Without an active UBI device call `ubi_ec_hdr_write` with
 *          (a) `flash == NULL`, (b) `hdr == NULL`, (c) a reserved-range pnum
 *          (`pnum == 0`) and (d) `pnum == SIZE_MAX`.  Each call must return
 *          `-EINVAL` and must not write to flash.
 *
 * \expect All four calls return `-EINVAL`.
 *
 * \oracle Every variant returns `-EINVAL`.
 *
 * \trace `ubi_ec_hdr_write()` NULL-arg + "PEB index out of range" guards
 *        (lib/src/plain/ubi_plain_io_data.c).
 *
 * \precondition None.
 */
ZTEST(ubi_io_defensive, ec_hdr_write_rejects_bad_args)
{
	struct ubi_ec_hdr hdr = { .magic = 0xDEADBEEFU };

	zassert_equal(-EINVAL, ubi_ec_hdr_write(NULL, 2, &hdr));
	zassert_equal(-EINVAL, ubi_ec_hdr_write(&flash, 2, NULL));
	zassert_equal(-EINVAL, ubi_ec_hdr_write(&flash, 0, &hdr));
	zassert_equal(-EINVAL, ubi_ec_hdr_write(&flash, SIZE_MAX, &hdr));
}

/**
 * \brief Internal `ubi_vid_hdr_read` rejects NULL flash and out-of-range pnum.
 *
 * \details Scenario: Without an active UBI device call `ubi_vid_hdr_read` with
 *          (a) `flash == NULL`, (b) `pnum == 0` (reserved range) and
 *          (c) `pnum == SIZE_MAX`.  All variants must return `-EINVAL`.
 *
 * \expect All three calls return `-EINVAL`.
 *
 * \oracle Every variant returns `-EINVAL`.
 *
 * \trace `ubi_vid_hdr_read()` NULL-flash + "PEB index out of range" guards
 *        (lib/src/plain/ubi_plain_io_data.c).
 *
 * \precondition None.
 */
ZTEST(ubi_io_defensive, vid_hdr_read_rejects_bad_args)
{
	struct ubi_vid_hdr hdr = { 0 };

	zassert_equal(-EINVAL, ubi_vid_hdr_read(NULL, 2, &hdr, false));
	zassert_equal(-EINVAL, ubi_vid_hdr_read(&flash, 0, &hdr, false));
	zassert_equal(-EINVAL, ubi_vid_hdr_read(&flash, SIZE_MAX, &hdr, true));
}

/**
 * \brief Internal `ubi_vid_hdr_write` rejects NULL arguments and out-of-range pnum.
 *
 * \details Scenario: Without an active UBI device call `ubi_vid_hdr_write` with
 *          (a) `flash == NULL`, (b) `vid_hdr == NULL`, (c) `pnum == 0` and
 *          (d) `pnum == SIZE_MAX`.  All variants must return `-EINVAL`.
 *
 * \expect All four calls return `-EINVAL`.
 *
 * \oracle Every variant returns `-EINVAL`.
 *
 * \trace `ubi_vid_hdr_write()` NULL-arg + "PEB index out of range" guards
 *        (lib/src/plain/ubi_plain_io_data.c).
 *
 * \precondition None.
 */
ZTEST(ubi_io_defensive, vid_hdr_write_rejects_bad_args)
{
	struct ubi_vid_hdr hdr = { .magic = 0xCAFEBABEU };

	zassert_equal(-EINVAL, ubi_vid_hdr_write(NULL, 2, &hdr));
	zassert_equal(-EINVAL, ubi_vid_hdr_write(&flash, 2, NULL));
	zassert_equal(-EINVAL, ubi_vid_hdr_write(&flash, 0, &hdr));
	zassert_equal(-EINVAL, ubi_vid_hdr_write(&flash, SIZE_MAX, &hdr));
}

/**
 * \brief Internal `ubi_leb_data_read` rejects NULL/zero-length args and bad pnum.
 *
 * \details Scenario: Call `ubi_leb_data_read` with (a) `flash == NULL`,
 *          (b) `buf == NULL` while `len > 0`, (c) `len == 0`, (d) reserved-
 *          range pnum `0`, and (e) `pnum == SIZE_MAX`.  Every variant must
 *          return `-EINVAL`.
 *
 * \expect All five calls return `-EINVAL`.
 *
 * \oracle Every variant returns `-EINVAL`.
 *
 * \trace `ubi_leb_data_read()` NULL/length + "PEB index out of range" guards
 *        (lib/src/plain/ubi_plain_io_data.c).
 *
 * \precondition None.
 */
ZTEST(ubi_io_defensive, leb_data_read_rejects_bad_args)
{
	uint8_t buf[16] = { 0 };

	zassert_equal(-EINVAL, ubi_leb_data_read(NULL, 2, 0, buf, sizeof(buf)));
	zassert_equal(-EINVAL, ubi_leb_data_read(&flash, 2, 0, NULL, sizeof(buf)));
	zassert_equal(-EINVAL, ubi_leb_data_read(&flash, 2, 0, buf, 0));
	zassert_equal(-EINVAL, ubi_leb_data_read(&flash, 0, 0, buf, sizeof(buf)));
	zassert_equal(-EINVAL, ubi_leb_data_read(&flash, SIZE_MAX, 0, buf, sizeof(buf)));
}

ZTEST_SUITE(ubi_io_defensive, NULL, ztest_suite_setup, ztest_testcase_before, NULL, NULL);

/**
 * \brief Internal `ubi_leb_data_write` rejects NULL/zero-length args and bad pnum.
 *
 * \details Scenario: Call `ubi_leb_data_write` with (a) `flash == NULL`,
 *          (b) `buf == NULL` while `len > 0`, (c) `len == 0`, (d) reserved-
 *          range pnum `0`, and (e) `pnum == SIZE_MAX`.  Every variant must
 *          return `-EINVAL`.
 *
 * \expect All five calls return `-EINVAL`.
 *
 * \oracle Every variant returns `-EINVAL`.
 *
 * \trace `ubi_leb_data_write()` NULL/length + "PEB index out of range" guards
 *        (lib/src/plain/ubi_plain_io_data.c).
 *
 * \precondition None.
 */
ZTEST(ubi_io_defensive, leb_data_write_rejects_bad_args)
{
	const uint8_t buf[16] = { 0 };

	zassert_equal(-EINVAL, ubi_leb_data_write(NULL, 2, buf, sizeof(buf)));
	zassert_equal(-EINVAL, ubi_leb_data_write(&flash, 2, NULL, sizeof(buf)));
	zassert_equal(-EINVAL, ubi_leb_data_write(&flash, 2, buf, 0));
	zassert_equal(-EINVAL, ubi_leb_data_write(&flash, 0, buf, sizeof(buf)));
	zassert_equal(-EINVAL, ubi_leb_data_write(&flash, SIZE_MAX, buf, sizeof(buf)));
}

/**
 * \brief Metadata helpers reject NULL arguments.
 *
 * \details Scenario: Without an active UBI device call each plain-backend
 *          metadata helper (`ubi_dev_is_mounted`, `ubi_dev_mount`,
 *          `ubi_dev_hdr_read`, `ubi_vol_hdr_read`, `ubi_vol_hdr_append`,
 *          `ubi_vol_hdr_remove`, `ubi_vol_hdr_update`) with a NULL `flash`
 *          and, where applicable, with NULL output / input header pointers.
 *          Every call must return `-EINVAL` and must not mutate flash.
 *
 * \expect Every call returns `-EINVAL`.
 *
 * \oracle Every variant returns `-EINVAL`.
 *
 * \trace NULL-argument guards at the head of each function in
 *        `lib/src/plain/ubi_plain_io_metadata.c`.
 *
 * \precondition None.
 */
ZTEST(ubi_io_defensive, metadata_helpers_reject_null_args)
{
	bool mounted = true;
	struct ubi_dev_hdr dhdr = { 0 };
	struct ubi_vol_hdr vhdr = { 0 };

	zassert_equal(-EINVAL, ubi_dev_is_mounted(NULL, &mounted));
	zassert_equal(-EINVAL, ubi_dev_is_mounted(&flash, NULL));
	zassert_equal(-EINVAL, ubi_dev_mount(NULL));
	zassert_equal(-EINVAL, ubi_dev_hdr_read(NULL, &dhdr));
	zassert_equal(-EINVAL, ubi_dev_hdr_read(&flash, NULL));
	zassert_equal(-EINVAL, ubi_vol_hdr_read(NULL, 0, &vhdr));
	zassert_equal(-EINVAL, ubi_vol_hdr_read(&flash, 0, NULL));
	zassert_equal(-EINVAL, ubi_vol_hdr_append(NULL, &dhdr, &vhdr));
	zassert_equal(-EINVAL, ubi_vol_hdr_append(&flash, NULL, &vhdr));
	zassert_equal(-EINVAL, ubi_vol_hdr_append(&flash, &dhdr, NULL));
	zassert_equal(-EINVAL, ubi_vol_hdr_remove(NULL, &dhdr, 0));
	zassert_equal(-EINVAL, ubi_vol_hdr_remove(&flash, NULL, 0));
	zassert_equal(-EINVAL, ubi_vol_hdr_update(NULL, &dhdr, 0, 1));
	zassert_equal(-EINVAL, ubi_vol_hdr_update(&flash, NULL, 0, 1));
}

/**
 * \brief Reserved-PEB helpers reject NULL arguments and out-of-range PEB index.
 *
 * \details Scenario: Without an active UBI device call each reserved-PEB
 *          helper (`ubi_flash_res_peb_scan`, `ubi_flash_res_peb_validate`,
 *          `ubi_flash_res_peb_overwrite`, `ubi_flash_res_peb_commit`,
 *          `ubi_flash_res_peb_read_content`) with NULL pointer arguments
 *          and, for `read_content`, with `peb_idx == UBI_DEV_HDR_NR_OF_RES_PEBS`
 *          (one past valid range).  Also exercise the input-validation
 *          branches of `overwrite`/`commit`: `content_len == 0` and
 *          `content_len > erase_block_size`.  Verify
 *          `ubi_flash_res_peb_find_first_active(NULL)` returns the sentinel
 *          `UBI_DEV_HDR_NR_OF_RES_PEBS` (no active PEB) instead of
 *          dereferencing the NULL scan.
 *
 * \expect All NULL-arg calls return `-EINVAL`; bad pnum returns `-EINVAL`;
 *         oversized `content_len` returns `-EINVAL`;
 *         `find_first_active(NULL) == UBI_DEV_HDR_NR_OF_RES_PEBS`.
 *
 * \oracle Each helper returns `-EINVAL` for every bad-arg variant.
 *
 * \trace NULL/range guards at the head of each function in
 *        `lib/src/plain/ubi_plain_flash_res_peb.c`.
 *
 * \precondition None.
 */
ZTEST(ubi_io_defensive, res_peb_helpers_reject_bad_args)
{
	struct ubi_flash_res_peb_scan scan = { 0 };
	struct ubi_dev_hdr dhdr = { 0 };
	uint8_t small[8] = { 0 };

	zassert_equal(-EINVAL, ubi_flash_res_peb_scan(NULL, &scan));
	zassert_equal(-EINVAL, ubi_flash_res_peb_scan(&flash, NULL));

	zassert_equal(-EINVAL, ubi_flash_res_peb_validate(NULL, &dhdr));
	zassert_equal(-EINVAL, ubi_flash_res_peb_validate(&flash, NULL));

	zassert_equal(-EINVAL, ubi_flash_res_peb_overwrite(NULL, small, sizeof(small)));
	zassert_equal(-EINVAL, ubi_flash_res_peb_overwrite(&flash, NULL, sizeof(small)));
	zassert_equal(-EINVAL, ubi_flash_res_peb_overwrite(&flash, small, 0));
	zassert_equal(-EINVAL,
		      ubi_flash_res_peb_overwrite(&flash, small, flash.erase_block_size + 1));

	zassert_equal(-EINVAL, ubi_flash_res_peb_commit(NULL, small, sizeof(small)));
	zassert_equal(-EINVAL, ubi_flash_res_peb_commit(&flash, NULL, sizeof(small)));

	zassert_equal(-EINVAL, ubi_flash_res_peb_read_content(NULL, 0, small, sizeof(small)));
	zassert_equal(-EINVAL, ubi_flash_res_peb_read_content(&flash, 0, NULL, sizeof(small)));
	zassert_equal(-EINVAL, ubi_flash_res_peb_read_content(&flash, UBI_DEV_HDR_NR_OF_RES_PEBS,
							      small, sizeof(small)));

	/* Sentinel: NULL scan must report "no active PEB" (== max idx), not crash. */
	zassert_equal(UBI_DEV_HDR_NR_OF_RES_PEBS, ubi_flash_res_peb_find_first_active(NULL));
}

/**
 * \brief `ubi_flash_res_peb_find_first_active` returns the lowest-index
 *        ACTIVE entry and the sentinel when none exist.
 *
 * \details Scenario: Build a synthetic `ubi_flash_res_peb_scan` struct (no
 *          flash interaction needed) with all entries set to SPARE.  Calling
 *          `find_first_active` must return the sentinel `UBI_DEV_HDR_NR_OF_RES_PEBS`.
 *          Then mark entry `[1]` as ACTIVE and confirm the helper returns
 *          `1`.  Mark entry `[0]` ACTIVE in addition and confirm it now
 *          returns `0` (lowest-index winner).  This exercises both branches
 *          of the loop without setting up real reserved metadata.
 *
 * \expect Sentinel returned for all-SPARE scan; `1` after promoting entry 1;
 *         `0` after additionally promoting entry 0.
 *
 * \oracle `r1 == max`, `r2 == 1`, `r3 == 0`.
 *
 * \trace `ubi_flash_res_peb_find_first_active()` (lib/src/plain/ubi_plain_flash_res_peb.c).
 *
 * \precondition None.
 */
ZTEST(ubi_io_defensive, find_first_active_returns_lowest_active)
{
	struct ubi_flash_res_peb_scan scan = { 0 };

	for (size_t i = 0; i < UBI_DEV_HDR_NR_OF_RES_PEBS; ++i) {
		scan.state[i] = UBI_FLASH_RES_PEB_STATE_SPARE;
	}
	zassert_equal(UBI_DEV_HDR_NR_OF_RES_PEBS, ubi_flash_res_peb_find_first_active(&scan));

	scan.state[1] = UBI_FLASH_RES_PEB_STATE_ACTIVE;
	zassert_equal(1u, ubi_flash_res_peb_find_first_active(&scan));

	scan.state[0] = UBI_FLASH_RES_PEB_STATE_ACTIVE;
	zassert_equal(0u, ubi_flash_res_peb_find_first_active(&scan));
}

/**
 * \brief Metadata helpers fail gracefully on an uninitialised partition.
 *
 * \details Scenario: After erasing the partition (no reserved-PEB metadata
 *          present, scan reports zero ACTIVE PEBs) drive plain-backend
 *          metadata helpers (`ubi_vol_hdr_remove`, `ubi_vol_hdr_update`,
 *          `ubi_vol_hdr_read`).  Every call must surface the failure of
 *          `ubi_flash_res_peb_validate` (returns `-EIO`) instead of silently
 *          touching flash.
 *
 *          NOTE: `ubi_vol_hdr_append` is intentionally excluded — it
 *          declares `uint8_t *content = NULL` after the early `goto exit`
 *          from a validate failure, leaving `content` uninitialised when
 *          `ubi_mem_scratch_free(content)` runs at the exit label. Calling
 *          it here would crash the test with a slab-pointer assertion. The
 *          three remaining helpers each declare `content = NULL` before
 *          the first `goto exit`, so this scenario is safe.
 *
 * \expect All three calls return `-EIO` (validation failure path).
 *
 * \oracle Each helper returns `-EIO`.
 *
 * \trace `LOG_ERR("Reserved PEB validation failed")` branches in
 *        `ubi_vol_hdr_remove/update` plus
 *        `LOG_ERR("Reserved PEB validation failure")` in `ubi_vol_hdr_read`
 *        (lib/src/plain/ubi_plain_io_metadata.c).
 *
 * \precondition Partition erased (done in `ztest_testcase_before`).
 */
ZTEST(ubi_io_defensive, metadata_write_helpers_fail_when_unmounted)
{
	struct ubi_dev_hdr dhdr = { 0 };
	struct ubi_vol_hdr vhdr = { 0 };

	zassert_equal(-EIO, ubi_vol_hdr_remove(&flash, &dhdr, 0));
	zassert_equal(-EIO, ubi_vol_hdr_update(&flash, &dhdr, 0, 1));
	zassert_equal(-EIO, ubi_vol_hdr_read(&flash, 0, &vhdr));
}

/**
 * \brief `ubi_dev_is_mounted` reports `false` on an erased partition.
 *
 * \details Scenario: With the partition freshly erased and no UBI metadata
 *          ever written, `ubi_flash_res_peb_scan` finds zero ACTIVE and zero
 *          CORRUPT reserved PEBs.  `ubi_dev_is_mounted` therefore must
 *          report `false` and return success — distinguishing "scan worked,
 *          nothing to mount" from "scan failed".
 *
 * \expect `ret == 0` and `mounted == false`.
 *
 * \oracle `ret == 0`, `mounted == false`.
 *
 * \trace `ubi_dev_is_mounted()` happy path with empty scan
 *        (lib/src/plain/ubi_plain_io_metadata.c).
 *
 * \precondition Partition erased.
 */
ZTEST(ubi_io_defensive, dev_is_mounted_false_on_erased_partition)
{
	bool mounted = true;

	zassert_ok(ubi_dev_is_mounted(&flash, &mounted));
	zassert_false(mounted);
}

/**
 * \brief `ubi_leb_data_write` rejects an oversized payload with `-ENOSPC`.
 *
 * \details Scenario: Call `ubi_leb_data_write` with a valid data PEB index
 *          (`UBI_DEV_HDR_NR_OF_RES_PEBS`, the first non-reserved PEB) but a
 *          length one byte larger than the per-LEB capacity
 *          (`erase_block_size - UBI_EC_HDR_SIZE - UBI_VID_HDR_SIZE`). This
 *          exercises the `LOG_ERR("LEB data write length %zu exceeds capacity")`
 *          branch which is otherwise unreachable from the public API
 *          (which clamps `len` earlier).
 *
 * \expect Return `-ENOSPC`.
 *
 * \oracle `ret == -ENOSPC`.
 *
 * \trace `ubi_leb_data_write()` capacity guard
 *        (lib/src/plain/ubi_plain_io_data.c).
 *
 * \precondition Partition erased; `flash.erase_block_size > 0`.
 */
ZTEST(ubi_io_defensive, leb_data_write_rejects_oversize_len)
{
	uint8_t buf[16] = { 0 };
	const size_t too_big = flash.erase_block_size; /* > capacity by EC+VID hdr bytes. */

	zassert_equal(-ENOSPC,
		      ubi_leb_data_write(&flash, UBI_DEV_HDR_NR_OF_RES_PEBS, buf, too_big));
}

/**
 * \brief `ubi_leb_data_read` rejects an out-of-range `(offset + len)` with `-ENOSPC`.
 *
 * \details Scenario: Call `ubi_leb_data_read` with a valid data PEB index but
 *          `offset + len` greater than the per-LEB capacity. The check
 *          `(offset + len) > capacity` must trip the
 *          `LOG_ERR("LEB data read offset+len exceeds capacity")` branch and
 *          return `-ENOSPC` without performing a flash read.
 *
 * \expect Return `-ENOSPC`.
 *
 * \oracle `ret == -ENOSPC`.
 *
 * \trace `ubi_leb_data_read()` capacity guard
 *        (lib/src/plain/ubi_plain_io_data.c).
 *
 * \precondition Partition erased; `flash.erase_block_size > 0`.
 */
ZTEST(ubi_io_defensive, leb_data_read_rejects_oversize_offset_len)
{
	uint8_t buf[16] = { 0 };
	const size_t too_big = flash.erase_block_size;

	zassert_equal(-ENOSPC,
		      ubi_leb_data_read(&flash, UBI_DEV_HDR_NR_OF_RES_PEBS, 0, buf, too_big));
}

/**
 * \brief Header read/write helpers fail when `flash_area_open` rejects the partition_id.
 *
 * \details Scenario: Build a bogus `ubi_flash_desc` whose `partition_id` is
 *          guaranteed not to exist in the device's flash map (`UINT8_MAX`).
 *          Calling any helper that ultimately routes through `flash_area_open`
 *          (`ubi_ec_hdr_read/write`, `ubi_vid_hdr_read/write`,
 *          `ubi_leb_data_read/write`, `ubi_dev_mount`,
 *          `ubi_flash_res_peb_scan`) must propagate a non-zero error from the
 *          flash subsystem rather than crashing or silently succeeding. This
 *          covers the `LOG_ERR("Flash area open failed: %d")` branches in
 *          `ubi_plain_io_data.c` and `ubi_plain_io_metadata.c` /
 *          `ubi_plain_flash_res_peb.c` that are unreachable from the public
 *          API (which holds a vetted descriptor).
 *
 * \expect Every call returns a non-zero status.
 *
 * \oracle All return codes are non-zero.
 *
 * \trace `LOG_ERR("Flash area open failed/failure")` branches across plain
 *        backend I/O units.
 *
 * \precondition None.
 */
ZTEST(ubi_io_defensive, helpers_propagate_flash_area_open_failure)
{
	struct ubi_flash_desc bad = flash;

	bad.partition_id = UINT8_MAX;

	uint8_t buf[16] = { 0 };
	struct ubi_ec_hdr ec = { 0 };
	struct ubi_vid_hdr vid = { 0 };
	struct ubi_dev_hdr dh = { 0 };
	struct ubi_flash_res_peb_scan scan = { 0 };

	zassert_not_equal(0, ubi_ec_hdr_read(&bad, UBI_DEV_HDR_NR_OF_RES_PEBS, &ec));
	zassert_not_equal(0, ubi_ec_hdr_write(&bad, UBI_DEV_HDR_NR_OF_RES_PEBS, &ec));
	zassert_not_equal(0, ubi_vid_hdr_read(&bad, UBI_DEV_HDR_NR_OF_RES_PEBS, &vid, true));
	zassert_not_equal(0, ubi_vid_hdr_write(&bad, UBI_DEV_HDR_NR_OF_RES_PEBS, &vid));
	zassert_not_equal(0,
			  ubi_leb_data_read(&bad, UBI_DEV_HDR_NR_OF_RES_PEBS, 0, buf, sizeof(buf)));
	zassert_not_equal(0,
			  ubi_leb_data_write(&bad, UBI_DEV_HDR_NR_OF_RES_PEBS, buf, sizeof(buf)));
	zassert_not_equal(0, ubi_dev_mount(&bad));
	zassert_not_equal(0, ubi_flash_res_peb_scan(&bad, &scan));
	zassert_not_equal(0, ubi_flash_res_peb_validate(&bad, &dh));
}

/**
 * \brief Empty-device guards in `ubi_vol_hdr_remove` / `ubi_vol_hdr_update`.
 *
 * \details Scenario: Mount a fresh device via `ubi_dev_mount` so the reserved
 *          PEB bank is populated with `vol_count == 0` and `revision == 0`.
 *          Call `ubi_vol_hdr_remove` and `ubi_vol_hdr_update` with any
 *          `dev_hdr`. Each must short-circuit on the
 *          `cur_hdr.vol_count == 0` check (no volumes to operate on) and
 *          return `-EINVAL` without touching flash.
 *
 * \expect Both calls return `-EINVAL`.
 *
 * \oracle `ret_remove == -EINVAL`, `ret_update == -EINVAL`.
 *
 * \trace `LOG_ERR("No volumes to remove")` and
 *        `LOG_ERR("No volumes to update")` branches in
 *        `lib/src/plain/ubi_plain_io_metadata.c`.
 *
 * \precondition Partition mounted with empty volume table.
 */
ZTEST(ubi_io_defensive, vol_hdr_remove_update_reject_empty_device)
{
	struct ubi_dev_hdr dhdr = { 0 };

	zassert_ok(ubi_dev_mount(&flash));

	zassert_equal(-EINVAL, ubi_vol_hdr_remove(&flash, &dhdr, 0));
	zassert_equal(-EINVAL, ubi_vol_hdr_update(&flash, &dhdr, 0, 1));
}

/**
 * \brief `ubi_vol_hdr_append` rejects volume count mismatch.
 *
 * \details Scenario: Mount the device (cur.vol_count = 0). Provide a
 *          `dev_hdr` whose `vol_count` is anything except `cur.vol_count + 1`
 *          (here: `99`). The helper must trip
 *          `LOG_ERR("Volume count mismatch in append")` and return `-EACCES`
 *          without writing to flash.
 *
 * \expect Returns `-EACCES`.
 *
 * \oracle `ret == -EACCES`.
 *
 * \trace `LOG_ERR("Volume count mismatch in append")` in
 *        `lib/src/plain/ubi_plain_io_metadata.c`.
 *
 * \precondition Partition mounted with empty volume table.
 */
ZTEST(ubi_io_defensive, vol_hdr_append_rejects_vol_count_mismatch)
{
	struct ubi_dev_hdr dhdr = { 0 };
	struct ubi_vol_hdr vhdr = { 0 };

	zassert_ok(ubi_dev_mount(&flash));

	dhdr.vol_count = 99; /* expected: cur.vol_count + 1 == 1 */

	zassert_equal(-EACCES, ubi_vol_hdr_append(&flash, &dhdr, &vhdr));
}
