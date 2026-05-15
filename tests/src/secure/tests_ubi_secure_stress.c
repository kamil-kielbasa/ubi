/**
 * \file    tests_ubi_secure_stress.c
 * \author  Kamil Kielbasa
 *
 * \brief   Stress parity tests for the secure backend.  Run only on
 *          native_sim (flash simulator); heavy flash I/O — must NOT run
 *          on real hardware to avoid premature flash wear.
 *
 * \details Mirrors a focused subset of `tests_ubi_stress.c` for the
 *          secure backend.  Validates that a long burst of write / read
 *          / unmap / erase cycles leaks no heap memory under
 *          authenticated AEAD bookkeeping and that the device's free /
 *          dirty PEB accounting returns to the steady-state baseline
 *          after every full reclamation pass.
 *
 *          The per-PEB EC oracle used by the plain wear-leveling stress
 *          test is intentionally omitted: the secure backend does not
 *          surface `ubi_device_get_peb_ec()` because the per-PEB EC
 *          counter is part of an authenticated reserved-area record
 *          that the public diagnostic API cannot peek without a hook.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>
#include <ubi_secure.h>
#include <ubi_test.h>

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

/* Standard library headers: */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

/* Conservatively below the metadata-counter rotate-NOW threshold (95% of the
 * shared reserved-PEB counter budget on native_sim).  20 cycles × 2 LEBs ×
 * (write + unmap + erase) keeps every counter domain well within budget so
 * the suite never trips a key-rotation event and remains a pure stress run. */
#define STRESS_CYCLES 20U

/* Module types and type definitiones ----------------------------------------------------------- */

/* Module interface variables and constants ----------------------------------------------------- */

/* Static variables and constants --------------------------------------------------------------- */

static struct ubi_flash_desc flash = { 0 };

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
extern struct sys_heap _system_heap;
#endif

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

ZTEST_SUITE(ubi_secure_stress, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);

/**
 * \brief 20 write-read-unmap-erase cycles in secure mode leak no heap memory.
 *
 * \details Scenario: Capture a heap snapshot via `sys_heap_runtime_stats_get()`, initialise
 *          a secure device, create a 2-LEB static volume `"stress"`, then run
 *          `STRESS_CYCLES` (20) iterations of: write both LEBs, read back and verify,
 *          unmap both, drain every dirty PEB via `ubi_device_erase_peb()`.  Deinit the
 *          device and re-snapshot the heap.  The secure backend allocates anchor / AEAD /
 *          key-derivation buffers on every mapping change; a leak there would compound
 *          across this run and surface as a `free_bytes` delta.  Cycle count is held
 *          below the metadata-counter rotate-NOW threshold so the run is a pure stress
 *          test, not a budget-exhaustion test.
 *
 * \expect Every write / read round-trip succeeds with matching data; the heap
 *         `free_bytes` after `ubi_device_deinit()` is exactly equal to the pre-init
 *         baseline, confirming no leak across all `STRESS_CYCLES` iterations.
 *
 * \oracle `mem_after.free_bytes == mem_before.free_bytes` after deinit;
 *         every per-cycle `zassert_mem_equal(rdata, wdata, 4)` for both
 *         LEB 0 and LEB 1 holds across all `STRESS_CYCLES` iterations.
 *
 * \trace Plain parity → `tests_ubi_stress`.
 *
 * \precondition `CONFIG_SYS_HEAP_RUNTIME_STATS` enabled; `STRESS_CYCLES`
 *               kept below the metadata-counter rotate-NOW threshold so
 *               the run is a stress test, not a budget exhaustion test.
 */
ZTEST(ubi_secure_stress, repeated_write_erase_cycles)
{
	struct sys_memory_stats mem_before = { 0 };
	struct sys_memory_stats mem_after = { 0 };

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &mem_before));

	struct ubi_device *ubi = ubi_test_secure_init(&flash);

	const struct ubi_volume_config cfg = {
		.name = "stress",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t wdata[] = { 0x01, 0x02, 0x03, 0x04 };
	uint8_t rdata[4];

	for (size_t cycle = 0; cycle < STRESS_CYCLES; cycle++) {
		zassert_ok(ubi_leb_write(ubi, vol_id, 0, wdata, sizeof(wdata)));
		zassert_ok(ubi_leb_write(ubi, vol_id, 1, wdata, sizeof(wdata)));

		zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
		zassert_mem_equal(rdata, wdata, sizeof(wdata));
		zassert_ok(ubi_leb_read(ubi, vol_id, 1, 0, rdata, sizeof(rdata)));
		zassert_mem_equal(rdata, wdata, sizeof(wdata));

		zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
		zassert_ok(ubi_leb_unmap(ubi, vol_id, 1));

		struct ubi_device_info info = { 0 };
		zassert_ok(ubi_device_get_info(ubi, &info));

		for (size_t d = 0; d < info.dirty_peb_count; d++) {
			zassert_ok(ubi_device_erase_peb(ubi));
		}
	}

	zassert_ok(ubi_device_deinit(ubi));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &mem_after));
	zassert_equal(mem_before.free_bytes, mem_after.free_bytes, "Memory leak detected");
}

/**
 * \brief Free / dirty PEB accounting returns to baseline after every reclamation pass.
 *
 * \details Scenario: Initialise a secure device, create a 1-LEB static volume `"acct"`
 *          and capture the post-create free-PEB count as the steady-state baseline.
 *          Then run `STRESS_CYCLES` iterations of: write LEB 0, unmap LEB 0, drain
 *          every dirty PEB.  After each cycle re-read `ubi_device_get_info()` and
 *          assert that the free-PEB count is exactly the baseline — every dirty PEB
 *          produced by the write must be reclaimed by the erase phase, and the secure
 *          backend's anchor migration must never leak a PEB into a third class
 *          (`bad`, `reserved`, etc.).
 *
 * \expect `info.free_peb_count == baseline` after every cycle; `info.dirty_peb_count
 *         == 0` after every cycle.  No accounting drift across all iterations.
 *
 * \oracle After every reclamation pass, `info.free_peb_count == baseline`
 *         and `info.dirty_peb_count == 0`; equality must hold for all
 *         `STRESS_CYCLES` iterations without drift.
 *
 * \trace PEB accounting parity; plain parity → `tests_ubi_stress`.
 *
 * \precondition 1-LEB static volume `"acct"` on a freshly formatted secure
 *               partition.
 */
ZTEST(ubi_secure_stress, peb_accounting_stable_across_cycles)
{
	struct ubi_device *ubi = ubi_test_secure_init(&flash);

	const struct ubi_volume_config cfg = {
		.name = "acct",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	struct ubi_device_info baseline = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &baseline));

	const uint8_t wdata[] = { 0xCA, 0xFE, 0xBA, 0xBE };

	for (size_t cycle = 0; cycle < STRESS_CYCLES; cycle++) {
		zassert_ok(ubi_leb_write(ubi, vol_id, 0, wdata, sizeof(wdata)));
		zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

		struct ubi_device_info info = { 0 };
		zassert_ok(ubi_device_get_info(ubi, &info));

		/* Drain until no dirty PEB remains.  Secure anchor migration can
		 * produce additional dirty PEBs while the erase loop is still
		 * running, so a fixed-count loop is insufficient. */
		while (info.dirty_peb_count > 0) {
			zassert_ok(ubi_device_erase_peb(ubi));
			memset(&info, 0, sizeof(info));
			zassert_ok(ubi_device_get_info(ubi, &info));
		}

		zassert_equal(info.dirty_peb_count, 0,
			      "Cycle %zu: dirty PEBs remain after drain (%zu)", cycle,
			      info.dirty_peb_count);
		zassert_equal(info.free_peb_count, baseline.free_peb_count,
			      "Cycle %zu: free_peb_count drift: baseline=%zu now=%zu", cycle,
			      baseline.free_peb_count, info.free_peb_count);
	}

	zassert_ok(ubi_device_deinit(ubi));
}
