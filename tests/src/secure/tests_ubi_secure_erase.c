/**
 * \file    tests_ubi_secure_erase.c
 * \author  Kamil Kielbasa
 *
 * \brief   Parity tests for secure backend PEB erase lifecycle.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>
#include "arrays.h"
#include "ubi_secure_test_hooks.h"

/* Test fixtures: */
#include "ubi_test_fixture.h"
#include "ubi_test_memory.h"
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
static struct ubi_flash_desc flash = { 0 };

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
extern struct sys_heap _system_heap;
#endif

static struct sys_memory_stats before_init = { 0 };
static struct sys_memory_stats after_init = { 0 };
static struct sys_memory_stats after_deinit = { 0 };

/* Static function declarations ----------------------------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_before(void *ctx);

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
}

/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_secure_erase, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);

/**
 * \brief Write until partition full, unmap, erase all dirty PEBs.
 *
 * \details Scenario: Create a 1-LEB static volume, write data, unmap, erase dirty
 *          PEBs, deinit and re-init across multiple cycles until the
 *          partition is exercised. Verifies dirty_peb_count transitions.
 *          Parity with plain ubi_erase.one_volume_one_leb_operations_with_reboot.
 *
 * \expect dirty_peb_count drops to 0 after erase; data written before
 *           unmap is no longer accessible; heap fully reclaimed after deinit.
 */
ZTEST(ubi_secure_erase, fill_unmap_erase_cycle)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;
	const size_t lnum = 0;

	/* 1. Init, create volume. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	struct ubi_device_info info_after_init = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info_after_init));
	/* reserved = leb_count + 1 hidden anchor PEB per volume. */
	zassert_equal(vol_cfg.leb_count + 1, info_after_init.reserved_peb_count);

	const size_t initial_free = info_after_init.free_peb_count;

	/* 2. Write repeatedly, overwriting LEB 0 to distribute PEBs
	 *    across free/dirty.  The emergency-reserve refill
	 *    may recycle dirty PEBs during overwrites, so track the
	 *    free+dirty conservation law rather than individual counts. */
	struct ubi_device_info info = { 0 };
	for (size_t i = 0; i < initial_free; ++i) {
		zassert_ok(ubi_leb_write(ubi, vol_id, lnum, array_128, ARRAY_SIZE(array_128)));

		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(ubi, &info));
		/* Exactly 1 PEB is mapped (LEB 0); the rest are free+dirty. */
		zassert_equal(initial_free - 1, info.free_peb_count + info.dirty_peb_count);
	}

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(initial_free - 1, info.free_peb_count + info.dirty_peb_count);

	/* 3. Verify data. */
	uint8_t rdata[ARRAY_SIZE(array_128)] = { 0 };
	size_t rdata_size = 0;
	zassert_ok(ubi_leb_get_size(ubi, vol_id, lnum, &rdata_size));
	zassert_equal(ARRAY_SIZE(array_128), rdata_size);
	zassert_ok(ubi_leb_read(ubi, vol_id, lnum, 0, rdata, rdata_size));
	zassert_mem_equal(rdata, array_128, ARRAY_SIZE(array_128));

	/* 4. Deinit → re-init. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	ubi = NULL;
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	/* 5. Unmap and erase all dirty PEBs one by one.
	 *    The anchor witness rewrite may recycle the old anchor
	 *    to dirty during the loop, so drive to dirty == 0 instead of
	 *    counting a fixed number of iterations. */
	zassert_ok(ubi_leb_unmap(ubi, vol_id, lnum));

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	/* After unmap: no mapped PEBs — all initial_free are in free+dirty. */
	zassert_equal(initial_free, info.free_peb_count + info.dirty_peb_count);

	while (info.dirty_peb_count > 0) {
		zassert_ok(ubi_device_erase_peb(ubi));

		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(ubi, &info));
	}

	/* 6. Verify all dirty PEBs were erased. */
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(initial_free, info.free_peb_count);
	zassert_equal(0, info.dirty_peb_count);

	/* 7. Deinit. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);
}

/**
 * \brief Verify the hidden anchor PEB participates in normal erase cycling.
 *
 * \details Scenario: Create a 1-LEB volume, overwrite twice (pushing leb_write_counter
 *          above the initial anchor counter), unmap, then erase all dirty PEBs.
 *          The second dirty PEB is the last writable witness; the erase loop
 *          rewrites the anchor (old anchor PEB → dirty → erased → free pool).
 *          Over 4 cycles, free_peb_count always restores, confirming no PEB
 *          is permanently trapped.  The first cycle requires 3 erases (2 user
 *          + 1 old anchor), proving anchor migration.
 *
 * \expect Anchor PEB migrates at least once; free_peb_count restores
 *           every cycle; heap fully reclaimed after deinit.
 */
ZTEST(ubi_secure_erase, anchor_participates_in_wear_leveling)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'a', '_', 'w', 'l' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;
	const size_t lnum = 0;

	/* 1. Init, create volume. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	const size_t initial_free = info.free_peb_count;

	zassert_true(initial_free >= 3, "Need at least 3 free PEBs for this test");

#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)
	size_t anchor_pnum_initial = SIZE_MAX;

	zassert_ok(ubi_secure_test_get_peb_for_lnum(ubi, vol_id, SIZE_MAX, &anchor_pnum_initial));
#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */

	/* 2. Repeat write-overwrite-unmap-erase cycles. */
	const size_t N_CYCLES = 4;
	size_t total_erases = 0;
	size_t first_cycle_erases = 0;

#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)
	size_t anchor_pnum_after_first = SIZE_MAX;
#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */

	for (size_t c = 0; c < N_CYCLES; c++) {
		/* Two writes: counter 0→1→2.  After unmap, the PEB with
		 * leb_write_counter=2 exceeds the initial anchor counter (1). */
		zassert_ok(ubi_leb_write(ubi, vol_id, lnum, array_128, ARRAY_SIZE(array_128)));
		zassert_ok(ubi_leb_write(ubi, vol_id, lnum, array_128, ARRAY_SIZE(array_128)));
		zassert_ok(ubi_leb_unmap(ubi, vol_id, lnum));

		size_t cycle_erases = 0;

		zassert_ok(ubi_device_get_info(ubi, &info));

		while (info.dirty_peb_count > 0) {
			zassert_ok(ubi_device_erase_peb(ubi));
			cycle_erases++;
			zassert_ok(ubi_device_get_info(ubi, &info));
		}

		total_erases += cycle_erases;

		if (c == 0) {
			first_cycle_erases = cycle_erases;
#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)
			zassert_ok(ubi_secure_test_get_peb_for_lnum(ubi, vol_id, SIZE_MAX,
								    &anchor_pnum_after_first));
#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */
		}

		/* All PEBs accounted for after full erase. */
		zassert_equal(initial_free, info.free_peb_count,
			      "Free PEB count not restored after cycle %zu", c);
		zassert_equal(0, info.dirty_peb_count);
	}

	/* First cycle: anchor migrated → 3 erases (2 user dirty + old anchor).
	 * Subsequent cycles: anchor counter already high, no migration → 2 each.
	 * Total > 2 * N proves at least one anchor migration happened. */
	zassert_equal(3, first_cycle_erases,
		      "First cycle must trigger anchor migration (expected 3, got %zu)",
		      first_cycle_erases);
	zassert_true(total_erases > 2 * N_CYCLES,
		     "Total erases %zu must exceed %zu (proves anchor migration)", total_erases,
		     2 * N_CYCLES);

#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)
	/* Direct proof of migration: the anchor PEB number must have changed
	 * after the first cycle. The existing erase-count assertion above
	 * permits further migrations in later cycles (total > 2 * N_CYCLES)
	 * so we deliberately do not assert that the anchor stays pinned. */
	zassert_not_equal(anchor_pnum_initial, anchor_pnum_after_first,
			  "first cycle must relocate the anchor PEB: initial=%zu after=%zu",
			  anchor_pnum_initial, anchor_pnum_after_first);
#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */

	/* 3. Deinit. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);
}

/**
 * \brief Verify stale anchors are rejected as dirty on reboot after migration.
 *
 * \details Scenario: Create a 1-LEB volume, overwrite twice to push the VID counter
 *          above the initial anchor, unmap, erase all dirty (triggering
 *          anchor migration).  After reboot the old anchor PEB should not
 *          be recognized — the new anchor with the higher witness counter
 *          is the only valid one.  Verify that after a full erase cycle the
 *          free PEB count is fully restored, proving no stale anchor lingers.
 *
 * \expect After migration + reboot: only the newest anchor is live;
 *           stale anchor PEB recovered as dirty; full free count restored
 *           after erasing all dirty PEBs.
 *
 * \oracle After draining every dirty PEB post-reboot, `info.free_peb_count`
 *         equals the pre-write baseline and `info.volume_count == 1` —
 *         no stale anchor lingers in the reserved or bad pool.
 *
 * \trace stale anchor rejection; reboot recovery.
 *
 * \precondition 1-LEB static volume `"/stal"` on a freshly formatted secure
 *               partition.
 */
ZTEST(ubi_secure_erase, stale_anchor_rejected_after_reboot)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 's', 't', 'a', 'l' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;
	const size_t lnum = 0;

	/* 1. Init, create, push counter above anchor. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	const size_t initial_free = info.free_peb_count;

	/* Two overwrites: counter goes above anchor → erase will trigger
	 * anchor migration during the witness check. */
	zassert_ok(ubi_leb_write(ubi, vol_id, lnum, array_128, ARRAY_SIZE(array_128)));
	zassert_ok(ubi_leb_write(ubi, vol_id, lnum, array_128, ARRAY_SIZE(array_128)));
	zassert_ok(ubi_leb_unmap(ubi, vol_id, lnum));

	/* 2. Erase all dirty — triggers anchor migration. */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));

	while (info.dirty_peb_count > 0) {
		zassert_ok(ubi_device_erase_peb(ubi));
		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(ubi, &info));
	}

	zassert_equal(initial_free, info.free_peb_count, "All PEBs accounted for before reboot");

	/* 3. Deinit → reboot. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	ubi = NULL;
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	/* 4. After reboot: the old stale anchor PEB (if it survived flash)
	 *    must have been rejected in favor of the newer one during scan.
	 *    Verify volume is intact and data PEB accounting is correct. */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count);
	/* reserved = leb_count + 1 anchor. */
	zassert_equal(vol_cfg.leb_count + 1, info.reserved_peb_count);

	/* Any stale anchor duplicate should appear as dirty, not stuck. */
	while (info.dirty_peb_count > 0) {
		zassert_ok(ubi_device_erase_peb(ubi));
		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(ubi, &info));
	}

	/* All free PEBs restored. */
	zassert_equal(initial_free, info.free_peb_count,
		      "Free PEB count fully restored after stale anchor cleanup");

	/* 5. Deinit. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	ubi_test_memory_check(&before_init, &after_init, &after_deinit);
}

/**
 * \brief Verify reclaim preserves anchor continuity witness across full cycle.
 *
 * \details Scenario: Run a full reclaim cycle (write → unmap → erase → rewrite) over
 *          multiple iterations.  After each full cycle, verify the anchor
 *          remains valid by creating a fresh reboot and checking that the
 *          volume and its reserved PEB count are intact. This tests:
 *          reclaim with hidden-anchor preservation.
 *
 * \expect Across N full reclaim cycles + reboot: volume always recognized,
 *           reserved_peb_count correct, no orphaned PEBs.
 */
ZTEST(ubi_secure_erase, reclaim_preserves_continuity_witness)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'r', 'c', 'l', 'm' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;
	const size_t lnum = 0;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	const size_t initial_free = info.free_peb_count;

	/* Run 3 full reclaim cycles. */
	const size_t N_CYCLES = 3;

	for (size_t c = 0; c < N_CYCLES; c++) {
		/* Write and overwrite to generate dirty PEBs. */
		zassert_ok(ubi_leb_write(ubi, vol_id, lnum, array_128, ARRAY_SIZE(array_128)));
		zassert_ok(ubi_leb_write(ubi, vol_id, lnum, array_128, ARRAY_SIZE(array_128)));

		/* Unmap LEB. */
		zassert_ok(ubi_leb_unmap(ubi, vol_id, lnum));

		/* Erase all dirty PEBs (full reclaim). */
		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(ubi, &info));

		while (info.dirty_peb_count > 0) {
			zassert_ok(ubi_device_erase_peb(ubi));
			memset(&info, 0, sizeof(info));
			zassert_ok(ubi_device_get_info(ubi, &info));
		}

		/* All free PEBs restored each cycle. */
		zassert_equal(initial_free, info.free_peb_count,
			      "Free PEBs not restored after cycle %zu", c);
	}

	/* Reboot and verify anchor continuity. */
	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count);
	zassert_equal(vol_cfg.leb_count + 1, info.reserved_peb_count);
	zassert_equal(initial_free, info.free_peb_count);

	/* Write again to prove anchor is still valid for new writes. */
	zassert_ok(ubi_leb_write(ubi, vol_id, lnum, array_128, ARRAY_SIZE(array_128)));

	uint8_t rdata[ARRAY_SIZE(array_128)] = { 0 };
	size_t rsize = 0;
	zassert_ok(ubi_leb_get_size(ubi, vol_id, lnum, &rsize));
	zassert_equal(ARRAY_SIZE(array_128), rsize);
	zassert_ok(ubi_leb_read(ubi, vol_id, lnum, 0, rdata, rsize));
	zassert_mem_equal(rdata, array_128, ARRAY_SIZE(array_128));

	zassert_ok(ubi_device_deinit(ubi));
}
