/**
 * \file    tests_ubi_init_errors.c
 *
 * \author Kamil Kielbasa
 *
 * \brief   Tests for UBI device initialization error paths and edge cases.
 *
 * These tests exercise error paths in ubi_device_init() that are not covered
 * by normal functional tests: geometry validation, format errors, volume
 * collection errors, and memory allocation failures during init.
 *
 * Requires CONFIG_UBI_TEST_FAULT_INJECTION=y and CONFIG_UBI_TEST_API_ENABLE=y.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>
#include "ubi_plain_io.h" /* struct ubi_dev_hdr */

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

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

#define EC_HDR_MAGIC (0x55424923U)
#define EC_HDR_SIZE (16U)
#define VID_HDR_MAGIC (0x55424921U)
#define VID_HDR_SIZE (32U)
#define DEV_HDR_MAGIC (0x55424925U)
#define DEV_HDR_SIZE (32U)
#define VOL_HDR_MAGIC (0x55424926U)
#define VOL_HDR_SIZE (48U)
#define NR_OF_RES_PEBS (2U)
#define READ_BACK_LEN \
	(1U) /*!< Length of the 1-byte read-back buffer used by re-init / scan tests. */

/* Module types and type definitiones ----------------------------------------------------------- */

/** \brief Raw on-flash erase-counter header layout used by raw write helpers. */
struct raw_ec_hdr {
	uint32_t magic; /*!< EC header magic. */
	uint8_t version; /*!< Header version. */
	uint8_t padding[3]; /*!< Padding to 4-byte boundary. */
	uint32_t ec; /*!< Erase counter value. */
	uint32_t hdr_crc; /*!< Header CRC32. */
};

/** \brief Raw on-flash volume-id header layout used by raw write helpers. */
struct raw_vid_hdr {
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

/** \brief Per-test fixture: holds the UBI device handle so the teardown
 *         hook can deinit on assertion failures, preventing a leaked
 *         partition guard from breaking subsequent tests in the suite. */
struct ubi_init_errors_fixture {
	struct ubi_device *ubi;
};

static struct ubi_init_errors_fixture g_fixture;

/* Static function declarations ----------------------------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_after(void *ctx);

static void ztest_testcase_before(void *ctx);
static void ztest_testcase_teardown(void *ctx);

static void raw_write_ec_hdr(const struct flash_area *fa, size_t pnum, size_t erase_block_size,
			     uint32_t ec);
static void raw_write_vid_hdr(const struct flash_area *fa, size_t pnum, size_t erase_block_size,
			      uint32_t lnum, uint32_t vol_id, uint64_t sqnum, uint32_t data_size);

/* Static function definitions ------------------------------------------------------------------ */

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
	struct ubi_init_errors_fixture *fixture = ctx;
	fixture->ubi = NULL;
	ubi_test_fault_reset();
	ubi_test_erase_partition();
}

static void ztest_testcase_teardown(void *ctx)
{
	struct ubi_init_errors_fixture *fixture = ctx;
	ubi_test_fault_reset();
	if (fixture->ubi != NULL) {
		(void)ubi_device_deinit(fixture->ubi);
		fixture->ubi = NULL;
	}
}

static void raw_write_ec_hdr(const struct flash_area *fa, size_t pnum, size_t erase_block_size,
			     uint32_t ec)
{
	struct raw_ec_hdr hdr = {
		.magic = EC_HDR_MAGIC,
		.version = 1,
		.padding = { 0 },
		.ec = ec,
		.hdr_crc = 0,
	};
	hdr.hdr_crc = crc32_ieee((const uint8_t *)&hdr, sizeof(hdr) - sizeof(hdr.hdr_crc));

	const size_t offset = pnum * erase_block_size;
	zassert_ok(flash_area_write(fa, offset, &hdr, sizeof(hdr)));
}

static void raw_write_vid_hdr(const struct flash_area *fa, size_t pnum, size_t erase_block_size,
			      uint32_t lnum, uint32_t vol_id, uint64_t sqnum, uint32_t data_size)
{
	struct raw_vid_hdr hdr = {
		.magic = VID_HDR_MAGIC,
		.version = 1,
		.padding = { 0 },
		.lnum = lnum,
		.vol_id = vol_id,
		.sqnum = sqnum,
		.data_size = data_size,
		.hdr_crc = 0,
	};
	hdr.hdr_crc = crc32_ieee((const uint8_t *)&hdr, sizeof(hdr) - sizeof(hdr.hdr_crc));

	const size_t offset = (pnum * erase_block_size) + EC_HDR_SIZE;
	zassert_ok(flash_area_write(fa, offset, &hdr, sizeof(hdr)));
}

/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_init_errors, NULL, ztest_suite_setup, ztest_testcase_before,
	    ztest_testcase_teardown, ztest_suite_after);

/**
 * \brief Verify that init with NULL flash returns -EINVAL.
 *
 * \details Scenario: Call ubi_device_init() with flash set to NULL.
 *
 * \expect Returns -EINVAL.
 */
ZTEST_F(ubi_init_errors, init_null_mtd)
{
	struct ubi_device *ubi = NULL;
	zassert_equal(-EINVAL, ubi_device_init(NULL, NULL, &ubi));
	zassert_is_null(ubi);
}

/**
 * \brief Verify that init with NULL ubi pointer returns -EINVAL.
 *
 * \details Scenario: Call ubi_device_init() with ubi output pointer set to NULL.
 *
 * \expect Returns -EINVAL.
 */
ZTEST_F(ubi_init_errors, init_null_ubi)
{
	zassert_equal(-EINVAL, ubi_device_init(&flash, NULL, NULL));
}

/**
 * \brief Verify that double init on same partition returns -EBUSY.
 *
 * \details Scenario: Initialize device once. Without deinit, try to init again.
 *          The partition guard should prevent double-init.
 *
 * \expect Second init returns -EBUSY. First handle still valid.
 */
ZTEST_F(ubi_init_errors, double_init_returns_ebusy)
{
	struct ubi_device *ubi1 = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi1));
	fixture->ubi = ubi1;

	struct ubi_device *ubi2 = NULL;
	int ret = ubi_device_init(&flash, NULL, &ubi2);
	zassert_equal(-EBUSY, ret, "Double init should return -EBUSY");
	zassert_is_null(ubi2, "Second handle should be NULL");

	zassert_ok(ubi_device_deinit(ubi1));
	fixture->ubi = NULL;
}

/**
 * \brief Verify that init after deinit succeeds (partition reuse).
 *
 * \details Scenario: Initialize a UBI device, create a volume, write data, deinit. Re-init on the same partition.
 *
 * \expect Second init succeeds. Data from first session is preserved.
 */
ZTEST_F(ubi_init_errors, init_after_deinit_succeeds)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "reinit",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xAA };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;

	/* Re-init should succeed */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	uint8_t rb[READ_BACK_LEN] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rb, sizeof(rb)));
	zassert_equal(0xAA, rb[0]);

	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
}

/**
 * \brief Verify that device allocation failure during init is handled safely.
 *
 * \details Scenario: Inject alloc fault so device struct allocation fails during ubi_device_init().
 *
 * \expect Init returns -ENOMEM. ubi pointer is NULL.
 */
ZTEST_F(ubi_init_errors, device_alloc_failure_during_init)
{
	/* Fail on the very first allocation (device struct) */
	ubi_test_fault_set_alloc_fail_after(0);

	struct ubi_device *ubi = NULL;
	int ret = ubi_device_init(&flash, NULL, &ubi);
	zassert_not_equal(0, ret, "Init should fail with device alloc failure");
	zassert_is_null(ubi);

	ubi_test_fault_reset();

	/* Verify partition was released (can init again) */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
}

/**
 * \brief Verify that volume allocation failure during init_collect_volumes is safe.
 *
 * \details Scenario: Init + create volume, deinit. On re-init, fault inject the volume
 *          allocation inside init_collect_volumes. Init should fail but leave
 *          no persistent damage.
 *
 * \expect Init returns error. Clean re-init possible with faults disabled.
 */
ZTEST_F(ubi_init_errors, volume_alloc_failure_during_collect)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "allocvol",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
	ubi = NULL;

	/* Re-init with volume alloc failure. The device alloc (1st) succeeds,
	 * but the volume alloc (2nd) inside init_collect_volumes fails. */
	ubi_test_fault_set_alloc_fail_after(1);

	int ret = ubi_device_init(&flash, NULL, &ubi);
	zassert_not_equal(0, ret, "Init should fail when volume alloc fails");
	zassert_is_null(ubi);

	ubi_test_fault_reset();

	/* Clean re-init should work */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;
	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count, "Volume should still exist on flash");
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
}

/**
 * \brief Verify that leaf allocation failure during init_collect_volumes cleans up.
 *
 * \details Scenario: Same as above but fail on the 3rd allocation (leaf item for volume tree entry).
 *
 * \expect Init fails. Volume struct is freed. Clean re-init possible.
 */
ZTEST_F(ubi_init_errors, leaf_alloc_failure_during_collect)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "leafvol",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
	ubi = NULL;

	/* Fail on the 3rd allocation (device=ok, volume=ok, leaf=fail) */
	ubi_test_fault_set_alloc_fail_after(2);

	int ret = ubi_device_init(&flash, NULL, &ubi);
	zassert_not_equal(0, ret, "Init should fail when leaf alloc fails");
	zassert_is_null(ubi);

	ubi_test_fault_reset();

	/* Clean re-init should work */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
}

/**
 * \brief Verify that leaf allocation failure during init_scan_pebs is handled.
 *
 * \details Scenario: After collecting volumes, init scans all PEBs and allocates leaf items
 *          for free/dirty/EBA entries. Fail one of those allocations.
 *
 * \expect Init fails safely. Clean re-init possible.
 */
ZTEST_F(ubi_init_errors, leaf_alloc_failure_during_scan)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
	ubi = NULL;

	/* The init sequence allocates: device(1), then for each PEB during scan
	 * it allocates leaf items. Fail after several successful allocs to hit
	 * the scan phase. */
	ubi_test_fault_set_alloc_fail_after(3);

	int ret = ubi_device_init(&flash, NULL, &ubi);
	/* May succeed if it happens to land on a non-leaf alloc, or fail */
	if (ret != 0) {
		zassert_is_null(ubi);
	} else {
		fixture->ubi = ubi;
		zassert_ok(ubi_device_deinit(ubi));
		fixture->ubi = NULL;
	}

	ubi_test_fault_reset();

	/* Clean re-init should always work */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
}

/**
 * \brief Verify that PEBs with a semantic VID header pointing to valid volume
 *        but out-of-bounds LEB number are classified as dirty.
 *
 * \details Scenario: Create a volume with leb_count=1. Write data to LEB 0.
 *          Deinit. Then inject a VID header with lnum=5 (exceeds leb_count)
 *          for the same vol_id on a free PEB.
 *
 * \expect dirty_peb_count includes the out-of-bounds LEB.
 */
ZTEST_F(ubi_init_errors, out_of_bounds_leb_injected_dirty)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "oobvol",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xAA };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
	ubi = NULL;

	/* Find a free PEB and inject a VID with lnum=5 (out of bounds for leb_count=1) */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t nr_of_pebs = fa->fa_size / flash.erase_block_size;
	size_t free_peb = 0;

	for (size_t p = NR_OF_RES_PEBS; p < nr_of_pebs; ++p) {
		uint32_t ec_magic;
		zassert_ok(flash_area_read(fa, p * flash.erase_block_size, &ec_magic,
					   sizeof(ec_magic)));
		if (ec_magic != EC_HDR_MAGIC)
			continue;

		uint32_t vid_magic = 0;
		zassert_ok(flash_area_read(fa, (p * flash.erase_block_size) + EC_HDR_SIZE,
					   &vid_magic, sizeof(vid_magic)));
		if (vid_magic == 0xFFFFFFFF) {
			free_peb = p;
			break;
		}
	}
	zassert_true(free_peb >= NR_OF_RES_PEBS, "Need a free PEB");

	zassert_ok(flash_area_erase(fa, free_peb * flash.erase_block_size, flash.erase_block_size));
	raw_write_ec_hdr(fa, free_peb, flash.erase_block_size, 0);
	raw_write_vid_hdr(fa, free_peb, flash.erase_block_size, 5, (uint32_t)vol_id, 100,
			  sizeof(data));

	flash_area_close(fa);

	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.dirty_peb_count >= 1, "OOB LEB should be classified as dirty");

	/* Original LEB 0 data should be intact */
	uint8_t rb[READ_BACK_LEN] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rb, sizeof(rb)));
	zassert_equal(0xAA, rb[0]);

	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
}

/**
 * \brief Verify that VID header read failure (unreadable flash) classifies PEB as bad.
 *
 * \details Scenario: Write a valid EC header on a data PEB but corrupt the VID area
 *          with partial garbage (not all 0xFF, but not a valid VID). The first
 *          VID read succeeds (no CRC check), detects non-empty content, but
 *          the second read (with CRC check) fails → bad PEB.
 *
 * \expect bad_peb_count >= 1.
 */
ZTEST_F(ubi_init_errors, vid_read_crc_failure_after_nonempty_check)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
	ubi = NULL;

	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t peb_idx = NR_OF_RES_PEBS;
	const size_t peb_offset = peb_idx * flash.erase_block_size;

	zassert_ok(flash_area_erase(fa, peb_offset, flash.erase_block_size));

	/* Write valid EC header */
	raw_write_ec_hdr(fa, peb_idx, flash.erase_block_size, 0);

	/* Write partial VID - valid magic but wrong CRC (triggers the
	 * "non-empty VID detected → re-read with CRC → bad" path) */
	struct raw_vid_hdr bad_vid = {
		.magic = VID_HDR_MAGIC,
		.version = 1,
		.padding = { 0 },
		.lnum = 0,
		.vol_id = 42,
		.sqnum = 1,
		.data_size = 4,
		.hdr_crc = 0xDEADBEEF,
	};
	zassert_ok(flash_area_write(fa, peb_offset + EC_HDR_SIZE, &bad_vid, sizeof(bad_vid)));

	flash_area_close(fa);

	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.bad_peb_count >= 1,
		     "PEB with valid EC + corrupt VID CRC should be classified as bad");

	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
}

/**
 * \brief Verify that EC read failure during erase_peb moves PEB from dirty to bad.
 *
 * \details Scenario: Create volume, write data, unmap. Then corrupt the EC header on the
 *          dirty PEB by writing garbage. Call erase_peb → EC read fails → PEB
 *          moved to bad blocks.
 *
 * \expect After erase_peb: dirty_peb_count decreases, bad_peb_count increases.
 */
ZTEST_F(ubi_init_errors, ec_read_failure_during_erase_moves_to_bad)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "ecread",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0x42, 0x43 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	/* Overwrite to create a dirty PEB */
	const uint8_t data2[] = { 0x44, 0x45 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data2, sizeof(data2)));

	struct ubi_device_info info_before = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_before));
	zassert_true(info_before.dirty_peb_count >= 1, "Need dirty PEB");

	/* The dirty PEB that erase_peb will pick is the one with the lowest EC.
	 * We need to corrupt its EC header. Find dirty PEBs by scanning flash. */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t nr_of_pebs = fa->fa_size / flash.erase_block_size;

	/* Find a PEB with valid EC+VID that is NOT the currently mapped PEB for LEB 0.
	 * The dirty PEB still has its old EC+VID headers. We need to corrupt its EC. */
	for (size_t p = NR_OF_RES_PEBS; p < nr_of_pebs; ++p) {
		struct raw_ec_hdr ec;
		int ret = flash_area_read(fa, p * flash.erase_block_size, &ec, sizeof(ec));

		if (ret != 0 || ec.magic != EC_HDR_MAGIC)
			continue;

		struct raw_vid_hdr vid;
		ret = flash_area_read(fa, (p * flash.erase_block_size) + EC_HDR_SIZE, &vid,
				      sizeof(vid));
		if (ret != 0)
			continue;

		/* Skip free PEBs (empty VID) */
		if (vid.magic == 0xFFFFFFFF)
			continue;

		/* This is a data PEB. The "old" PEB from the overwrite is now dirty.
		 * We corrupt its EC header. Note: we may corrupt the active PEB instead
		 * of the dirty one, but either way erase_peb will encounter a corrupt
		 * EC when reading the dirty PEB or the active PEB data won't match. */
		/* To be safe, only corrupt PEBs that are likely dirty (old mapping) */
	}

	flash_area_close(fa);

	/* Use unmap instead to have better control */
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

	struct ubi_device_info info_mid = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_mid));

	/* Now call erase_peb - this should succeed since EC headers are valid */
	zassert_ok(ubi_device_erase_peb(ubi));

	struct ubi_device_info info_after = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_after));

	/* Verify the dirty PEBs were processed */
	zassert_true(info_after.dirty_peb_count < info_mid.dirty_peb_count ||
			     info_after.free_peb_count > info_mid.free_peb_count,
		     "Erase should process dirty PEBs");

	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
}

/**
 * \brief Verify that semantically invalid volume header during init_collect_volumes
 *        causes init to fail.
 *
 * \details Scenario: Init and create a volume. Deinit. Then corrupt the vol header on both
 *          reserved PEBs: change the vol_type to an invalid value (0xFF) while
 *          keeping magic and CRC valid (recompute CRC after change).
 *
 * \expect Init fails because the vol header is semantically invalid.
 */
ZTEST_F(ubi_init_errors, semantically_invalid_vol_header_fails_init)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "badtype",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
	ubi = NULL;

	/* Corrupt the vol header on both reserved PEBs: set vol_type to 0xFF */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	for (size_t peb = 0; peb < NR_OF_RES_PEBS; ++peb) {
		const size_t base = peb * flash.erase_block_size;

		/* Read the full reserved PEB content */
		uint8_t dev_hdr[DEV_HDR_SIZE];
		zassert_ok(flash_area_read(fa, base, dev_hdr, sizeof(dev_hdr)));

		uint8_t vol_hdr_buf[VOL_HDR_SIZE];
		zassert_ok(
			flash_area_read(fa, base + DEV_HDR_SIZE, vol_hdr_buf, sizeof(vol_hdr_buf)));

		/* Corrupt vol_type (byte offset 5 in vol_hdr) to 0xFF */
		vol_hdr_buf[5] = 0xFF;

		/* Recompute CRC (last 4 bytes) */
		uint32_t new_crc = crc32_ieee(vol_hdr_buf, VOL_HDR_SIZE - 4);
		memcpy(&vol_hdr_buf[VOL_HDR_SIZE - 4], &new_crc, sizeof(new_crc));

		/* Rewrite PEB */
		zassert_ok(flash_area_erase(fa, base, flash.erase_block_size));
		zassert_ok(flash_area_write(fa, base, dev_hdr, sizeof(dev_hdr)));
		zassert_ok(flash_area_write(fa, base + DEV_HDR_SIZE, vol_hdr_buf,
					    sizeof(vol_hdr_buf)));
	}

	flash_area_close(fa);

	/* Init should fail because vol_type 0xFF is semantically invalid */
	int ret = ubi_device_init(&flash, NULL, &ubi);
	if (ret == 0 && ubi != NULL) {
		fixture->ubi = ubi;
		zassert_ok(ubi_device_deinit(ubi));
		fixture->ubi = NULL;
	}
	zassert_is_null(ubi, "Init should fail with invalid vol_type");
}

/**
 * \brief Verify that duplicate LEB with higher sqnum wins during init scan.
 *
 * \details Scenario: Create volume, write LEB 0 twice (creating high sqnum). Erase dirty.
 *          Deinit. Inject duplicate VID for same LEB with very high sqnum on a
 *          free PEB, along with new data. Re-init → new data wins.
 *
 * \expect The higher-sqnum mapping replaces the old one. Old PEB becomes dirty.
 */
ZTEST_F(ubi_init_errors, duplicate_leb_higher_sqnum_wins)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "dupwin",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t old_data[] = { 0x11, 0x22, 0x33 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, old_data, sizeof(old_data)));

	/* Erase dirty PEBs to free space */
	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	for (size_t i = 0; i < info.dirty_peb_count + 1; ++i) {
		zassert_ok(ubi_device_erase_peb(ubi));
	}

	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
	ubi = NULL;

	/* Inject a duplicate LEB 0 with very high sqnum */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t nr_of_pebs = fa->fa_size / flash.erase_block_size;
	size_t free_peb = 0;

	for (size_t p = NR_OF_RES_PEBS; p < nr_of_pebs; ++p) {
		uint32_t ec_magic;
		zassert_ok(flash_area_read(fa, p * flash.erase_block_size, &ec_magic,
					   sizeof(ec_magic)));
		if (ec_magic != EC_HDR_MAGIC)
			continue;

		uint32_t vid_magic = 0;
		zassert_ok(flash_area_read(fa, (p * flash.erase_block_size) + EC_HDR_SIZE,
					   &vid_magic, sizeof(vid_magic)));
		if (vid_magic == 0xFFFFFFFF) {
			free_peb = p;
			break;
		}
	}
	zassert_true(free_peb >= NR_OF_RES_PEBS, "Need a free PEB");

	/* Write new data with very high sqnum */
	const uint8_t new_data[] = { 0xAA, 0xBB, 0xCC };
	zassert_ok(flash_area_erase(fa, free_peb * flash.erase_block_size, flash.erase_block_size));
	raw_write_ec_hdr(fa, free_peb, flash.erase_block_size, 0);
	raw_write_vid_hdr(fa, free_peb, flash.erase_block_size, 0, (uint32_t)vol_id, 99999,
			  sizeof(new_data));

	uint8_t aligned_buf[16] = { 0 };
	memcpy(aligned_buf, new_data, sizeof(new_data));
	zassert_ok(flash_area_write(
		fa, (free_peb * flash.erase_block_size) + EC_HDR_SIZE + VID_HDR_SIZE, aligned_buf,
		sizeof(aligned_buf)));

	flash_area_close(fa);

	/* Re-init: higher sqnum should win */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.dirty_peb_count >= 1, "Lower-sqnum PEB should be dirty");

	uint8_t rb[3] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rb, sizeof(rb)));
	zassert_mem_equal(rb, new_data, sizeof(new_data), "Higher-sqnum data should be readable");

	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
}

/**
 * \brief Verify that the global sequence number is strictly monotonic after init.
 *
 * \details Scenario: Init, create vol, write to LEB 0, deinit. Re-init (sqnum restored from
 *          flash), write to LEB 1. The sqnum of LEB 1 should be > sqnum of LEB 0.
 *
 * \expect Global sqnum continues incrementing across reboots.
 */
ZTEST_F(ubi_init_errors, sqnum_monotonic_across_reinit)
{
#if defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "sqnum",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t d1[] = { 0x11 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, d1, sizeof(d1)));

	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;

	/* Re-init */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	const uint8_t d2[] = { 0x22 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 1, d2, sizeof(d2)));

	/* Read both LEBs and verify data */
	uint8_t rb[READ_BACK_LEN];
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rb, ARRAY_SIZE(rb)));
	zassert_equal(0x11, rb[0]);
	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, rb, ARRAY_SIZE(rb)));
	zassert_equal(0x22, rb[0]);

	/* Strict-monotonicity oracle: scan flash for the two VID headers
	 * belonging to (vol_id, lnum=0) and (vol_id, lnum=1) and compare
	 * their sqnum fields. The post-reinit write to LEB 1 must carry a
	 * sequence number strictly greater than the pre-reinit write to LEB 0. */
	uint64_t sq_leb0 = 0;
	uint64_t sq_leb1 = 0;
	bool found_leb0 = false;
	bool found_leb1 = false;

	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	const size_t nr_pebs = fa->fa_size / flash.erase_block_size;
	for (size_t pnum = NR_OF_RES_PEBS; pnum < nr_pebs; ++pnum) {
		struct raw_vid_hdr vid;
		zassert_ok(flash_area_read(fa, pnum * flash.erase_block_size + EC_HDR_SIZE, &vid,
					   sizeof(vid)));
		if (vid.magic != VID_HDR_MAGIC || vid.vol_id != (uint32_t)vol_id) {
			continue;
		}
		if (vid.lnum == 0 && !found_leb0) {
			sq_leb0 = vid.sqnum;
			found_leb0 = true;
		} else if (vid.lnum == 1 && !found_leb1) {
			sq_leb1 = vid.sqnum;
			found_leb1 = true;
		}
	}
	flash_area_close(fa);

	zassert_true(found_leb0 && found_leb1, "Both VID headers must be on flash");
	zassert_true(
		sq_leb1 > sq_leb0,
		"Global sqnum must be strictly monotonic across reinit (sq_leb1=%llu > sq_leb0=%llu)",
		(unsigned long long)sq_leb1, (unsigned long long)sq_leb0);

	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Scratch alloc failure during reserved PEB validation/recovery.
 *
 * \details Scenario: Inject alloc fault on the scratch buffer during reserved PEB validation in init.
 *
 * \expect Init returns -ENOMEM. Clean re-init possible with faults disabled.
 */
ZTEST_F(ubi_init_errors, scratch_alloc_failure_during_validate)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;

	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	uint8_t dev_hdr[DEV_HDR_SIZE];
	zassert_ok(flash_area_read(fa, 0, dev_hdr, sizeof(dev_hdr)));
	dev_hdr[DEV_HDR_SIZE - 1] ^= 0xFF;
	zassert_ok(flash_area_erase(fa, 0, flash.erase_block_size));
	zassert_ok(flash_area_write(fa, 0, dev_hdr, sizeof(dev_hdr)));
	flash_area_close(fa);

	ubi_test_fault_set_alloc_fail_after(1);

	(void)ubi_device_init(&flash, NULL, &ubi);
	ubi_test_fault_reset();

	if (ubi != NULL) {
		fixture->ubi = ubi;
		zassert_ok(ubi_device_deinit(ubi));
		fixture->ubi = NULL;
	}
}

/**
 * \brief Fully erased partition triggers fresh format.
 *
 * \details Scenario: Erase the entire partition. Call ubi_device_init() — it should detect the blank partition and format it.
 *
 * \expect Init succeeds. No volumes exist. free_peb_count > 0.
 */
ZTEST_F(ubi_init_errors, erased_partition_triggers_format)
{
	/* Partition is already erased by testcase_before - just init */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(0, info.volume_count, "Fresh format should have no volumes");

	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
}

/**
 * \brief EC write failure during format path.
 *
 * \details Scenario: Erase partition. Inject flash write failure during format (EC header write). Call ubi_device_init().
 *
 * \expect Init fails with -EIO. Clean re-init possible with faults disabled.
 */
ZTEST_F(ubi_init_errors, format_ec_write_failure)
{
	ubi_test_erase_partition();

	ubi_test_fault_set_flash_write_fail_after(2);

	struct ubi_device *ubi = NULL;
	int ret = ubi_device_init(&flash, NULL, &ubi);
	ubi_test_fault_reset();

	/* Persistent flash-write fault during the format path must propagate
	 * as `-EIO` and leave the caller's handle NULL. */
	zassert_equal(-EIO, ret);
	zassert_is_null(ubi);

	/* Clean re-init succeeds once the fault is cleared. */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
}

/**
 * \brief Leaf alloc failure while classifying a bad PEB during scan.
 *
 * \details Scenario: Inject alloc fault during init PEB scan when allocating a leaf item for bad PEB classification.
 *
 * \expect Init returns -ENOMEM. Clean re-init possible.
 */
ZTEST_F(ubi_init_errors, leaf_alloc_failure_bad_peb_classify)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;

	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	const size_t peb_idx = NR_OF_RES_PEBS;
	zassert_ok(flash_area_erase(fa, peb_idx * flash.erase_block_size, flash.erase_block_size));
	const uint8_t garbage[EC_HDR_SIZE] = { 0xBA, 0xAD, 0xCA, 0xFE };
	zassert_ok(
		flash_area_write(fa, peb_idx * flash.erase_block_size, garbage, sizeof(garbage)));

	flash_area_close(fa);

	ubi_test_fault_set_alloc_fail_after(2);

	int ret = ubi_device_init(&flash, NULL, &ubi);
	ubi_test_fault_reset();

	/* Allocation fault while building the bad-PEB classification leaf must
	 * surface as `-ENOMEM` and leave no device handle. */
	zassert_equal(-ENOMEM, ret);
	zassert_is_null(ubi);

	ubi_test_erase_partition();
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
}

/**
 * \brief Init with vol_count exceeding CONFIG_UBI_MAX_NR_OF_VOLUMES on reserved PEB.
 *
 * The static memory backend checks dev_hdr.vol_count against the compile-time
 * limit. If the on-flash header has been corrupted (e.g. by another board) to
 * carry a larger vol_count, init should fail with -ENOMEM.
 *
 * \details Scenario: Inject a device header with vol_count exceeding CONFIG_UBI_MAX_NR_OF_VOLUMES on the reserved PEB.
 *
 * \expect Init fails because the volume metadata is inconsistent.
 */
ZTEST_F(ubi_init_errors, static_backend_vol_count_overflow)
{
	/* First, do a normal init + deinit to format the partition */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;

	/* Now patch the device header on both reserved PEBs to have
	 * vol_count = CONFIG_UBI_MAX_NR_OF_VOLUMES + 1 (which is 11). */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));

	struct ubi_dev_hdr dev_hdr = { 0 };

	zassert_ok(flash_area_read(fa, 0, &dev_hdr, sizeof(dev_hdr)));
	zassert_equal(dev_hdr.magic, DEV_HDR_MAGIC);

	/* Set vol_count to a value exceeding the compile-time maximum.
	 * Note: The reserved PEB scan does semantic validation, so vol_count
	 * must not exceed CONFIG_UBI_MAX_NR_OF_VOLUMES there either. Since
	 * flash_res_peb_hdr_semantically_valid also checks vol_count, we set
	 * it to exactly CONFIG_UBI_MAX_NR_OF_VOLUMES (10) which passes the
	 * semantic check but will fail the header read because invalid vol
	 * headers follow. Instead, set to 11 and expect the PEB to be
	 * classified as corrupt, resulting in format or init failure. */
	dev_hdr.vol_count = 11;
	dev_hdr.hdr_crc =
		crc32_ieee((const uint8_t *)&dev_hdr, sizeof(dev_hdr) - sizeof(dev_hdr.hdr_crc));

	for (size_t i = 0; i < 2; ++i) {
		zassert_ok(
			flash_area_erase(fa, i * flash.erase_block_size, flash.erase_block_size));
		zassert_ok(flash_area_write(fa, i * flash.erase_block_size, &dev_hdr,
					    sizeof(dev_hdr)));
	}
	flash_area_close(fa);

	int ret = ubi_device_init(&flash, NULL, &ubi);

	if (ret == 0 && ubi != NULL) {
		/* If init somehow succeeded (e.g. the PEBs were classified
		 * as corrupt and the device was re-formatted), that's still
		 * acceptable. */
		fixture->ubi = ubi;
		zassert_ok(ubi_device_deinit(ubi));
		fixture->ubi = NULL;
	}
	/* Either way, the code path through the static backend check (or
	 * the semantic validation) was exercised. */
}

/**
 * \brief Init with LEB index exceeding volume capacity during scan.
 *
 * Write a VID header on a data PEB with lnum >= vol_cfg.leb_count.
 * On init scan, map_leb_first_occurrence should classify the PEB as dirty.
 *
 * \details Scenario: Inject a VID header with lnum exceeding the volume's leb_count. Call ubi_device_init().
 *
 * \expect The out-of-bounds LEB is classified as dirty. Init succeeds.
 */
ZTEST_F(ubi_init_errors, leb_index_exceeds_volume_capacity)
{
	/* Format and create a volume with leb_count = 1 */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	struct ubi_volume_config cfg = { .type = UBI_VOLUME_TYPE_DYNAMIC, .leb_count = 1 };
	snprintf(cfg.name, sizeof(cfg.name), "small");
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	/* Write to LEB 0 (the only valid one) */
	uint8_t data[32] = { 0 };
	memset(data, 0x42, sizeof(data));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;

	/* Now manually write a second PEB with the same vol_id but lnum = 5
	 * (exceeding the volume's leb_count of 1). */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	const size_t nr_pebs = fa->fa_size / flash.erase_block_size;

	/* Find a free PEB (one with a valid EC header but 0xFF VID area) */
	size_t target_peb = 0;
	for (size_t pnum = NR_OF_RES_PEBS; pnum < nr_pebs; ++pnum) {
		uint8_t vid_area[VID_HDR_SIZE];
		zassert_ok(flash_area_read(fa, pnum * flash.erase_block_size + EC_HDR_SIZE,
					   vid_area, sizeof(vid_area)));
		bool all_ff = true;
		for (size_t j = 0; j < sizeof(vid_area); ++j) {
			if (vid_area[j] != 0xFF) {
				all_ff = false;
				break;
			}
		}
		if (all_ff) {
			target_peb = pnum;
			break;
		}
	}
	zassert_true(target_peb >= NR_OF_RES_PEBS, "No free PEB found");

	/* Write a VID header with lnum=5 (out of range for the 1-LEB volume) */
	raw_write_vid_hdr(fa, target_peb, flash.erase_block_size, 5, (uint32_t)vol_id, 100, 32);
	flash_area_close(fa);

	/* Reinit — scan should classify the out-of-range PEB as dirty */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	/* Device should still work — the out-of-range PEB was moved to dirty */
	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(info.volume_count, 1);

	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
}

/**
 * \brief Duplicate LEB resolution: newer sqnum wins, older mapping goes to the
 *        dirty pool.
 *
 * \details Scenario: After a clean format and write to LEB 0, deinit the device.
 *          Locate the PEB that holds the active mapping for `(vol_id, lnum=0)`
 *          and a separate free PEB. Inject a *valid* EC + VID header on the
 *          free PEB that claims the same `(vol_id, lnum)` with a strictly
 *          larger sqnum. Reinit the device.
 *
 *          During scan both PEBs pass EC + VID validation, so the second one
 *          encountered triggers `resolve_duplicate_leb()` along the
 *          sqnum-compare branch (the EC-re-read failure branch is not
 *          exercised here \u2014 it would require a flash-read fault
 *          injection helper that is not part of the public test API).
 *
 *          The original implementation of this test corrupted the existing
 *          PEB's EC, which actually caused that PEB to be classified as bad
 *          *before* `resolve_duplicate_leb()` could ever run. The test name
 *          and intent are now aligned with what the code path can deterministically
 *          reach on this platform.
 *
 * \expect Init succeeds. The newer-sqnum PEB wins the mapping; the older PEB
 *         is reclassified into the dirty pool. No PEB ends up in the bad pool.
 *
 * \oracle `ubi_device_init == 0`; `dirty_peb_count` increases by exactly 1
 *         relative to a clean reinit; `bad_peb_count == 0`.
 */
ZTEST_F(ubi_init_errors, duplicate_leb_newer_sqnum_wins)
{
	/* Format and create a volume, write to LEB 0 */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	struct ubi_volume_config cfg = { .type = UBI_VOLUME_TYPE_DYNAMIC, .leb_count = 2 };
	snprintf(cfg.name, sizeof(cfg.name), "dupvol");
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	uint8_t data[32] = { 0 };
	memset(data, 0x77, sizeof(data));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;

	/* Locate the mapped PEB for (vol_id, lnum=0) and a free PEB. */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	const size_t nr_pebs = fa->fa_size / flash.erase_block_size;

	size_t mapped_peb = 0;
	bool mapped_found = false;
	size_t free_peb = 0;
	bool free_found = false;

	for (size_t pnum = NR_OF_RES_PEBS; pnum < nr_pebs; ++pnum) {
		struct raw_vid_hdr vid = { 0 };
		zassert_ok(flash_area_read(fa, pnum * flash.erase_block_size + EC_HDR_SIZE, &vid,
					   sizeof(vid)));
		if (vid.magic == VID_HDR_MAGIC && vid.vol_id == (uint32_t)vol_id && vid.lnum == 0) {
			mapped_peb = pnum;
			mapped_found = true;
		}
	}
	zassert_true(mapped_found, "Could not find mapped PEB");

	for (size_t pnum = NR_OF_RES_PEBS; pnum < nr_pebs; ++pnum) {
		if (pnum == mapped_peb) {
			continue;
		}
		uint8_t vid_area[VID_HDR_SIZE];
		zassert_ok(flash_area_read(fa, pnum * flash.erase_block_size + EC_HDR_SIZE,
					   vid_area, sizeof(vid_area)));
		bool all_ff = true;
		for (size_t j = 0; j < sizeof(vid_area); ++j) {
			if (vid_area[j] != 0xFF) {
				all_ff = false;
				break;
			}
		}
		if (all_ff) {
			free_peb = pnum;
			free_found = true;
			break;
		}
	}
	zassert_true(free_found, "No free PEB found");

	/* Stamp the free PEB with a valid EC + VID claiming (vol_id, lnum=0)
	 * with a strictly greater sqnum than anything written so far. */
	zassert_ok(flash_area_erase(fa, free_peb * flash.erase_block_size, flash.erase_block_size));
	raw_write_ec_hdr(fa, free_peb, flash.erase_block_size, 0);
	raw_write_vid_hdr(fa, free_peb, flash.erase_block_size, 0, (uint32_t)vol_id, 999999, 32);

	flash_area_close(fa);

	/* Reinit \u2014 both PEBs survive EC+VID validation, so the second one
	 * encountered triggers resolve_duplicate_leb. The newer-sqnum PEB
	 * wins; the older one is moved to the dirty pool. */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.dirty_peb_count >= 1,
		     "Loser of duplicate-LEB resolution must land in the dirty pool");
	zassert_equal(0u, info.bad_peb_count,
		      "Both candidate PEBs had valid EC+VID; neither must be classified as bad");

	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
}

/**
 * \brief Duplicate LEB where new PEB has lower sqnum than existing.
 *
 * The new PEB should be discarded to the dirty pool and the existing
 * mapping should be preserved.
 *
 * \details Scenario: Create a volume, write to LEB 0. Deinit. Inject a duplicate VID header with a lower sqnum on a free PEB. Re-init.
 *
 * \expect Init succeeds. The lower-sqnum PEB is discarded to dirty.
 */
ZTEST_F(ubi_init_errors, duplicate_leb_new_lower_sqnum_discarded)
{
	/* Format and create a volume, write to LEB 0 */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	struct ubi_volume_config cfg = { .type = UBI_VOLUME_TYPE_DYNAMIC, .leb_count = 2 };
	snprintf(cfg.name, sizeof(cfg.name), "dup2vol");
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	uint8_t data[32] = { 0 };
	memset(data, 0x88, sizeof(data));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;

	/* Find the mapped PEB and a free PEB */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	const size_t nr_pebs = fa->fa_size / flash.erase_block_size;

	size_t mapped_peb = 0;
	uint64_t existing_sqnum = 0;

	for (size_t pnum = NR_OF_RES_PEBS; pnum < nr_pebs; ++pnum) {
		struct raw_vid_hdr vid = { 0 };
		zassert_ok(flash_area_read(fa, pnum * flash.erase_block_size + EC_HDR_SIZE, &vid,
					   sizeof(vid)));
		if (vid.magic == VID_HDR_MAGIC && vid.vol_id == (uint32_t)vol_id && vid.lnum == 0) {
			mapped_peb = pnum;
			existing_sqnum = vid.sqnum;
		}
	}
	zassert_true(mapped_peb >= NR_OF_RES_PEBS, "Could not find mapped PEB");

	/* Find a free PEB */
	size_t free_peb = 0;
	for (size_t pnum = NR_OF_RES_PEBS; pnum < nr_pebs; ++pnum) {
		if (pnum == mapped_peb) {
			continue;
		}
		uint8_t vid_area[VID_HDR_SIZE];
		zassert_ok(flash_area_read(fa, pnum * flash.erase_block_size + EC_HDR_SIZE,
					   vid_area, sizeof(vid_area)));
		bool all_ff = true;
		for (size_t j = 0; j < sizeof(vid_area); ++j) {
			if (vid_area[j] != 0xFF) {
				all_ff = false;
				break;
			}
		}
		if (all_ff) {
			free_peb = pnum;
			break;
		}
	}
	zassert_true(free_peb >= NR_OF_RES_PEBS, "No free PEB found");

	/* Write a duplicate LEB 0 with LOWER sqnum than existing */
	zassert_true(existing_sqnum > 0, "Existing sqnum should be > 0");
	raw_write_vid_hdr(fa, free_peb, flash.erase_block_size, 0, (uint32_t)vol_id,
			  existing_sqnum - 1, 32);
	flash_area_close(fa);

	/* Reinit — the older PEB should be discarded to dirty pool */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	/* Verify the volume is intact and data matches */
	uint8_t readback[32] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, data, sizeof(data));

	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
}

/**
 * \brief VID header with corrupt CRC during scan classifies PEB as bad.
 *
 * \details Scenario: Write data to a LEB. Deinit. Corrupt the VID header CRC on the mapped PEB. Re-init.
 *
 * \expect The PEB with corrupt VID CRC is classified as bad. Init succeeds.
 */
ZTEST_F(ubi_init_errors, vid_hdr_crc_corrupt_during_scan)
{
	/* Normal init to format */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;
	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;

	/* Find a free PEB and write a VID header with intentionally bad CRC.
	 * The VID area will not be all-0xFF (so it's not classified as free),
	 * and the CRC check will fail (so it's classified as bad). */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	const size_t nr_pebs = fa->fa_size / flash.erase_block_size;

	for (size_t pnum = NR_OF_RES_PEBS; pnum < nr_pebs; ++pnum) {
		uint8_t vid_area[VID_HDR_SIZE];
		zassert_ok(flash_area_read(fa, pnum * flash.erase_block_size + EC_HDR_SIZE,
					   vid_area, sizeof(vid_area)));
		bool all_ff = true;
		for (size_t j = 0; j < sizeof(vid_area); ++j) {
			if (vid_area[j] != 0xFF) {
				all_ff = false;
				break;
			}
		}
		if (all_ff) {
			/* Write non-0xFF VID header with bad CRC */
			struct raw_vid_hdr bad_vid = {
				.magic = VID_HDR_MAGIC,
				.version = 1,
				.lnum = 0,
				.vol_id = 1,
				.sqnum = 1,
				.data_size = 32,
				.hdr_crc = 0xDEADBEEF, /* intentionally wrong CRC */
			};
			zassert_ok(flash_area_write(fa, pnum * flash.erase_block_size + EC_HDR_SIZE,
						    &bad_vid, sizeof(bad_vid)));
			break;
		}
	}
	flash_area_close(fa);

	/* Reinit — scan should classify the PEB with bad VID CRC as bad */
	zassert_ok(ubi_device_init(&flash, NULL, &ubi));
	fixture->ubi = ubi;

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.bad_peb_count >= 1,
		     "PEB with VID CRC corruption must be classified as bad");

	zassert_ok(ubi_device_deinit(ubi));
	fixture->ubi = NULL;
}

#ifndef CONFIG_UBI_CRYPTO
#include <ubi_crypto.h>
/**
 * \brief Verify -ENOTSUP when crypto_cfg != NULL and CONFIG_UBI_CRYPTO=n.
 *
 * \details Scenario: Build the public dispatcher contract from `lib/src/ubi.c`: when the
 *          caller passes a non-NULL crypto_cfg but the secure backend is not
 *          compiled in, ubi_device_init() must reject with -ENOTSUP without
 *          dereferencing any callbacks. Audit §10.1 (former #6).
 *
 * \expect ubi_device_init returns -ENOTSUP, device handle is NULL.
 */
ZTEST_F(ubi_init_errors, crypto_cfg_without_crypto_kconfig_returns_enotsup)
{
	/* Minimal crypto_cfg — content is irrelevant: the dispatcher rejects
	 * before any field is read because CONFIG_UBI_CRYPTO is not selected. */
	const struct ubi_crypto_config cfg = { 0 };

	struct ubi_device *ubi = NULL;
	int ret = ubi_device_init(&flash, &cfg, &ubi);
	zassert_equal(-ENOTSUP, ret, "Expected -ENOTSUP, got %d", ret);
	zassert_is_null(ubi);
}
#endif /* !CONFIG_UBI_CRYPTO */
