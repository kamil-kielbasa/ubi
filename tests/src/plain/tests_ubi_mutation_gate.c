/**
 * \file    tests_ubi_mutation_gate.c
 *
 * \brief   Tests for the central mutation gate.
 *
 * Verifies that:
 *   1. Degraded mode blocks volume create / resize / remove with -EROFS.
 *   2. The test-only write shutdown flag blocks ALL mutators with -EROFS.
 *   3. Read operations are unaffected by write shutdown.
 *
 * Requires CONFIG_UBI_TEST_API_ENABLE=y and CONFIG_UBI_TEST_FAULT_INJECTION=y.
 *
 * \copyright Copyright (c) 2026
 *
 */

/* Include files ------------------------------------------------------------------------------- */

/* UBI header: */
#include <ubi.h>
#include <ubi_test.h>
#include "ubi_test_fixture.h"
#include "ubi_test_memory.h"

/* Zephyr headers: */
#include <zephyr/ztest.h>
#include <zephyr/storage/flash_map.h>

/* Standard headers: */
#include <string.h>

/* Module defines ------------------------------------------------------------------------------ */

#define DEV_HDR_SIZE (32U)
#define VOL_HDR_SIZE (48U)
#define NR_OF_RES_PEBS (2U)

/* Module types and type definitiones ---------------------------------------------------------- */
/* Module interface variables and constants ---------------------------------------------------- */
/* Static variables and constants -------------------------------------------------------------- */

static struct ubi_mtd mtd = { 0 };
static struct ubi_device *g_ubi;

/* Static function declarations ---------------------------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_testcase_before(void *ctx);
static void ztest_testcase_teardown(void *ctx);

/* Static function definitions ----------------------------------------------------------------- */

static void *ztest_suite_setup(void)
{
	ubi_test_setup_mtd(&mtd);
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
#if defined(CONFIG_UBI_TEST_API_ENABLE)
		ubi_test_set_write_shutdown(g_ubi, false);
#endif
		(void)ubi_device_deinit(g_ubi);
		g_ubi = NULL;
	}
}

/* Module interface function definitions ------------------------------------------------------- */

ZTEST_SUITE(ubi_mutation_gate, NULL, ztest_suite_setup, ztest_testcase_before,
	    ztest_testcase_teardown, NULL);

/**
 * \brief Verify that the test write shutdown flag blocks all public mutators.
 *
 * \details Enable write shutdown via the test API. Then call every public
 *          mutator and verify that each returns -EROFS. Read-only operations
 *          (get_info, leb_read, leb_is_mapped, leb_get_size) must still work.
 *
 * \expect  All mutators return -EROFS. All readers return 0 or valid data.
 */
ZTEST(ubi_mutation_gate, write_shutdown_blocks_all_mutators)
{
#if defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = ubi_test_init_device(&mtd);
	g_ubi = ubi;

	/* Create a volume and write data so we can test read paths too. */
	const struct ubi_volume_config cfg = {
		.name = "gatevol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[16] = {
		0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
		0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
	};
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	/* Erase dirty PEBs so the device is clean. */
	for (int i = 0; i < 5; ++i) {
		(void)ubi_device_erase_peb(ubi);
	}

	/* Enable global write shutdown. */
	ubi_test_set_write_shutdown(ubi, true);

	/* -- Reserved metadata mutators -- */
	const struct ubi_volume_config new_cfg = {
		.name = "blocked",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int blocked_id = -1;
	zassert_equal(-EROFS, ubi_volume_create(ubi, &new_cfg, &blocked_id),
		      "volume_create must be blocked");

	const struct ubi_volume_config resize_cfg = {
		.name = "gatevol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 3,
	};
	zassert_equal(-EROFS, ubi_volume_resize(ubi, vol_id, &resize_cfg),
		      "volume_resize must be blocked");

	zassert_equal(-EROFS, ubi_volume_remove(ubi, vol_id), "volume_remove must be blocked");

	/* -- Data-path mutators -- */
	const uint8_t new_data[16] = {
		0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11,
		0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
	};
	zassert_equal(-EROFS, ubi_leb_write(ubi, vol_id, 1, new_data, sizeof(new_data)),
		      "leb_write must be blocked");

	zassert_equal(-EROFS, ubi_leb_map(ubi, vol_id, 1), "leb_map must be blocked");

	zassert_equal(-EROFS, ubi_leb_unmap(ubi, vol_id, 0), "leb_unmap must be blocked");

	/* -- Maintenance mutators -- */
	zassert_equal(-EROFS, ubi_device_erase_peb(ubi), "erase_peb must be blocked");

	/* -- Read-only operations must still work -- */
	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count, "get_info should still work");

	uint8_t readback[16] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, data, sizeof(data), "leb_read should still work");

	bool is_mapped = false;
	zassert_ok(ubi_leb_is_mapped(ubi, vol_id, 0, &is_mapped));
	zassert_true(is_mapped, "leb_is_mapped should still work");

	size_t size = 0;
	zassert_ok(ubi_leb_get_size(ubi, vol_id, 0, &size));
	zassert_equal(sizeof(data), size, "leb_get_size should still work");

	struct ubi_volume_config vol_info = { 0 };
	size_t alloc_lebs = 0;
	zassert_ok(ubi_volume_get_info(ubi, vol_id, &vol_info, &alloc_lebs));
	zassert_equal(1, alloc_lebs, "volume_get_info should still work");

	/* Disable shutdown and verify mutators work again. */
	ubi_test_set_write_shutdown(ubi, false);

	zassert_ok(ubi_leb_write(ubi, vol_id, 1, new_data, sizeof(new_data)),
		   "leb_write should work after shutdown is lifted");

	zassert_ok(ubi_device_check_invariants(ubi));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Verify that degraded mode blocks volume create, resize, and remove.
 *
 * \details Corrupt reserved PEBs to attempt degraded mode entry. If the flash
 *          simulator successfully recovers (writes/erases never fail), the test
 *          is skipped. When degraded mode is achieved, all three
 *          reserved-metadata mutators must return -EROFS.
 *
 * \expect  volume_create/resize/remove return -EROFS in degraded mode.
 */
ZTEST(ubi_mutation_gate, degraded_mode_blocks_reserved_metadata_only)
{
	/* Normal init with a volume. */
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(&mtd, NULL, &ubi));
	g_ubi = ubi;

	const struct ubi_volume_config cfg = {
		.name = "degvol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;
	g_ubi = NULL;

	/* Corrupt both reserved PEB CRCs so init sees 0 active PEBs initially,
	 * but recovery from scratch_alloc path may still succeed on the simulator.
	 * If it does, we skip — degraded mode is not achievable here. */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(mtd.partition_id, &fa));

	for (size_t peb = 0; peb < NR_OF_RES_PEBS; ++peb) {
		const size_t base = peb * mtd.erase_block_size;
		uint8_t hdr_buf[DEV_HDR_SIZE] = { 0 };

		zassert_ok(flash_area_read(fa, base, hdr_buf, sizeof(hdr_buf)));

		/* Flip CRC bytes to invalidate the header. */
		hdr_buf[DEV_HDR_SIZE - 1] ^= 0xFF;
		hdr_buf[DEV_HDR_SIZE - 2] ^= 0xFF;

		zassert_ok(flash_area_erase(fa, base, mtd.erase_block_size));
		zassert_ok(flash_area_write(fa, base, hdr_buf, sizeof(hdr_buf)));
	}

	flash_area_close(fa);

	int init_ret = ubi_device_init(&mtd, NULL, &ubi);

	if (init_ret != 0 || ubi == NULL) {
		/* Init failed entirely — can't test degraded mode. */
		ztest_test_skip();
		return;
	}

	g_ubi = ubi;

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));

	if (!info.read_only_degraded) {
		/* Recovery succeeded — can't test degraded mode on this simulator. */
		g_ubi = NULL;
		zassert_ok(ubi_device_deinit(ubi));
		ztest_test_skip();
		return;
	}

	/* Reserved-metadata mutators must be blocked. */
	const struct ubi_volume_config new_cfg = {
		.name = "blocked",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int blocked_id = -1;
	zassert_equal(-EROFS, ubi_volume_create(ubi, &new_cfg, &blocked_id),
		      "volume_create must return -EROFS in degraded mode");

	zassert_equal(-EROFS, ubi_volume_remove(ubi, vol_id),
		      "volume_remove must return -EROFS in degraded mode");

	const struct ubi_volume_config resize_cfg = {
		.name = "degvol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 3,
	};
	zassert_equal(-EROFS, ubi_volume_resize(ubi, vol_id, &resize_cfg),
		      "volume_resize must return -EROFS in degraded mode");

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that corrupting a reserved PEB at runtime triggers transparent
 *        recovery during the next volume operation.
 *
 * \details After init (both reserved PEBs healthy), corrupt one reserved PEB
 *          CRC directly on flash. The next volume_create triggers
 *          dev_hdr_read_and_bump → ubi_flash_res_peb_validate → scan + recovery.
 *          On the simulator, recovery always succeeds, so the device stays
 *          healthy (read_only_degraded remains false). The volume operation
 *          completes normally.
 *
 * \expect  Volume created successfully. Device info reports read_only_degraded
 *          as false (recovery repaired the corrupt PEB).
 */
ZTEST(ubi_mutation_gate, runtime_corrupt_peb_recovered_transparently)
{
	struct ubi_device *ubi = ubi_test_init_device(&mtd);
	g_ubi = ubi;

	/* Create a first volume so the reserved PEBs contain real data. */
	const struct ubi_volume_config cfg1 = {
		.name = "vol_before",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id1 = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg1, &vol_id1));

	/* Corrupt reserved PEB 0: flip the CRC bytes in the device header. */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(mtd.partition_id, &fa));

	uint8_t hdr_buf[DEV_HDR_SIZE] = { 0 };
	zassert_ok(flash_area_read(fa, 0, hdr_buf, sizeof(hdr_buf)));

	hdr_buf[DEV_HDR_SIZE - 1] ^= 0xFF;
	hdr_buf[DEV_HDR_SIZE - 2] ^= 0xFF;

	zassert_ok(flash_area_erase(fa, 0, mtd.erase_block_size));
	zassert_ok(flash_area_write(fa, 0, hdr_buf, sizeof(hdr_buf)));
	flash_area_close(fa);

	/* Next volume operation triggers validate → recovery of PEB 0. */
	const struct ubi_volume_config cfg2 = {
		.name = "vol_after",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id2 = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg2, &vol_id2));

	/* Recovery succeeded transparently — device must stay healthy. */
	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_false(info.read_only_degraded,
		      "Device must not be degraded after successful recovery");
	zassert_equal(2, info.volume_count);

	zassert_ok(ubi_device_check_invariants(ubi));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify that runtime reserved PEB degradation sets read_only_degraded
 *        and blocks subsequent mutations.
 *
 * \details After init (both reserved PEBs healthy), corrupt one reserved PEB
 *          and inject a flash erase fault so recovery cannot restore it. The
 *          next volume operation triggers validate → recovery fails → -EROFS.
 *          dev_hdr_read_and_bump sets the read_only_degraded flag. Subsequent
 *          mutations are blocked by the gate.
 *
 * \expect  volume_create returns -EROFS. get_info reports read_only_degraded
 *          as true. All subsequent mutations return -EROFS. Read operations
 *          still work.
 */
ZTEST(ubi_mutation_gate, runtime_degradation_sets_flag_and_blocks_mutations)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	struct ubi_device *ubi = ubi_test_init_device(&mtd);
	g_ubi = ubi;

	/* Create a volume so reserved PEBs contain real data. */
	const struct ubi_volume_config cfg1 = {
		.name = "vol_rt",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg1, &vol_id));

	/* Write data so we can verify reads still work. */
	const uint8_t data[16] = {
		0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE,
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
	};
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	/* Corrupt reserved PEB 0: flip CRC bytes in the device header. */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(mtd.partition_id, &fa));

	uint8_t hdr_buf[DEV_HDR_SIZE] = { 0 };
	zassert_ok(flash_area_read(fa, 0, hdr_buf, sizeof(hdr_buf)));

	hdr_buf[DEV_HDR_SIZE - 1] ^= 0xFF;
	hdr_buf[DEV_HDR_SIZE - 2] ^= 0xFF;

	zassert_ok(flash_area_erase(fa, 0, mtd.erase_block_size));
	zassert_ok(flash_area_write(fa, 0, hdr_buf, sizeof(hdr_buf)));
	flash_area_close(fa);

	/* Fault: next flash erase fails → recovery of PEB 0 is blocked. */
	ubi_test_fault_set_flash_erase_fail_after(0);

	/* Attempt volume_create → validate → recovery fails → -EROFS. */
	const struct ubi_volume_config cfg2 = {
		.name = "must_fail",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int blocked_id = -1;
	zassert_equal(-EROFS, ubi_volume_create(ubi, &cfg2, &blocked_id),
		      "volume_create must return -EROFS on runtime degradation");

	/* Reset faults so reads and get_info work normally. */
	ubi_test_fault_reset();

	/* Flag must be set. */
	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.read_only_degraded,
		     "read_only_degraded must be true after runtime degradation");

	/* Gate must block all reserved-metadata mutations. */
	zassert_equal(-EROFS, ubi_volume_create(ubi, &cfg2, &blocked_id),
		      "gate must block create after degradation");
	zassert_equal(-EROFS, ubi_volume_remove(ubi, vol_id),
		      "gate must block remove after degradation");

	const struct ubi_volume_config resize_cfg = {
		.name = "vol_rt",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	zassert_equal(-EROFS, ubi_volume_resize(ubi, vol_id, &resize_cfg),
		      "gate must block resize after degradation");

	/* Read operations must still work. */
	uint8_t readback[16] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, data, sizeof(data), "leb_read must still work");

	bool is_mapped = false;
	zassert_ok(ubi_leb_is_mapped(ubi, vol_id, 0, &is_mapped));
	zassert_true(is_mapped);

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Verify that erase_peb recovers the reserved PEB bank and clears
 *        the read_only_degraded flag.
 *
 * \details Enter runtime degraded mode (corrupt PEB + erase fault). Clear the
 *          erase fault so flash operations succeed again. Call erase_peb which
 *          attempts reserved PEB recovery as part of its maintenance cycle.
 *          After recovery, the degraded flag must be cleared and volume
 *          mutations must work again.
 *
 * \expect  After erase_peb, read_only_degraded is false. volume_create works.
 */
ZTEST(ubi_mutation_gate, erase_peb_recovers_reserved_bank)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	struct ubi_device *ubi = ubi_test_init_device(&mtd);
	g_ubi = ubi;

	/* Create a volume so reserved PEBs contain real data. */
	const struct ubi_volume_config cfg1 = {
		.name = "vol_rec",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg1, &vol_id));

	/* Corrupt reserved PEB 0: flip CRC bytes in the device header. */
	const struct flash_area *fa = NULL;
	zassert_ok(flash_area_open(mtd.partition_id, &fa));

	uint8_t hdr_buf[DEV_HDR_SIZE] = { 0 };
	zassert_ok(flash_area_read(fa, 0, hdr_buf, sizeof(hdr_buf)));

	hdr_buf[DEV_HDR_SIZE - 1] ^= 0xFF;
	hdr_buf[DEV_HDR_SIZE - 2] ^= 0xFF;

	zassert_ok(flash_area_erase(fa, 0, mtd.erase_block_size));
	zassert_ok(flash_area_write(fa, 0, hdr_buf, sizeof(hdr_buf)));
	flash_area_close(fa);

	/* Fault: next flash erase fails → recovery of PEB 0 is blocked. */
	ubi_test_fault_set_flash_erase_fail_after(0);

	/* Trigger degradation: volume_create → validate → recovery fails. */
	const struct ubi_volume_config cfg_fail = {
		.name = "must_fail",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int blocked_id = -1;
	zassert_equal(-EROFS, ubi_volume_create(ubi, &cfg_fail, &blocked_id));

	/* Verify device is degraded. */
	ubi_test_fault_reset();

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_true(info.read_only_degraded, "device must be degraded");

	/* erase_peb triggers reserved PEB recovery as part of maintenance. */
	(void)ubi_device_erase_peb(ubi);

	/* After recovery, degraded flag must be cleared. */
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_false(info.read_only_degraded, "erase_peb must clear degraded flag after recovery");

	/* Volume mutations must work again. */
	const struct ubi_volume_config cfg2 = {
		.name = "vol_after_rec",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id2 = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg2, &vol_id2));

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(2, info.volume_count);

	zassert_ok(ubi_device_check_invariants(ubi));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}
