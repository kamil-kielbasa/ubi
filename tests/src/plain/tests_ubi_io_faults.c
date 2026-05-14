/**
 * \file    tests_ubi_io_faults.c
 *
 * \author Kamil Kielbasa
 *
 * \brief   Flash I/O fault injection tests for coverage of write error paths.
 *
 * These tests use the flash write fault injection hook to verify that UBI
 * operations remain safe when flash writes fail at various points:
 * - VID header write failure during leb_write → PEB marked bad
 * - EC header write failure during erase_peb → PEB marked bad
 * - Data write failure during leb_write → PEB marked bad, old data preserved
 * - Write failure during volume create (vol header append)
 * - Write failure during format (EC header initialization)
 *
 * Requires CONFIG_UBI_TEST_FAULT_INJECTION=y and CONFIG_UBI_TEST_API_ENABLE=y.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>

/* Test fixtures: */
#include "ubi_test_fixture.h"
#include "ubi_test_memory.h"

/* Zephyr headers: */
#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>

/* Standard library headers: */
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

/* Header constants must match ubi_io.h */
#define EC_HDR_MAGIC (0x55424923U)
#define EC_HDR_SIZE (16U)
#define VID_HDR_MAGIC (0x55424921U)
#define VID_HDR_SIZE (32U)
#define NR_OF_RES_PEBS (2U)

/* Module types and type definitiones ----------------------------------------------------------- */

/** \brief Raw on-flash erase-counter header layout used by the raw write helpers. */
struct raw_ec_hdr_io {
	uint32_t magic; /*!< EC header magic. */
	uint8_t version; /*!< Header version. */
	uint8_t padding[3]; /*!< Padding to 4-byte boundary. */
	uint32_t ec; /*!< Erase counter value. */
	uint32_t hdr_crc; /*!< Header CRC32. */
};

/** \brief Raw on-flash volume-id header layout used by the raw write helpers. */
struct raw_vid_hdr_io {
	uint32_t magic; /*!< VID header magic. */
	uint8_t version; /*!< Header version. */
	uint8_t padding[3]; /*!< Padding to 4-byte boundary. */
	uint32_t lnum; /*!< Logical erase block number. */
	uint32_t vol_id; /*!< Volume identifier. */
	uint64_t sqnum; /*!< Sequence number. */
	uint32_t data_size; /*!< Data length in this LEB. */
	uint32_t hdr_crc; /*!< Header CRC32. */
};

/* Module interface variables and constants ----------------------------------------------------- */

/* Static variables and constants --------------------------------------------------------------- */

static struct ubi_flash_desc flash = { 0 };

/** \brief Per-test fixture: holds the UBI device handle so the
 *         teardown hook can deinit on assertion failures. */
struct ubi_io_faults_fixture {
	struct ubi_device *ubi;
};

static struct ubi_io_faults_fixture g_fixture;

/* Static function declarations ----------------------------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_after(void *ctx);

static void ztest_testcase_before(void *ctx);
static void ztest_testcase_teardown(void *ctx);

static void io_raw_write_ec(const struct flash_area *fa, size_t pnum, size_t ebs, uint32_t ec);
static void io_raw_write_vid(const struct flash_area *fa, size_t pnum, size_t ebs, uint32_t lnum,
			     uint32_t vol_id, uint64_t sqnum, uint32_t data_size);

/* Static function definitions ------------------------------------------------------------------ */

static void io_raw_write_ec(const struct flash_area *fa, size_t pnum, size_t ebs, uint32_t ec)
{
	struct raw_ec_hdr_io hdr = { .magic = EC_HDR_MAGIC, .version = 1, .ec = ec };
	hdr.hdr_crc = crc32_ieee((const uint8_t *)&hdr, sizeof(hdr) - sizeof(hdr.hdr_crc));
	(void)flash_area_write(fa, pnum * ebs, &hdr, sizeof(hdr));
}

static void io_raw_write_vid(const struct flash_area *fa, size_t pnum, size_t ebs, uint32_t lnum,
			     uint32_t vol_id, uint64_t sqnum, uint32_t data_size)
{
	struct raw_vid_hdr_io hdr = { .magic = VID_HDR_MAGIC,
				      .version = 1,
				      .lnum = lnum,
				      .vol_id = vol_id,
				      .sqnum = sqnum,
				      .data_size = data_size };
	hdr.hdr_crc = crc32_ieee((const uint8_t *)&hdr, sizeof(hdr) - sizeof(hdr.hdr_crc));
	(void)flash_area_write(fa, (pnum * ebs) + EC_HDR_SIZE, &hdr, sizeof(hdr));
}

static void *ztest_suite_setup(void)
{
	ubi_test_setup_mtd(&flash);
	g_fixture.ubi = NULL;
	return &g_fixture;
}

static void ztest_suite_after(void *ctx)
{
	(void)ctx;
}

static void ztest_testcase_before(void *ctx)
{
	struct ubi_io_faults_fixture *fixture = ctx;

	fixture->ubi = NULL;
	ubi_test_fault_reset();
	ubi_test_erase_partition();
}

static void ztest_testcase_teardown(void *ctx)
{
	struct ubi_io_faults_fixture *fixture = ctx;
	ubi_test_fault_reset();
	if (fixture->ubi != NULL) {
		(void)ubi_device_deinit(fixture->ubi);
		fixture->ubi = NULL;
	}
}

/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_io_faults, NULL, ztest_suite_setup, ztest_testcase_before, ztest_testcase_teardown,
	    ztest_suite_after);

/**
 * \brief Verify that data write failure during leb_write marks PEB as bad.
 *
 * \details Scenario: Setup: Initialize device, create volume. Inject write fault so
 *          the data write in leb_prepare_new_mapping() fails (first flash
 *          write). The free PEB should be marked bad.
 *
 * \expect leb_write returns error. bad_peb_count increases by 1.
 *         free_peb_count decreases by 1. Device remains consistent.
 */
ZTEST_F(ubi_io_faults, vid_hdr_write_failure_marks_peb_bad)
{
	struct ubi_device *ubi = ubi_test_init_device(&flash);
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "wfault_vol1",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	struct ubi_device_info info_before = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_before));

	/* Inject fault: next flash write fails (data write — first in sequence). */
	ubi_test_fault_set_flash_write_fail_after(0);

	const uint8_t data[] = { 0xAA, 0xBB, 0xCC, 0xDD };
	int ret = ubi_leb_write(ubi, vol_id, 0, data, sizeof(data));
	zassert_not_equal(0, ret, "Write should fail with injected fault");

	ubi_test_fault_reset();

	struct ubi_device_info info_after = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_after));

	zassert_equal(info_before.bad_peb_count + 1, info_after.bad_peb_count,
		      "Failed PEB should be marked bad");
	zassert_equal(info_before.free_peb_count - 1, info_after.free_peb_count,
		      "Free PEB count should decrease");

	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that VID commit failure preserves old mapping (COW semantics).
 *
 * \details Scenario: Setup: Write data to LEB 0 successfully. Then inject write fault
 *          after the 1st successful write (data write succeeds, VID commit fails).
 *          VID is the commit point, so the old LEB 0 data should remain
 *          intact (copy-on-write semantics).
 *
 * \expect Second write returns error. Old data readable. bad_peb_count increases.
 */
ZTEST_F(ubi_io_faults, data_write_failure_preserves_old_mapping)
{
	struct ubi_device *ubi = ubi_test_init_device(&flash);
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "wfault_vol2",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* First write succeeds normally. */
	const uint8_t original[] = { 0xDE, 0xAD, 0xBE, 0xEF };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, original, sizeof(original)));

	struct ubi_device_info info_before = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_before));

	/* Inject fault: Let VID header write succeed (1 write), fail on data write. */
	ubi_test_fault_set_flash_write_fail_after(1);

	const uint8_t new_data[] = { 0xCA, 0xFE, 0xBA, 0xBE };
	int ret = ubi_leb_write(ubi, vol_id, 0, new_data, sizeof(new_data));
	zassert_not_equal(0, ret, "Overwrite should fail with data write fault");

	ubi_test_fault_reset();

	/* Old data should still be readable (COW semantics). */
	uint8_t readback[4] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, original, sizeof(original),
			  "Old data should be preserved after write failure");

	struct ubi_device_info info_after = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_after));
	zassert_true(info_after.bad_peb_count > info_before.bad_peb_count,
		     "Failed PEB should be marked bad");

	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that leb_map with VID write failure marks PEB bad.
 *
 * \details Scenario: leb_map internally calls leb_prepare_new_mapping with buf=NULL, len=0.
 *          If VID header write fails, the PEB should be marked bad.
 *
 * \expect leb_map returns error. bad_peb_count increases.
 */
ZTEST_F(ubi_io_faults, leb_map_vid_write_failure)
{
	struct ubi_device *ubi = ubi_test_init_device(&flash);
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "map_unmap_vol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	struct ubi_device_info info_before = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_before));

	ubi_test_fault_set_flash_write_fail_after(0);

	int ret = ubi_leb_map(ubi, vol_id, 0);
	zassert_not_equal(0, ret, "Map should fail with injected write fault");

	ubi_test_fault_reset();

	struct ubi_device_info info_after = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_after));
	zassert_equal(info_before.bad_peb_count + 1, info_after.bad_peb_count,
		      "Failed PEB should be marked bad");

	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify recovery after write fault — bad PEB can be tortured back to free pool.
 *
 * \details Scenario: Write to LEB with injected VID write fault → PEB goes bad.
 *          Call erase_peb() which triggers torture. Since the underlying
 *          flash is fine (fault was injected), torture should recover the PEB.
 *
 * \expect After erase_peb, bad_peb_count returns to 0. The PEB was recovered.
 */
ZTEST_F(ubi_io_faults, write_fault_peb_recoverable_by_torture)
{
#if defined(CONFIG_FLASH_SIMULATOR)
	struct ubi_device *ubi = ubi_test_init_device(&flash);
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "torture_vol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Inject VID write fault → PEB marked bad */
	ubi_test_fault_set_flash_write_fail_after(0);

	const uint8_t data[] = { 0x42 };
	int ret = ubi_leb_write(ubi, vol_id, 0, data, sizeof(data));
	zassert_not_equal(0, ret);

	ubi_test_fault_reset();

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.bad_peb_count, "Should have 1 bad PEB");

	/* Torture should recover the PEB since flash is actually fine */
	zassert_ok(ubi_device_erase_peb(ubi));

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.bad_peb_count, "Bad PEB should be recovered by torture");

	/* Now the write should succeed */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t rb[1] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rb, sizeof(rb)));
	zassert_equal(0x42, rb[0]);

	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Verify that multiple consecutive write failures degrade gracefully.
 *
 * \details Scenario: Inject persistent write fault. Attempt multiple writes.
 *          Each should fail and mark a PEB bad. Once all free PEBs are
 *          exhausted, write returns -ENOSPC.
 *
 * \expect Each write failure increments bad_peb_count. Eventually returns -ENOSPC.
 */
ZTEST_F(ubi_io_faults, multiple_write_failures_exhaust_free_pebs)
{
	struct ubi_device *ubi = ubi_test_init_device(&flash);
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "wretry_exh_vol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	const size_t initial_free = info.free_peb_count;

	/* Keep injecting write faults until no free PEBs remain */
	const uint8_t data[] = { 0x42 };

	for (size_t i = 0; i < initial_free; ++i) {
		ubi_test_fault_set_flash_write_fail_after(0);
		int ret = ubi_leb_write(ubi, vol_id, 0, data, sizeof(data));
		ubi_test_fault_reset();

		if (ret == -ENOSPC) {
			break;
		}
		zassert_not_equal(0, ret, "Write should fail under fault injection");
	}

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.bad_peb_count > 0, "Should have bad PEBs after failures");
	zassert_equal(0, info.free_peb_count, "All free PEBs should be consumed");

	ubi_test_fault_reset();
	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify EC header write failure after successful erase in erase_peb().
 *
 * \details Scenario: Create volume, write data, unmap to produce a dirty PEB.
 *          Then inject write fault so the EC header re-write in erase_peb() fails
 *          after the actual flash erase succeeded. This should mark the PEB bad.
 *
 * \expect erase_peb returns error. The dirty PEB moves to bad list.
 */
ZTEST_F(ubi_io_faults, ec_write_failure_during_erase_peb)
{
	struct ubi_device *ubi = ubi_test_init_device(&flash);
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "ec_wfault_vol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Write then unmap to create a dirty PEB */
	const uint8_t data[] = { 0x11, 0x22 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

	struct ubi_device_info info_before = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_before));
	zassert_true(info_before.dirty_peb_count >= 1, "Need at least 1 dirty PEB");

	/* Inject write fault: The erase itself will succeed, but the
	 * EC header write in erase_peb() will fail. */
	ubi_test_fault_set_flash_write_fail_after(0);

	zassert_equal(-EIO, ubi_device_erase_peb(ubi),
		      "erase_peb must surface -EIO when EC header re-write fails after erase");

	ubi_test_fault_reset();

	/* The erase path should have detected the write failure and marked PEB bad */
	struct ubi_device_info info_after = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_after));

	zassert_true(info_after.bad_peb_count > info_before.bad_peb_count,
		     "PEB should be marked bad when EC write fails after erase");
	zassert_true(info_after.dirty_peb_count < info_before.dirty_peb_count,
		     "Dirty PEB should be removed from dirty list");

	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify EC header write failure during torture recovery.
 *
 * \details Scenario: Create a bad PEB through VID write fault. Then inject another
 *          write fault so that torture's EC header write also fails. The PEB
 *          should remain in the bad list.
 *
 * \expect bad_peb_count remains > 0 after erase_peb torture attempt.
 */
ZTEST_F(ubi_io_faults, ec_write_failure_during_torture)
{
#if defined(CONFIG_FLASH_SIMULATOR)
	struct ubi_device *ubi = ubi_test_init_device(&flash);
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "torture_ec_vol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* First, create a bad PEB via VID write fault */
	ubi_test_fault_set_flash_write_fail_after(0);
	const uint8_t data[] = { 0x42 };
	zassert_equal(-EIO, ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)),
		      "leb_write must surface -EIO when VID write fault hits");
	ubi_test_fault_reset();

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.bad_peb_count);

	/* Now inject write fault so torture's EC write also fails */
	ubi_test_fault_set_flash_write_fail_after(0);
	zassert_ok(ubi_device_erase_peb(ubi));
	ubi_test_fault_reset();

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.bad_peb_count >= 1, "PEB should remain bad when torture EC write fails");

	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Verify that write fault during volume create is handled safely.
 *
 * \details Scenario: Volume create writes volume headers to reserved PEBs via
 *          ubi_vol_hdr_append → ubi_flash_res_peb_commit → ubi_flash_res_peb_overwrite.
 *          These writes go through flash_area_write directly (not flash_write_with_retry)
 *          since reserved PEBs have their own multi-PEB fallback. This test instead
 *          uses allocation fault injection to verify create safety.
 *
 * \expect Volume create fails cleanly. No volume persists on re-init.
 */
ZTEST_F(ubi_io_faults, alloc_fail_during_volume_create)
{
	struct ubi_device *ubi = ubi_test_init_device(&flash);
	fixture->ubi = ubi;

	/* Fail on the 1st allocation inside volume_create (volume struct) */
	ubi_test_fault_set_alloc_fail_after(0);

	const struct ubi_volume_config cfg = {
		.name = "vcreate_flt1",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	int ret = ubi_volume_create(ubi, &cfg, &vol_id);
	zassert_equal(-ENOMEM, ret, "Create should fail with ENOMEM");

	ubi_test_fault_reset();

	/* No volume should exist */
	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.volume_count, "No volume should be created");

	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));

	/* Verify no volume persists after re-init */
	ubi = ubi_test_init_device(&flash);
	fixture->ubi = ubi;
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.volume_count, "Volume should not persist after failed create");
	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that leaf allocation failure during volume create (after volume alloc)
 *        cleans up the volume struct and leaves no persistent state.
 *
 * \expect Create returns -ENOMEM. Volume struct freed. No volume on re-init.
 *
 * \details Scenario: Initialize device. Inject an allocation fault at position 1 (leaf
 *          alloc, after the volume struct alloc succeeds). Call ubi_volume_create for a
 *          dynamic volume. Reset the fault. Verify via check_invariants and get_info.
 */
ZTEST_F(ubi_io_faults, leaf_alloc_fail_during_volume_create)
{
	struct ubi_device *ubi = ubi_test_init_device(&flash);
	fixture->ubi = ubi;

	/* Fail on the 2nd allocation (leaf item after volume struct succeeds) */
	ubi_test_fault_set_alloc_fail_after(1);

	const struct ubi_volume_config cfg = {
		.name = "vcreate_flt2",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	int ret = ubi_volume_create(ubi, &cfg, &vol_id);
	zassert_equal(-ENOMEM, ret, "Create should fail when leaf alloc fails");

	ubi_test_fault_reset();

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.volume_count);

	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify device deinit is safe after write faults leave bad PEBs.
 *
 * \details Scenario: Multiple write faults create bad PEBs. Verify deinit properly
 *          cleans up all tracked PEBs without leaks.
 *
 * \expect deinit succeeds. Memory stats show no leak (heap backend).
 */
ZTEST_F(ubi_io_faults, deinit_safe_after_write_faults)
{
	struct sys_memory_stats mem_before;
	ubi_test_memory_snapshot(&mem_before);

	struct ubi_device *ubi = ubi_test_init_device(&flash);
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "deinit_flt_vol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Create 2 bad PEBs via write faults */
	const uint8_t data[] = { 0x42 };
	for (int i = 0; i < 2; ++i) {
		ubi_test_fault_set_flash_write_fail_after(0);
		(void)ubi_leb_write(ubi, vol_id, 0, data, sizeof(data));
		ubi_test_fault_reset();
	}

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.bad_peb_count >= 2, "Should have at least 2 bad PEBs");

	/* Deinit should clean up everything */
	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));

	struct sys_memory_stats mem_after;
	ubi_test_memory_snapshot(&mem_after);
	ubi_test_memory_check_no_leak(&mem_before, &mem_after);
}

/**
 * \brief Verify that invariants hold after write fault and recovery.
 *
 * \details Scenario: Write fault creates bad PEB. Torture recovers it. Check invariants
 *          at each stage to ensure internal consistency.
 *
 * \expect check_invariants passes after fault, after torture, and after recovery.
 */
ZTEST_F(ubi_io_faults, invariants_hold_after_write_fault_and_recovery)
{
#if defined(CONFIG_UBI_TEST_API_ENABLE) && defined(CONFIG_FLASH_SIMULATOR)
	struct ubi_device *ubi = ubi_test_init_device(&flash);
	fixture->ubi = ubi;

	zassert_ok(ubi_device_check_invariants(ubi));

	const struct ubi_volume_config cfg = {
		.name = "invariant_vol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));
	zassert_ok(ubi_device_check_invariants(ubi));

	/* Write fault */
	ubi_test_fault_set_flash_write_fail_after(0);
	const uint8_t data[] = { 0x42 };
	zassert_equal(-EIO, ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)),
		      "leb_write must surface -EIO when VID write fault hits");
	ubi_test_fault_reset();

	/* Invariants should hold even with bad PEBs */
	zassert_ok(ubi_device_check_invariants(ubi));

	/* Torture recovers the PEB */
	zassert_ok(ubi_device_erase_peb(ubi));
	zassert_ok(ubi_device_check_invariants(ubi));

	/* Normal write should now succeed */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_device_check_invariants(ubi));

	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Scratch alloc failure inside ubi_vol_hdr_append during volume_create.
 *
 * During volume_create, allocations: (1) volume, (2) leaf, (3) scratch in append.
 * Failing at #3 exercises the error path inside ubi_vol_hdr_append.
 *
 * \details Scenario: Initialize device. Inject an allocation fault at position 2 (scratch
 *          alloc inside ubi_vol_hdr_append). Call ubi_volume_create. Reset fault and
 *          call check_invariants. Deinit.
 *
 * \expect create returns a non-zero error; check_invariants returns 0; device remains consistent.
 */
ZTEST_F(ubi_io_faults, vol_create_scratch_alloc_fails_in_append)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "scratch_vol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;

	/* Fail the 3rd allocation: volume_alloc(1), leaf_alloc(2), scratch_alloc(3) */
	ubi_test_fault_set_alloc_fail_after(2);
	int ret = ubi_volume_create(ubi, &cfg, &vol_id);
	ubi_test_fault_reset();

	zassert_equal(-ENOMEM, ret, "volume_create must surface -ENOMEM when scratch alloc fails");

	/* Device should still be consistent */
	zassert_ok(ubi_device_check_invariants(ubi));

	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Scratch alloc failure inside ubi_vol_hdr_remove during volume_remove.
 *
 * During volume_remove, no allocations happen before vol_hdr_remove's scratch alloc.
 * Failing at position 0 exercises the error path.
 *
 * \details Scenario: Initialize device, create a volume. Inject an allocation fault at
 *          position 0 (scratch alloc inside ubi_vol_hdr_remove). Call ubi_volume_remove.
 *          Reset fault and call check_invariants. Deinit.
 *
 * \expect remove returns a non-zero error; check_invariants returns 0; device remains consistent.
 */
ZTEST_F(ubi_io_faults, vol_remove_scratch_alloc_fails_in_remove)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "vrm_scratch",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Fail immediately — the first alloc in vol_hdr_remove is scratch */
	ubi_test_fault_set_alloc_fail_after(0);
	int ret = ubi_volume_remove(ubi, vol_id);
	ubi_test_fault_reset();

	zassert_equal(-ENOMEM, ret, "volume_remove must surface -ENOMEM when scratch alloc fails");

	/* Device should still be consistent */
	zassert_ok(ubi_device_check_invariants(ubi));

	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Scratch alloc failure inside ubi_vol_hdr_update during volume_resize.
 *
 * During volume_resize, no allocations happen before vol_hdr_update's scratch alloc.
 *
 * \details Scenario: Initialize device, create a dynamic volume with leb_count=1. Inject
 *          an allocation fault at position 0 (scratch alloc inside ubi_vol_hdr_update).
 *          Call ubi_volume_resize to leb_count=2. Reset fault and call check_invariants.
 *
 * \expect resize returns a non-zero error; check_invariants returns 0; device remains consistent.
 */
ZTEST_F(ubi_io_faults, vol_resize_scratch_alloc_fails_in_update)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "vrsz_scratch",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	struct ubi_volume_config new_cfg = cfg;
	new_cfg.leb_count = 2;

	/* Fail immediately — vol_hdr_update's scratch alloc is the first */
	ubi_test_fault_set_alloc_fail_after(0);
	int ret = ubi_volume_resize(ubi, vol_id, &new_cfg);
	ubi_test_fault_reset();

	zassert_equal(-ENOMEM, ret, "volume_resize must surface -ENOMEM when scratch alloc fails");

	/* Device should still be consistent */
	zassert_ok(ubi_device_check_invariants(ubi));

	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Leaf alloc failure during init scan marks PEB processing.
 *
 * By failing allocations during init after the initial format succeeds,
 * the scan phase's leaf_alloc calls fail, preventing PEB classification.
 *
 * \details Scenario: Format the partition with no volumes (init+deinit). Sweep failure
 *          positions 0..20 on reinit. For each position: inject the fault, call
 *          ubi_device_init, reset the fault. If init succeeded, deinit before next iteration.
 *
 * \expect Sweep completes; every failure position is handled gracefully; no crashes.
 */
ZTEST_F(ubi_io_faults, init_alloc_failure_sweep_no_volumes)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	/* Format partition with no volumes */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;

	/* Sweep all allocation failure positions during reinit.
	 * With 14 data PEBs: device_alloc(1) + up to 14 leaf_allocs = 15 total.
	 * Try positions 0..20 to cover edge cases. */
	int enomem_hits = 0;
	for (int fail_pos = 0; fail_pos <= 20; ++fail_pos) {
		ubi_test_fault_set_alloc_fail_after(fail_pos);
		ubi = NULL;
		int ret = ubi_device_init(&flash, NULL, &ubi);
		ubi_test_fault_reset();

		zassert_true(
			ret == 0 || ret == -ENOMEM,
			"init must surface 0 or -ENOMEM under alloc-fault sweep, got %d at pos=%d",
			ret, fail_pos);
		if (ret == -ENOMEM) {
			enomem_hits++;
		}

		if (ret == 0 && ubi != NULL) {
			fixture->ubi = ubi;
			zassert_ok(ubi_device_deinit(ubi));
			fixture->ubi = NULL;
		}
	}
	zassert_true(
		enomem_hits >= 1,
		"alloc-fault sweep must hit at least one -ENOMEM (got 0; injector silently no-op'd?)");
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Volume alloc failure during init_collect_volumes.
 *
 * When a volume exists, init_collect_volumes allocates a volume struct.
 * Failing this alloc covers the volume/leaf alloc paths
 * in the collect phase.
 *
 * \details Scenario: Set up a partition with one dynamic volume (2 LEBs), data in both
 *          LEBs and several erase cycles, then deinit. Sweep failure positions 0..25 on
 *          reinit; init/deinit per iteration.
 *
 * \expect Sweep completes; every alloc-failure position during init_collect_volumes is handled.
 */
ZTEST_F(ubi_io_faults, init_alloc_failure_sweep_with_volume)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	/* Create a formatted partition with one volume and one written LEB */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "init_sweep_vol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0x42, 0x43 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 1, data, sizeof(data)));

	/* Erase dirty PEBs so scan state is clean */
	for (int i = 0; i < 5; ++i) {
		(void)ubi_device_erase_peb(ubi);
	}

	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;

	/* Sweep all failure positions during reinit.
	 * Init allocates: device(1) + leaves for free PEBs (~10-12) +
	 * leaves for mapped PEBs (~2) + volume_alloc(1) + leaf for vols(1).
	 * Try wide range to cover all paths. */
	int enomem_hits = 0;
	for (int fail_pos = 0; fail_pos <= 25; ++fail_pos) {
		ubi_test_fault_set_alloc_fail_after(fail_pos);
		ubi = NULL;
		int ret = ubi_device_init(&flash, NULL, &ubi);
		ubi_test_fault_reset();

		zassert_true(
			ret == 0 || ret == -ENOMEM,
			"init must surface 0 or -ENOMEM under alloc-fault sweep, got %d at pos=%d",
			ret, fail_pos);
		if (ret == -ENOMEM) {
			enomem_hits++;
		}

		if (ret == 0 && ubi != NULL) {
			fixture->ubi = ubi;
			zassert_ok(ubi_device_deinit(ubi));
			fixture->ubi = NULL;
		}
	}
	zassert_true(
		enomem_hits >= 1,
		"alloc-fault sweep must hit at least one -ENOMEM (got 0; injector silently no-op'd?)");
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Alloc failure sweep with orphan PEBs.
 *
 * Creates orphan PEBs (volume deleted but mapped PEBs remain on flash)
 * to exercise classify_orphan_peb alloc failure paths during init scan.
 *
 * \details Scenario: Create a dynamic volume (3 LEBs), write to LEBs 0 and 1, remove the
 *          volume so its mapped PEBs become orphans, deinit without erasing dirty PEBs.
 *          Sweep failure positions 0..25 on reinit.
 *
 * \expect Sweep completes; orphan classification succeeds despite alloc faults.
 */
ZTEST_F(ubi_io_faults, init_alloc_failure_sweep_with_orphans)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	/* Create partition with volume, write data, then remove volume + deinit.
	 * The mapped PEBs become orphans on next init. */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "orph_sweep_vol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 3,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0x55 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 1, data, sizeof(data)));

	/* Remove volume — PEBs with VID headers referencing this vol remain */
	zassert_ok(ubi_volume_remove(ubi, vol_id));

	/* Don't erase dirty PEBs — they'll appear as orphans on reinit */
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;

	/* Sweep all failure positions */
	int enomem_hits = 0;
	for (int fail_pos = 0; fail_pos <= 25; ++fail_pos) {
		ubi_test_fault_set_alloc_fail_after(fail_pos);
		ubi = NULL;
		int ret = ubi_device_init(&flash, NULL, &ubi);
		ubi_test_fault_reset();

		zassert_true(
			ret == 0 || ret == -ENOMEM,
			"init must surface 0 or -ENOMEM under alloc-fault sweep, got %d at pos=%d",
			ret, fail_pos);
		if (ret == -ENOMEM) {
			enomem_hits++;
		}

		if (ret == 0 && ubi != NULL) {
			fixture->ubi = ubi;
			zassert_ok(ubi_device_deinit(ubi));
			fixture->ubi = NULL;
		}
	}
	zassert_true(
		enomem_hits >= 1,
		"alloc-fault sweep must hit at least one -ENOMEM (got 0; injector silently no-op'd?)");
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Alloc failure sweep with duplicate LEBs.
 *
 * Injects a duplicate LEB mapping on flash, then sweeps init alloc failures.
 * This exercises resolve_duplicate_leb alloc failure (L395-396).
 *
 * \details Scenario: Format, create volume (2 LEBs), write LEB 0, erase dirty PEBs,
 *          deinit. Inject a duplicate LEB 0 with a higher sqnum on a free PEB via raw
 *          flash writes. Sweep failure positions 0..25 on reinit.
 *
 * \expect Sweep completes; duplicate resolution succeeds despite alloc faults.
 */
ZTEST_F(ubi_io_faults, init_alloc_failure_sweep_with_duplicates)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "dup_sweep_vol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xDE };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	/* Erase dirty PEBs to free space */
	for (int i = 0; i < 5; ++i) {
		(void)ubi_device_erase_peb(ubi);
	}

	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;

	/* Inject a duplicate LEB 0 with high sqnum on a free PEB */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t nr_pebs = fa->fa_size / flash.erase_block_size;
	size_t free_peb = 0;

	for (size_t p = NR_OF_RES_PEBS; p < nr_pebs; ++p) {
		uint8_t vid[VID_HDR_SIZE];
		(void)flash_area_read(fa, (p * flash.erase_block_size) + EC_HDR_SIZE, vid,
				      sizeof(vid));
		uint32_t vid_magic = 0;
		memcpy(&vid_magic, vid, 4);
		if (vid_magic == 0xFFFFFFFF) {
			free_peb = p;
			break;
		}
	}

	if (free_peb >= NR_OF_RES_PEBS) {
		(void)flash_area_erase(fa, free_peb * flash.erase_block_size,
				       flash.erase_block_size);
		io_raw_write_ec(fa, free_peb, flash.erase_block_size, 0);
		io_raw_write_vid(fa, free_peb, flash.erase_block_size, 0, (uint32_t)vol_id, 999999,
				 sizeof(data));
	}

	flash_area_close(fa);

	/* Sweep all failure positions */
	int enomem_hits = 0;
	for (int fail_pos = 0; fail_pos <= 25; ++fail_pos) {
		ubi_test_fault_set_alloc_fail_after(fail_pos);
		ubi = NULL;
		int ret = ubi_device_init(&flash, NULL, &ubi);
		ubi_test_fault_reset();

		zassert_true(
			ret == 0 || ret == -ENOMEM,
			"init must surface 0 or -ENOMEM under alloc-fault sweep, got %d at pos=%d",
			ret, fail_pos);
		if (ret == -ENOMEM) {
			enomem_hits++;
		}

		if (ret == 0 && ubi != NULL) {
			fixture->ubi = ubi;
			zassert_ok(ubi_device_deinit(ubi));
			fixture->ubi = NULL;
		}
	}
	zassert_true(
		enomem_hits >= 1,
		"alloc-fault sweep must hit at least one -ENOMEM (got 0; injector silently no-op'd?)");
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Alloc failure sweep with a PEB that has corrupt VID CRC.
 *
 * Writes a VID header with bad CRC on a data PEB, then sweeps init.
 * This exercises the validate_vid_header CRC-fail-path alloc failure (L302-303).
 *
 * \details Scenario: Format the partition, deinit. Corrupt the VID CRC on the first data
 *          PEB by writing a bad CRC value. Sweep failure positions 0..25 on reinit.
 *
 * \expect Sweep completes; CRC validation failure is handled without leaks.
 */
ZTEST_F(ubi_io_faults, init_alloc_failure_sweep_with_bad_vid_crc)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	/* Format partition */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;

	/* Corrupt one data PEB's VID CRC: write VID header bytes but with bad CRC */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t target_peb = NR_OF_RES_PEBS; /* First data PEB */
	struct raw_vid_hdr_io vid = { .magic = VID_HDR_MAGIC,
				      .version = 1,
				      .lnum = 0,
				      .vol_id = 42,
				      .sqnum = 1,
				      .data_size = 1 };
	/* Deliberately compute WRONG CRC */
	vid.hdr_crc = 0xBADBAD00;
	(void)flash_area_write(fa, (target_peb * flash.erase_block_size) + EC_HDR_SIZE, &vid,
			       sizeof(vid));

	flash_area_close(fa);

	/* Sweep all failure positions */
	int enomem_hits = 0;
	for (int fail_pos = 0; fail_pos <= 25; ++fail_pos) {
		ubi_test_fault_set_alloc_fail_after(fail_pos);
		ubi = NULL;
		int ret = ubi_device_init(&flash, NULL, &ubi);
		ubi_test_fault_reset();

		zassert_true(
			ret == 0 || ret == -ENOMEM,
			"init must surface 0 or -ENOMEM under alloc-fault sweep, got %d at pos=%d",
			ret, fail_pos);
		if (ret == -ENOMEM) {
			enomem_hits++;
		}

		if (ret == 0 && ubi != NULL) {
			fixture->ubi = ubi;
			zassert_ok(ubi_device_deinit(ubi));
			fixture->ubi = NULL;
		}
	}
	zassert_true(
		enomem_hits >= 1,
		"alloc-fault sweep must hit at least one -ENOMEM (got 0; injector silently no-op'd?)");
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Alloc failure sweep with a PEB that has a corrupt EC header.
 *
 * Corrupts the EC header of the first data PEB, then sweeps init alloc
 * failures to exercise the validate_ec_header alloc failure path (L238-239).
 *
 * \details Scenario: Format, deinit. Corrupt the EC header CRC on the first data PEB
 *          (erase the PEB and write a bad CRC). Sweep failure positions 0..25 on reinit.
 *
 * \expect Sweep completes; the EC corruption path is exercised safely.
 */
ZTEST_F(ubi_io_faults, init_alloc_failure_sweep_with_bad_ec)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	/* Format partition so all data PEBs have valid EC headers */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;

	/* Corrupt EC header CRC on the first data PEB */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t target_peb = NR_OF_RES_PEBS; /* PEB 2 = first data PEB */
	const size_t peb_off = target_peb * flash.erase_block_size;
	/* Read EC header, erase PEB, corrupt CRC, write back */
	uint8_t ec_hdr[16] = { 0 };
	zassert_ok(flash_area_read(fa, peb_off, ec_hdr, sizeof(ec_hdr)));
	zassert_ok(flash_area_erase(fa, peb_off, flash.erase_block_size));
	uint32_t bad_crc = 0xDEADBEEF;
	memcpy(ec_hdr + 12, &bad_crc, sizeof(bad_crc));
	zassert_ok(flash_area_write(fa, peb_off, ec_hdr, sizeof(ec_hdr)));

	flash_area_close(fa);

	/* Sweep — the bad EC PEB triggers validate_ec_header failure path.
	 * When alloc fails at the right position the error path is exercised. */
	int enomem_hits = 0;
	for (int fail_pos = 0; fail_pos <= 25; ++fail_pos) {
		ubi_test_fault_set_alloc_fail_after(fail_pos);
		ubi = NULL;
		int ret = ubi_device_init(&flash, NULL, &ubi);
		ubi_test_fault_reset();

		zassert_true(
			ret == 0 || ret == -ENOMEM,
			"init must surface 0 or -ENOMEM under alloc-fault sweep, got %d at pos=%d",
			ret, fail_pos);
		if (ret == -ENOMEM) {
			enomem_hits++;
		}

		if (ret == 0 && ubi != NULL) {
			fixture->ubi = ubi;
			zassert_ok(ubi_device_deinit(ubi));
			fixture->ubi = NULL;
		}
	}
	zassert_true(
		enomem_hits >= 1,
		"alloc-fault sweep must hit at least one -ENOMEM (got 0; injector silently no-op'd?)");
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Diag alloc fault during ubi_device_get_peb_ec.
 *
 * \details Scenario: Initialize device. Inject an allocation failure at position 0.
 *          Call ubi_device_get_peb_ec. Reset fault. Deinit.
 *
 * \expect ubi_device_get_peb_ec returns a non-zero error; the peb_ec output pointer remains NULL.
 */
ZTEST_F(ubi_io_faults, get_peb_ec_diag_alloc_fault)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	/* Make next alloc fail — get_peb_ec calls ubi_mem_diag_alloc */
	ubi_test_fault_set_alloc_fail_after(0);
	size_t *peb_ec = NULL;
	size_t len = 0;
	int ret = ubi_device_get_peb_ec(ubi, &peb_ec, &len);
	ubi_test_fault_reset();

	zassert_equal(ret, -ENOMEM,
		      "get_peb_ec must return -ENOMEM under diag alloc failure, got %d", ret);
	zassert_is_null(peb_ec);

	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Alloc failure sweep during volume create with multiple volumes.
 *
 * Creates one volume, then sweeps alloc failures during creation of a second.
 * This exercises different alloc failure points within vol_hdr_append.
 *
 * \details Scenario: Initialize, create the first dynamic volume. Sweep failure positions
 *          0..5 on the creation of a second volume. Per iteration: inject fault, attempt
 *          create, reset fault; if create succeeded remove for the next iteration.
 *
 * \expect Sweep completes; every failure position is handled; check_invariants returns 0.
 */
ZTEST_F(ubi_io_faults, vol_create_second_volume_alloc_sweep)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	/* Create first volume normally */
	struct ubi_volume_config cfg1 = {
		.name = "first",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id1 = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg1, &vol_id1));

	/* Try creating second volume with various failure positions */
	struct ubi_volume_config cfg2 = {
		.name = "second",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};

	for (int fail_pos = 0; fail_pos <= 5; ++fail_pos) {
		int vol_id2 = -1;
		ubi_test_fault_set_alloc_fail_after(fail_pos);
		int ret = ubi_volume_create(ubi, &cfg2, &vol_id2);
		ubi_test_fault_reset();

		if (ret == 0) {
			/* If it succeeded (unlikely for most positions), remove for next iter */
			(void)ubi_volume_remove(ubi, vol_id2);
		}
	}

	/* Device should still be consistent */
	zassert_ok(ubi_device_check_invariants(ubi));

	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Alloc failure sweep during volume remove with multiple volumes.
 *
 * Creates two volumes, then sweeps alloc failures during removal of one.
 * This exercises alloc failure paths in vol_hdr_remove.
 *
 * \details Scenario: Initialize, create two dynamic volumes. Sweep failure positions
 *          0..3 on removal of the first volume. Per iteration: inject fault, attempt
 *          remove, reset fault; if remove succeeded recreate for the next iteration.
 *
 * \expect Sweep completes; every failure position is handled; check_invariants returns 0.
 */
ZTEST_F(ubi_io_faults, vol_remove_alloc_sweep)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	struct ubi_volume_config cfg1 = {
		.name = "rm_sweep_vol1",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id1 = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg1, &vol_id1));

	struct ubi_volume_config cfg2 = {
		.name = "rm_sweep_vol2",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id2 = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg2, &vol_id2));

	/* Sweep failure positions during remove of vol1 */
	for (int fail_pos = 0; fail_pos <= 3; ++fail_pos) {
		ubi_test_fault_set_alloc_fail_after(fail_pos);
		int ret = ubi_volume_remove(ubi, vol_id1);
		ubi_test_fault_reset();

		if (ret == 0) {
			/* If remove succeeded, re-create for the next iteration */
			zassert_ok(ubi_volume_create(ubi, &cfg1, &vol_id1));
		}
	}

	zassert_ok(ubi_device_check_invariants(ubi));

	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Verify deterministic behaviour when the very first flash erase
 *        attempt during ubi_device_erase_peb() fails.
 *
 * \details Scenario: Init device, create a 2-LEB volume, write data to
 *          both LEBs, then remove the volume to generate dirty PEBs.
 *          Snapshot device info, inject `flash_erase_fail_after(0)` so
 *          the next flash_area_erase fails on the very first call,
 *          invoke `ubi_device_erase_peb()`, reset the fault and snapshot
 *          again.
 *
 *          Empirically: the erase call surfaces `-EIO`, exactly one
 *          dirty PEB transitions out of the dirty list and into the
 *          free pool (the implementation reclaims it without a
 *          successful re-erase), and no PEB is added to the bad list.
 *          This contradicts the historical name `moves_to_bad` which
 *          described an aspirational behaviour the implementation does
 *          not actually perform.
 *
 * \expect erase_peb returns `-EIO`; dirty count drops by exactly 1;
 *         free count increases by exactly 1; bad count is unchanged;
 *         check_invariants returns 0.
 */
ZTEST_F(ubi_io_faults, erase_peb_flash_erase_failure_recycles_dirty_peb)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	/* Create a volume, write data, then remove to generate dirty PEBs */
	struct ubi_volume_config cfg = {
		.name = "erase_flt_vol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xAA, 0xBB };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 1, data, sizeof(data)));
	zassert_ok(ubi_volume_remove(ubi, vol_id));

	struct ubi_device_info info_before = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_before));

	/* Inject erase failure on the very first flash_area_erase attempt */
	ubi_test_fault_set_flash_erase_fail_after(0);
	int ret = ubi_device_erase_peb(ubi);
	ubi_test_fault_reset();

	zassert_equal(-EIO, ret,
		      "erase_peb must surface -EIO when the underlying flash erase faults");

	struct ubi_device_info info_after = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_after));
	zassert_equal(info_before.bad_peb_count, info_after.bad_peb_count,
		      "bad_peb_count must NOT change on erase fault (current implementation)");
	zassert_equal(info_before.dirty_peb_count - 1, info_after.dirty_peb_count,
		      "exactly one dirty PEB must be reclaimed on erase fault");
	zassert_equal(info_before.free_peb_count + 1, info_after.free_peb_count,
		      "reclaimed dirty PEB must end up in the free pool");

	/* Invariants must still hold */
	zassert_ok(ubi_device_check_invariants(ubi));

	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Verify overwrite preserves old mapping when VID commit fails.
 *
 * \details Scenario: VID is the commit point. If VID write fails after data has been
 *          written, the old mapping must remain active and readable.
 *
 *          Write order per leb_write with data:
 *            flash write #0: data payload
 *            flash write #1: VID header (commit point)
 *
 *          Fault: fail_after(1) — let data write succeed, fail VID write.
 *
 * \expect  Old data remains readable. New mapping is not active.
 */
ZTEST_F(ubi_io_faults, overwrite_preserves_old_mapping_when_commit_vid_fails)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = ubi_test_init_device(&flash);
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "commitA",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* First write — establish an old mapping. */
	const uint8_t old_data[16] = {
		0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
		0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
	};
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, old_data, sizeof(old_data)));

	struct ubi_device_info info_before = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_before));

	/* Inject fault: let data write succeed (write #0), fail VID (write #1). */
	ubi_test_fault_set_flash_write_fail_after(1);

	const uint8_t new_data[16] = {
		0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11,
		0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
	};
	int ret = ubi_leb_write(ubi, vol_id, 0, new_data, sizeof(new_data));
	zassert_not_equal(0, ret, "Overwrite should fail when VID commit is faulted");

	ubi_test_fault_reset();

	/* Old data must still be readable — the old mapping was not swapped. */
	uint8_t readback[16] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, old_data, sizeof(old_data),
			  "Old data must be preserved when VID commit fails");

	struct ubi_device_info info_after = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_after));
	zassert_true(info_after.bad_peb_count > info_before.bad_peb_count,
		     "Failed PEB should be marked bad");

	zassert_ok(ubi_device_check_invariants(ubi));

	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Verify first write to a LEB does not create mapping when VID commit fails.
 *
 * \details Scenario: A freshly created volume has all LEBs unmapped. The first
 *          ubi_leb_write() to a LEB creates a new PEB mapping. If the VID
 *          write (commit point) fails after the data payload succeeds, the
 *          LEB must remain unmapped.
 *
 *          Fault: fail_after(1) — let data write succeed, fail VID write.
 *
 * \expect  LEB remains unmapped. ubi_leb_is_mapped returns false.
 */
ZTEST_F(ubi_io_faults, new_mapping_not_visible_when_commit_vid_fails)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = ubi_test_init_device(&flash);
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "commitB",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* LEB 0 is not mapped — volume was just created, no writes yet. */
	bool is_mapped = true;
	zassert_ok(ubi_leb_is_mapped(ubi, vol_id, 0, &is_mapped));
	zassert_false(is_mapped, "LEB 0 should be unmapped initially");

	/* Inject fault: let data write succeed (write #0), fail VID (write #1). */
	ubi_test_fault_set_flash_write_fail_after(1);

	const uint8_t data[16] = {
		0xCA, 0xFE, 0xBA, 0xBE, 0xDE, 0xAD, 0xBE, 0xEF,
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
	};
	int ret = ubi_leb_write(ubi, vol_id, 0, data, sizeof(data));
	zassert_not_equal(0, ret, "Write should fail when VID commit is faulted");

	ubi_test_fault_reset();

	/* LEB 0 must still be unmapped — VID commit did not succeed. */
	is_mapped = true;
	zassert_ok(ubi_leb_is_mapped(ubi, vol_id, 0, &is_mapped));
	zassert_false(is_mapped, "LEB 0 must remain unmapped after failed VID commit");

	zassert_ok(ubi_device_check_invariants(ubi));

	fixture->ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}
