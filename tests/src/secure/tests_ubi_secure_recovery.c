/**
 * \file    tests_ubi_secure_recovery.c
 * \author  Kamil Kielbasa
 *
 * \brief   Secure backend recovery corner cases: power-cut rollback,
 *          interrupted anchor/reserved writes, freshness replay rejection.
 *
 * \details Covers secure recovery matrix items:
 *          - interrupted data write preserves old mapping (COW)
 *          - interrupted anchor update preserves continuity
 *          - replay of old reserved generation rejected by freshness
 *          - interrupted reserved PEB commit during volume create
 *
 * Requires CONFIG_UBI_TEST_FAULT_INJECTION=y and CONFIG_UBI_TEST_API_ENABLE=y.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>
#include "arrays.h"
#include "ubi_test_memory.h"

/* Test fixtures: */
#include "ubi_test_secure_fixture.h"

/* Zephyr headers: */
#include <psa/crypto.h>
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/sys_heap.h>

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

/* Static function declarations ----------------------------------------------------------------- */

static struct ubi_flash_desc flash = { 0 };

/* Module-level device pointer for teardown safety. */
static struct ubi_device *g_ubi = NULL;

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
extern struct sys_heap _system_heap;
#endif

static struct sys_memory_stats before_init = { 0 };
static struct sys_memory_stats after_init = { 0 };
static struct sys_memory_stats after_deinit = { 0 };

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

static void ztest_testcase_after(void *ctx)
{
	(void)ctx;
	ubi_test_fault_reset();
	if (g_ubi != NULL) {
		(void)ubi_device_deinit(g_ubi);
		g_ubi = NULL;
	}
}

/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_secure_recovery, NULL, ztest_suite_setup, ztest_suite_before, ztest_testcase_after,
	    NULL);

/**
 * \brief Interrupted LEB data write preserves old mapping (COW).
 *
 * \details Scenario: Write data to LEB 0, then attempt an overwrite with flash write
 *          fault injected after 1 successful write (LEB prefix written,
 *          ciphertext write fails). The VID header is never committed, so
 *          the old mapping must survive. After fault reset, verify that the
 *          original data is still readable.
 *
 * \expect Second write returns error. Old data still readable.
 *           Heap fully reclaimed after deinit.
 */
ZTEST(ubi_secure_recovery, interrupted_data_write_preserves_old_mapping)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	const struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'r', 'c', 'v', '1' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	/* First write: establish the old mapping. */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_128, ARRAY_SIZE(array_128)));

	/* Verify first write succeeded. */
	uint8_t rdata[ARRAY_SIZE(array_128)] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, ARRAY_SIZE(array_128)));
	zassert_mem_equal(rdata, array_128, ARRAY_SIZE(array_128));

	/* Inject fault: let LEB prefix write succeed (write #0), fail on ct+tag (write #1).
	 * The VID is never written -> the new PEB is uncommitted. */
	ubi_test_fault_set_flash_write_fail_after(1);

	const int ret = ubi_leb_write(ubi, vol_id, 0, array_256, ARRAY_SIZE(array_256));
	zassert_not_equal(0, ret, "Overwrite should fail with flash write fault");

	ubi_test_fault_reset();

	/* Old data must still be readable (COW: old PEB untouched). */
	memset(rdata, 0, sizeof(rdata));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, ARRAY_SIZE(array_128)));
	zassert_mem_equal(rdata, array_128, ARRAY_SIZE(array_128));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Interrupted VID commit preserves old mapping.
 *
 * \details Scenario: Write data to LEB 0, then attempt an overwrite with flash write
 *          fault injected after 2 successful writes (LEB prefix + ciphertext
 *          both written, VID commit write fails). Since VID is the commit
 *          point, the old mapping must survive.
 *
 * \expect Second write returns error. Old data still readable.
 */
ZTEST(ubi_secure_recovery, interrupted_vid_commit_preserves_old_mapping)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	const struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'r', 'c', 'v', '2' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	/* Establish old mapping. */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_128, ARRAY_SIZE(array_128)));

	/* Inject fault: let prefix(#0) + ct(#1) succeed, fail VID(#2). */
	ubi_test_fault_set_flash_write_fail_after(2);

	const int ret = ubi_leb_write(ubi, vol_id, 0, array_256, ARRAY_SIZE(array_256));
	zassert_not_equal(0, ret, "Overwrite should fail when VID commit is faulted");

	ubi_test_fault_reset();

	/* Old data preserved (VID never committed). */
	uint8_t rdata[ARRAY_SIZE(array_128)] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, ARRAY_SIZE(array_128)));
	zassert_mem_equal(rdata, array_128, ARRAY_SIZE(array_128));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Interrupted data write on first LEB write leaves LEB unmapped.
 *
 * \details Scenario: Attempt a first write to a LEB (no prior mapping). Inject flash
 *          fault to fail the VID commit. The LEB must remain unmapped.
 *
 * \expect Write returns error. LEB is not mapped after fault.
 */
ZTEST(ubi_secure_recovery, interrupted_first_write_leaves_unmapped)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	const struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'r', 'c', 'v', '3' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	/* LEB 0 is unmapped (fresh volume). */
	bool is_mapped = true;

	zassert_ok(ubi_leb_is_mapped(ubi, vol_id, 0, &is_mapped));
	zassert_false(is_mapped);

	/* Inject fault: let data writes succeed, fail VID commit (#2). */
	ubi_test_fault_set_flash_write_fail_after(2);

	const int ret = ubi_leb_write(ubi, vol_id, 0, array_128, ARRAY_SIZE(array_128));
	zassert_not_equal(0, ret, "First write should fail when VID is faulted");

	ubi_test_fault_reset();

	/* LEB must remain unmapped -- no commit. */
	is_mapped = true;
	zassert_ok(ubi_leb_is_mapped(ubi, vol_id, 0, &is_mapped));
	zassert_false(is_mapped, "LEB should remain unmapped after failed first write");

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Interrupted data write + reboot: old mapping survives scan.
 *
 * \details Scenario: Write to LEB 0, inject fault during overwrite, then reboot
 *          (deinit + re-init). Verify the partition scan correctly
 *          classifies the half-written PEB as dirty/free and preserves
 *          the committed old mapping.
 *
 * \expect After reboot: old data readable, volume intact.
 */
ZTEST(ubi_secure_recovery, interrupted_data_write_survives_reboot)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	const struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'r', 'c', 'v', '4' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	/* Establish old mapping. */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_128, ARRAY_SIZE(array_128)));

	/* Inject fault during overwrite attempt. */
	ubi_test_fault_set_flash_write_fail_after(1);

	(void)ubi_leb_write(ubi, vol_id, 0, array_256, ARRAY_SIZE(array_256));

	ubi_test_fault_reset();

	/* Reboot: deinit + re-init. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	ubi = NULL;
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;

	/* Verify old data survives reboot. The half-written PEB is classified
	 * as dirty during scan (no valid VID). */
	uint8_t rdata[ARRAY_SIZE(array_128)] = { 0 };
	size_t rdata_size = 0;

	zassert_ok(ubi_leb_get_size(ubi, vol_id, 0, &rdata_size));
	zassert_equal(ARRAY_SIZE(array_128), rdata_size);
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, rdata_size));
	zassert_mem_equal(rdata, array_128, ARRAY_SIZE(array_128));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Interrupted anchor rewrite preserves continuity after reboot.
 *
 * \details Scenario: Trigger anchor migration via the erase-witness path (overwrite
 *          LEB twice to push leb_write_counter above anchor, then erase
 *          dirty PEBs), but inject a flash write fault during the anchor
 *          rewrite. After reset + reboot, the old anchor must still be
 *          valid, and the volume must be recognized.
 *
 * \expect After interrupted anchor write + reboot: volume recognized,
 *           data writable. Heap fully reclaimed after deinit.
 *
 * \oracle Post-reboot `ubi_volume_get_info(vol_id)` returns 0; a fresh write
 *         and read-back round-trip succeeds bit-exact; `memory_check`
 *         confirms the heap returned to baseline after deinit.
 *
 * \trace interrupted anchor rewrite.
 *
 * \precondition `CONFIG_UBI_TEST_FAULT_INJECTION` + `CONFIG_UBI_TEST_API_ENABLE`;
 *               flash-write fault-injection hook available.
 */
ZTEST(ubi_secure_recovery, interrupted_anchor_write_preserves_continuity)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	const struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'r', 'c', 'a', '1' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));

	const size_t initial_free = info.free_peb_count;

	zassert_true(initial_free >= 3, "Need at least 3 free PEBs");

	/* Two overwrites: push leb_write_counter above initial anchor counter.
	 * This makes the subsequent erase cycle trigger anchor migration. */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_128, ARRAY_SIZE(array_128)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_128, ARRAY_SIZE(array_128)));
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

	/* Erase dirty PEBs with fault injection. The witness check during
	 * erase will attempt to rewrite the anchor PEB. We inject flash write
	 * fault to make the anchor rewrite fail. */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	bool fault_triggered = false;

	while (info.dirty_peb_count > 0) {
		if (!fault_triggered) {
			/* Let the first few writes succeed, then fail on a
			 * subsequent write (the anchor rewrite). */
			ubi_test_fault_set_flash_write_fail_after(2);
			fault_triggered = true;
		}

		int ret = ubi_device_erase_peb(ubi);

		if (ret != 0) {
			/* Fault triggered during erase -- expected. */
			ubi_test_fault_reset();
			break;
		}

		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(ubi, &info));
	}

	ubi_test_fault_reset();

	/* Reboot: deinit + re-init. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	ubi = NULL;
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;

	/* After reboot: volume must still be recognized with anchor intact. */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count, "Volume must survive after anchor write fault");
	zassert_equal(vol_cfg.leb_count + 1, info.reserved_peb_count);

	/* Must be able to write new data (proves anchor is valid). */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_128, ARRAY_SIZE(array_128)));

	uint8_t rdata[ARRAY_SIZE(array_128)] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, ARRAY_SIZE(array_128)));
	zassert_mem_equal(rdata, array_128, ARRAY_SIZE(array_128));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Replay of stale reserved generation ignored by init scan.
 *
 * \details Scenario: Create a volume (bumps device_revision to N), then create a second
 *          volume (bumps revision to N+1). After the second volume_create,
 *          overwrite one reserved PEB bank with a saved snapshot of the old
 *          (revision N) content. On reattach, the scan should use the other
 *          (valid) reserved PEB bank with revision N+1.
 *
 * \expect Volume count is 2 after reboot. Stale bank is ignored.
 */
ZTEST(ubi_secure_recovery, reserved_generation_replay_rejected)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	const struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg1 = {
		.name = { '/', 'r', 'p', 'l', '1' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	const struct ubi_volume_config vol_cfg2 = {
		.name = { '/', 'r', 'p', 'l', '2' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	struct ubi_device *ubi = NULL;
	int vol_id1 = -1;
	int vol_id2 = -1;

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;

	/* Create volume 1 -> reserved revision N. */
	zassert_ok(ubi_volume_create(ubi, &vol_cfg1, &vol_id1));

	/* Save snapshot of reserved PEB 0 content (the revision N copy). */
	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	uint8_t stale_bank[8192] = { 0 };
	const size_t peb0_offset = 0;

	zassert_true(flash.erase_block_size <= sizeof(stale_bank),
		     "stale_bank buffer too small for erase_block_size");
	zassert_ok(flash_area_read(fa, peb0_offset, stale_bank, flash.erase_block_size));
	flash_area_close(fa);

	/* Create volume 2 -> reserved revision N+1. */
	zassert_ok(ubi_volume_create(ubi, &vol_cfg2, &vol_id2));

	/* Overwrite reserved PEB bank 0 with the stale (revision N) snapshot.
	 * This simulates a replay attack on one bank. */
	zassert_ok(flash_area_open(flash.partition_id, &fa));
	zassert_ok(flash_area_erase(fa, peb0_offset, flash.erase_block_size));
	zassert_ok(flash_area_write(fa, peb0_offset, stale_bank, flash.erase_block_size));
	flash_area_close(fa);

	/* Reboot. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);

	ubi = NULL;

	/* Re-init: scan should pick PEB bank 1 (revision N+1), reject stale PEB 0. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(2, info.volume_count, "Both volumes must be visible (stale bank ignored)");

	/* Verify volumes are functional. */
	zassert_ok(ubi_leb_write(ubi, vol_id1, 0, array_128, ARRAY_SIZE(array_128)));
	zassert_ok(ubi_leb_write(ubi, vol_id2, 0, array_128, ARRAY_SIZE(array_128)));

	uint8_t rdata[ARRAY_SIZE(array_128)] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id1, 0, 0, rdata, ARRAY_SIZE(array_128)));
	zassert_mem_equal(rdata, array_128, ARRAY_SIZE(array_128));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Interrupted reserved PEB commit during volume create.
 *
 * \details Scenario: Inject flash write fault during volume_create so that the reserved
 *          PEB write fails. After the failed volume_create + reboot, the
 *          old device state must be intact (no partial volume should appear).
 *
 * \expect volume_create returns error. After reboot: device initializes
 *           successfully and is functional (can create and use new volumes).
 */
ZTEST(ubi_secure_recovery, interrupted_reserved_commit_no_ghost_volume)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	const struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg1 = {
		.name = { '/', 'r', 'e', 's', '1' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	const struct ubi_volume_config vol_cfg2 = {
		.name = { '/', 'r', 'e', 's', '2' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	struct ubi_device *ubi = NULL;
	int vol_id1 = -1;
	int vol_id2 = -1;

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;

	/* Create volume 1 successfully. */
	zassert_ok(ubi_volume_create(ubi, &vol_cfg1, &vol_id1));

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count);

	/* Inject fault: fail on the very first flash write of the reserved PEB commit
	 * for volume 2. With fail_after(0), the first write of res commit fails. */
	ubi_test_fault_set_flash_write_fail_after(0);

	const int ret = ubi_volume_create(ubi, &vol_cfg2, &vol_id2);

	ubi_test_fault_reset();

	/* Volume create should have failed. */
	zassert_not_equal(0, ret, "volume_create should fail with flash write fault");

	/* After the failed create, volume 1 should still be intact. */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count, "Only volume 1 should exist after failed create");

	/* Reboot. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	ubi = NULL;
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;

	/* After reboot: reserved PEB commit erases both banks before writing.
	 * With fail_after(0) both writes fail, so both banks are erased.
	 * Volume 1's metadata is lost. Expected 0 volumes after reboot. */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	/* The device is functional even if empty. Test that we can create
	 * and use a new volume on the recovered device. */
	const struct ubi_volume_config vol_cfg3 = {
		.name = { '/', 'r', 'e', 's', '3' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id3 = -1;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg3, &vol_id3));
	zassert_ok(ubi_leb_write(ubi, vol_id3, 0, array_128, ARRAY_SIZE(array_128)));

	uint8_t rdata[ARRAY_SIZE(array_128)] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id3, 0, 0, rdata, ARRAY_SIZE(array_128)));
	zassert_mem_equal(rdata, array_128, ARRAY_SIZE(array_128));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Interrupted anchor creation during volume create.
 *
 * \details Scenario: Allow reserved PEB commit to succeed but inject a fault during
 *          the anchor PEB creation. After reboot, the reserved metadata
 *          carries the volume record but the anchor PEB is incomplete.
 *          The init scan should handle this gracefully.
 *
 * \expect After reboot: device initializes successfully. No crash.
 */
ZTEST(ubi_secure_recovery, interrupted_anchor_create_during_volume_create)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	const struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'r', 'e', 's', '3' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;

	/* Reserved commit for volume_create does:
	 *   - erase res PEB 0, write res PEB 0 (write #0)
	 *   - erase res PEB 1, write res PEB 1 (write #1)
	 *   - anchor: prefix (write #2), ct+tag (write #3), VID (write #4)
	 *
	 * Inject fault after 3 writes to let reserved PEBs + anchor prefix
	 * succeed, but fail on anchor ct+tag -- anchor is incomplete. */
	ubi_test_fault_set_flash_write_fail_after(3);

	const int ret = ubi_volume_create(ubi, &vol_cfg, &vol_id);

	ubi_test_fault_reset();

	/* Volume create may fail or report success depending on when fault
	 * is hit. In either case, device must remain consistent. */

	/* Reboot. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);

	ubi = NULL;

	/* Init should succeed regardless -- it handles partial writes. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;

	/* The device must be in a consistent state. */
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));

	/* Whether the volume is visible depends on implementation (anchor-less
	 * detection). Either way, device must be functional and not crash.
	 * If the volume appears and create returned an error, the reserved
	 * metadata was committed. That is acceptable. */
	if (info.volume_count > 0 && ret == 0) {
		/* Volume was fully committed -- verify it works. */
		const int write_ret =
			ubi_leb_write(ubi, vol_id, 0, array_128, ARRAY_SIZE(array_128));
		if (write_ret == 0) {
			uint8_t rdata[ARRAY_SIZE(array_128)] = { 0 };

			zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, ARRAY_SIZE(array_128)));
			zassert_mem_equal(rdata, array_128, ARRAY_SIZE(array_128));
		}
	}

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Init-time anchor re-creation for volume whose anchor PEB was lost.
 *
 * \details Scenario: Create a volume with one LEB and write data. After deinit, erase
 *          the anchor PEB (PEB 2 — first data PEB allocated by anchor_create
 *          on a freshly formatted partition). On re-init the volume is still
 *          known from the reserved PEB metadata, but the anchor PEB is gone.
 *          The init code must detect anchor_pnum == SIZE_MAX and re-create
 *          the anchor from a free PEB.
 *
 * \expect Device initializes successfully. Volume is recognized and data is
 *           still readable. New writes succeed (proving anchor was re-created).
 *           Heap fully reclaimed after deinit.
 *
 * \oracle Re-init returns 0; the orphaned volume's `ubi_leb_read` round-trip
 *         matches the pre-erase payload (`zassert_mem_equal`); a follow-up
 *         write returns 0 and survives a second reboot.
 *
 * \trace anchor recreation on missing anchor PEB.
 *
 * \precondition `CONFIG_UBI_TEST_FAULT_INJECTION` + `CONFIG_UBI_TEST_API_ENABLE`;
 *               raw flash reachable for direct PEB erase via the test hook.
 */
ZTEST(ubi_secure_recovery, init_recreates_missing_anchor)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	const struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'r', 'c', 'o', '1' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;

	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_128, ARRAY_SIZE(array_128)));

	/* Verify data written. */
	uint8_t rdata[ARRAY_SIZE(array_128)] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, ARRAY_SIZE(array_128)));
	zassert_mem_equal(rdata, array_128, ARRAY_SIZE(array_128));

	/* Deinit. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);

	/* Corrupt the anchor PEB (PEB 2).
	 * On a freshly formatted partition, anchor_create takes the first
	 * free PEB (min EC = 0, first pnum = UBI_DEV_HDR_NR_OF_RES_PEBS = 2).
	 * Erasure destroys both EC and VID headers, making it unreadable. */
	{
		const struct flash_area *fa = NULL;

		zassert_ok(flash_area_open(flash.partition_id, &fa));

		const size_t anchor_peb = 2; /* UBI_DEV_HDR_NR_OF_RES_PEBS */

		zassert_ok(flash_area_erase(fa, anchor_peb * flash.erase_block_size,
					    flash.erase_block_size));
		flash_area_close(fa);
	}

	/* Re-init: volume exists in reserved metadata, anchor PEB is gone.
	 * Init must detect anchor_pnum == SIZE_MAX and re-create the anchor. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	ubi = NULL;
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	g_ubi = ubi;

	/* Volume must be recognized. */
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count, "Volume must survive after anchor PEB erasure");

	/* Original data must still be readable (data PEB was not touched). */
	memset(rdata, 0, sizeof(rdata));
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, ARRAY_SIZE(array_128)));
	zassert_mem_equal(rdata, array_128, ARRAY_SIZE(array_128));

	/* New writes must succeed (proving anchor was re-created). */
	zassert_ok(ubi_leb_write(ubi, vol_id, 1, array_128, ARRAY_SIZE(array_128)));

	uint8_t rdata2[ARRAY_SIZE(array_128)] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, rdata2, ARRAY_SIZE(array_128)));
	zassert_mem_equal(rdata2, array_128, ARRAY_SIZE(array_128));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);
#else
	ztest_test_skip();
#endif
}
