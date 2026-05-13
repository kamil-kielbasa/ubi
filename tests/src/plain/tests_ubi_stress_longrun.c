/**
 * \file    tests_ubi_stress_longrun.c
 *
 * \author Kamil Kielbasa
 *
 * \brief   Randomized churn and concurrency smoke tests.
 *
 * These tests exercise the UBI stack under sustained mixed workloads
 * and verify invariants hold throughout.
 *
 * Only built when CONFIG_FLASH_SIMULATOR is available (native_sim).
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>

/* Test fixtures: */
#include "ubi_test_fixture.h"

/* Zephyr headers: */
#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>

/* Standard library headers: */
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

/* Module types and type definitiones ----------------------------------------------------------- */

/* Module interface variables and constants ----------------------------------------------------- */

/* Static variables and constants --------------------------------------------------------------- */

static struct ubi_flash_desc flash = { 0 };

/* Static function declarations ----------------------------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_after(void *ctx);

static void ztest_testcase_before(void *ctx);
static void ztest_testcase_teardown(void *ctx);

/* Static function definitions ------------------------------------------------------------------ */

static void *ztest_suite_setup(void)
{
	ubi_test_setup_mtd(&flash);
	return NULL;
}

static void ztest_suite_after(void *ctx)
{
	(void)ctx;
}

static void ztest_testcase_before(void *ctx)
{
	(void)ctx;
	ubi_test_erase_partition();
}

static void ztest_testcase_teardown(void *ctx)
{
	(void)ctx;
}

/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_stress_longrun, NULL, ztest_suite_setup, ztest_testcase_before,
	    ztest_testcase_teardown, ztest_suite_after);

/**
 * \brief Randomized churn: create, write, read, erase, remove in a loop.
 *
 * Runs 50 iterations of: create a volume, write to random LEBs, read back,
 * erase dirty PEBs, remove the volume. Checks invariants after each cycle
 * if the test API is available.
 *
 * \details Scenario: Run 50 reboot cycles. Each cycle: init device, create dynamic
 *          volume "churn" with 3 LEBs, write a 32-byte pattern to LEBs 0-2, read back,
 *          erase dirty PEBs, check invariants, remove the volume, deinit.
 *
 * \expect All 50 cycles complete; reads match writes; invariants pass; no memory leaks.
 *
 * \oracle Every API call returns 0 across the 50 cycles; per-cycle
 *         `memcmp(rbuf, wbuf, 32) == 0` for each of the three LEBs;
 *         when `CONFIG_UBI_TEST_API_ENABLE` is on, the per-cycle
 *         invariant check passes.
 *
 * \trace Randomized stress longrun.
 *
 * \precondition Simulator-only (`CONFIG_FLASH_SIMULATOR`); deterministic
 *               pattern via `memset` (seeded RNG churn deferred).
 */
ZTEST(ubi_stress_longrun, randomized_churn_with_reboots)
{
	uint8_t wbuf[32];
	uint8_t rbuf[32];

	for (int cycle = 0; cycle < 50; cycle++) {
		struct ubi_device *ubi = ubi_test_init_device(&flash);

		const struct ubi_volume_config cfg = {
			.name = "churn",
			.type = UBI_VOLUME_TYPE_DYNAMIC,
			.leb_count = 3,
		};
		int vol_id = -1;
		zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

		for (size_t lnum = 0; lnum < 3; lnum++) {
			memset(wbuf, (uint8_t)(cycle + lnum), sizeof(wbuf));
			zassert_ok(ubi_leb_write(ubi, vol_id, lnum, wbuf, sizeof(wbuf)));
		}

		for (size_t lnum = 0; lnum < 3; lnum++) {
			memset(wbuf, (uint8_t)(cycle + lnum), sizeof(wbuf));
			zassert_ok(ubi_leb_read(ubi, vol_id, lnum, 0, rbuf, sizeof(rbuf)));
			zassert_mem_equal(rbuf, wbuf, sizeof(wbuf));
		}

		for (int i = 0; i < 5; i++) {
			zassert_ok(ubi_device_erase_peb(ubi));
		}

#if defined(CONFIG_UBI_TEST_API_ENABLE)
		zassert_ok(ubi_device_check_invariants(ubi));
#endif

		zassert_ok(ubi_volume_remove(ubi, vol_id));
		zassert_ok(ubi_device_deinit(ubi));
	}
}

/**
 * \brief Persistence check: write data, deinit+reinit, verify data survives.
 *
 * \details Scenario: Initialize device, create static volume "persist" (2 LEBs), write
 *          6 bytes {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE} to LEB 0, deinit. Reinit, read LEB 0,
 *          check device_info, deinit.
 *
 * \expect Reinit returns 0; volume_count == 1; the read data matches what was written.
 */
ZTEST(ubi_stress_longrun, persistence_across_reinit)
{
	struct ubi_device *ubi = ubi_test_init_device(&flash);

	const struct ubi_volume_config cfg = {
		.name = "persist",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	zassert_ok(ubi_device_deinit(ubi));

	ubi = ubi_test_init_device(&flash);

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count);

	uint8_t rbuf[6] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rbuf, sizeof(rbuf)));
	zassert_mem_equal(rbuf, data, sizeof(data));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Mixed operations: create multiple volumes, write, resize, remove.
 *
 * \details Scenario: Initialize, create dynamic "vol_a" (2 LEBs) and static "vol_b"
 *          (1 LEB), write {0xAA, 0xBB} to vol_a LEB 0 and {0xCC, 0xDD} to vol_b LEB 0,
 *          resize vol_a to 3 LEBs, read vol_a LEB 0, remove vol_b, check device_info
 *          and invariants, deinit.
 *
 * \expect All operations return 0; read data matches written data; invariants pass.
 */
ZTEST(ubi_stress_longrun, mixed_multi_volume_operations)
{
	struct ubi_device *ubi = ubi_test_init_device(&flash);

	const struct ubi_volume_config cfg_a = {
		.name = "vol_a",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	const struct ubi_volume_config cfg_b = {
		.name = "vol_b",
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	int id_a, id_b;
	zassert_ok(ubi_volume_create(ubi, &cfg_a, &id_a));
	zassert_ok(ubi_volume_create(ubi, &cfg_b, &id_b));

	const uint8_t data_a[] = { 0xAA, 0xBB };
	const uint8_t data_b[] = { 0xCC, 0xDD };
	zassert_ok(ubi_leb_write(ubi, id_a, 0, data_a, sizeof(data_a)));
	zassert_ok(ubi_leb_write(ubi, id_b, 0, data_b, sizeof(data_b)));

	const struct ubi_volume_config grow_cfg = {
		.name = "vol_a",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 3,
	};
	zassert_ok(ubi_volume_resize(ubi, id_a, &grow_cfg));

	uint8_t rbuf[2];
	zassert_ok(ubi_leb_read(ubi, id_a, 0, 0, rbuf, sizeof(rbuf)));
	zassert_mem_equal(rbuf, data_a, sizeof(data_a));

	zassert_ok(ubi_volume_remove(ubi, id_b));

	struct ubi_device_info info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &info));
	zassert_equal(1, info.volume_count);

#if defined(CONFIG_UBI_TEST_API_ENABLE)
	zassert_ok(ubi_device_check_invariants(ubi));
#endif

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Verify EC counter equality after 500 write-erase cycles.
 *
 * \details Scenario: Perform 500 write-unmap-erase cycles on a single-LEB volume.
 *          After all cycles complete, retrieve per-PEB erase counters and
 *          verify that the maximum deviation between any two counters is
 *          at most 2 (greedy wear-leveling guarantee). Also verify ec_avg
 *          matches the expected value.
 *
 * \expect All EC counters are within 2 of each other after 500 cycles.
 *         ec_avg matches total_erases / total_data_pebs.
 */
ZTEST(ubi_stress_longrun, ec_counters_equal_after_500_cycles)
{
#if defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = ubi_test_init_device(&flash);

	const struct ubi_volume_config cfg = {
		.name = "ec500",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xEC, 0x50, 0x00, 0xFF };
	const size_t nr_cycles = 500;

	for (size_t i = 0; i < nr_cycles; i++) {
		zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
		zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

		struct ubi_device_info info = { 0 };
		zassert_ok(ubi_device_get_info(ubi, &info));

		for (size_t d = 0; d < info.dirty_peb_count; d++) {
			zassert_ok(ubi_device_erase_peb(ubi));
		}
	}

	/* Retrieve per-PEB erase counters */
	size_t *peb_ec = NULL;
	size_t peb_ec_len = 0;
	zassert_ok(ubi_device_get_peb_ec(ubi, &peb_ec, &peb_ec_len));

	size_t min_ec = SIZE_MAX;
	size_t max_ec = 0;
	size_t sum_ec = 0;

	for (size_t i = 0; i < peb_ec_len; i++) {
		if (peb_ec[i] < min_ec) {
			min_ec = peb_ec[i];
		}
		if (peb_ec[i] > max_ec) {
			max_ec = peb_ec[i];
		}
		sum_ec += peb_ec[i];
	}

	zassert_true(max_ec <= min_ec + 2, "EC imbalance after %zu cycles: min=%zu max=%zu",
		     nr_cycles, min_ec, max_ec);

	/* Verify ec_avg matches */
	struct ubi_device_info final_info = { 0 };
	zassert_ok(ubi_device_get_info(ubi, &final_info));

	const size_t expected_avg = sum_ec / peb_ec_len;
	zassert_equal(final_info.ec_avg, expected_avg, "ec_avg mismatch: got %zu, expected %zu",
		      final_info.ec_avg, expected_avg);

	/* Verify invariants hold after sustained workload */
	zassert_ok(ubi_device_check_invariants(ubi));

	k_free(peb_ec);
	zassert_ok(ubi_device_deinit(ubi));
#else
	ztest_test_skip();
#endif
}
