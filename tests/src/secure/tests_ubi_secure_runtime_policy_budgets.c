/**
 * \file    tests_ubi_secure_runtime_policy_budgets.c
 * \author  Kamil Kielbasa
 *
 * \brief   Tests for secure runtime policy: sticky read-only, event callbacks,
 *          freshness sync, AUTH_FAILURE events, budget thresholds, refcount
 *          KEY_RETIRABLE, allowlist reject, missing-key write, rollback policy
 *          mismatch, and mixed-key rotation.
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
#include "ubi_test_secure_fixture.h"

/* Zephyr headers: */
#include <psa/crypto.h>
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>

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
static struct ubi_device *g_ubi;

/** Grouped test state — zeroed by memset in suite before(). */
struct runtime_policy_test_state {
	/** Number of events received by the callback. */
	size_t event_count;
	/** Type of the most recently received event. */
	enum ubi_crypto_event_type last_event_type;
	/** Number of times sync_freshness was called. */
	size_t sync_call_count;
	/** Return code that counting_sync_freshness uses (0 = success). */
	int sync_return_code;
	/** If > 0, counting_sync_freshness succeeds for the first N calls then fails. */
	size_t sync_fail_after;
	/** Number of FRESHNESS_SYNC_FAILURE events received. */
	size_t freshness_sync_failure_count;
	/** Number of AUTH_FAILURE events received. */
	size_t auth_failure_count;
	/** Number of KEY_ROTATE_SOON events received. */
	size_t rotate_soon_count;
	/** Number of KEY_ROTATE_NOW events received. */
	size_t rotate_now_count;
	/** Number of KEY_RETIRABLE events received. */
	size_t key_retirable_count;
	/** Key version from the most recent KEY_RETIRABLE event. */
	uint8_t key_retirable_kv;
	/** Number of KEY_VERSION_NOT_ALLOWLISTED events received. */
	size_t allowlist_reject_count;
	/** Number of KEY_VERSION_UNAVAILABLE events received. */
	size_t key_unavailable_count;
	/** Number of ROLLBACK_POLICY_MISMATCH events received. */
	size_t rollback_mismatch_count;
	/** Key version for which selective_get_key_id should fail (0 = no failure). */
	uint8_t fail_key_version;
};

static struct runtime_policy_test_state ts;

/* Static function definitions ------------------------------------------------------------------ */

/**
 * \brief Comprehensive event tracker — returns CONTINUE.
 */
static enum ubi_crypto_event_verdict tracking_event_cb(const struct ubi_crypto_event *event,
						       void *user_data)
{
	(void)user_data;
	ts.event_count++;
	ts.last_event_type = event->type;

	switch (event->type) {
	case UBI_CRYPTO_EVENT_KEY_ROTATE_SOON:
		ts.rotate_soon_count++;
		break;
	case UBI_CRYPTO_EVENT_KEY_ROTATE_NOW:
		ts.rotate_now_count++;
		break;
	case UBI_CRYPTO_EVENT_KEY_RETIRABLE:
		ts.key_retirable_count++;
		ts.key_retirable_kv = event->rotation.key_version;
		break;
	case UBI_CRYPTO_EVENT_KEY_VERSION_NOT_ALLOWLISTED:
		ts.allowlist_reject_count++;
		break;
	case UBI_CRYPTO_EVENT_KEY_VERSION_UNAVAILABLE:
		ts.key_unavailable_count++;
		break;
	case UBI_CRYPTO_EVENT_ROLLBACK_POLICY_MISMATCH:
		ts.rollback_mismatch_count++;
		break;
	case UBI_CRYPTO_EVENT_FRESHNESS_SYNC_FAILURE:
		ts.freshness_sync_failure_count++;
		break;
	case UBI_CRYPTO_EVENT_AUTH_FAILURE:
		ts.auth_failure_count++;
		break;
	default:
		break;
	}

	return UBI_CRYPTO_EVENT_CONTINUE;
}

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
	memset(&ts, 0, sizeof(ts));
}

static void ztest_suite_after(void *ctx)
{
	(void)ctx;
	if (g_ubi) {
		ubi_device_deinit(g_ubi);
		g_ubi = NULL;
	}
}

/* Module interface function definitions -------------------------------------------------------- */

#define BUDGET_NOW_THRESHOLD                                                                      \
	((uint64_t)CONFIG_UBI_CRYPTO_METADATA_COUNTER_BUDGET * CONFIG_UBI_CRYPTO_ROTATE_NOW_PCT / \
	 100)
#define BUDGET_SOON_THRESHOLD                                                                      \
	((uint64_t)CONFIG_UBI_CRYPTO_METADATA_COUNTER_BUDGET * CONFIG_UBI_CRYPTO_ROTATE_SOON_PCT / \
	 100)
/* Headroom — how far below NOW the counter starts.  Must be small enough
 * that the hard threshold is reached within the few free PEBs available
 * on native_sim, yet leave room for at least one successful operation
 * before the rejection.                                                  */
#define BUDGET_HEADROOM 5

#define LEB_BUDGET_NOW_THRESHOLD \
	((size_t)CONFIG_UBI_CRYPTO_LEB_WRITE_BUDGET * CONFIG_UBI_CRYPTO_ROTATE_NOW_PCT / 100)
#define LEB_BUDGET_SOON_THRESHOLD \
	((size_t)CONFIG_UBI_CRYPTO_LEB_WRITE_BUDGET * CONFIG_UBI_CRYPTO_ROTATE_SOON_PCT / 100)

ZTEST_SUITE(ubi_secure_runtime_policy_budgets, NULL, ztest_suite_setup, ztest_suite_before,
	    ztest_suite_after, NULL);

/**
 * \brief Reserved-area (DEVICE_HEADER + VOLUME_HEADER) write-budget exhaustion.
 *
 * \details Scenario: Pre-stages the shared reserved-PEB AEAD counter just below the
 *          hard threshold via the test hook, then issues volume_resize
 *          calls.  Each commit advances the counter by 1 + vol_count, so
 *          a handful of resizes crosses ROTATE_NOW_PCT.  The pre-commit
 *          budget must reject with -ENOSPC, emit KEY_ROTATE_NOW and put
 *          the device in sticky read-only state — every subsequent
 *          mutation class returns -EROFS, reads still succeed.
 *          Reattach with a new requested_write_key_version installs a
 *          fresh budget under new HKDF child keys and unblocks volume ops.
 *
 * \expect The last resize returns -ENOSPC; rotate_now_count == 1; subsequent writes/erases
 *         return -EROFS; reads still succeed. Reinit with kv=2 allows the resize to succeed.
 */
ZTEST(ubi_secure_runtime_policy_budgets, reserved_metadata_budget_exhausts_blocks_until_rotation)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	cfg.event_cb = tracking_event_cb;

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};

	int vol_id = -1;

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	const uint8_t persisted[] = { 0xCA, 0xFE };

	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, persisted, sizeof(persisted)));

	/* Drive the reserved-PEB counter close to the hard threshold; leave
	 * EC and VID at zero so only the reserved-area domain can trip. */
	ubi_secure_test_set_metadata_counters(g_ubi, BUDGET_NOW_THRESHOLD - BUDGET_HEADROOM, 0, 0);

	/* Each commit advances next_res_peb_counter by 1 + vol_count
	 * (vol_count == 1 here ⇒ +2 per resize).  Alternate leb_count so
	 * every call is a real resize. */
	struct ubi_volume_config grown = vol_cfg;
	int last_ret = 0;

	for (size_t i = 0; i < BUDGET_HEADROOM + 5; i++) {
		grown.leb_count = (i & 1) ? 2 : 3;

		last_ret = ubi_volume_resize(g_ubi, vol_id, &grown);
		if (last_ret != 0) {
			break;
		}
	}

	zassert_equal(last_ret, -ENOSPC, "Expected -ENOSPC, got %d", last_ret);
	zassert_equal(ts.rotate_now_count, 1, "Expected exactly one KEY_ROTATE_NOW event, got %zu",
		      ts.rotate_now_count);

	const uint8_t wdata[] = { 0xDE, 0xAD };

	zassert_equal(ubi_leb_write(g_ubi, vol_id, 0, wdata, sizeof(wdata)), -EROFS,
		      "Writes must be blocked after metadata exhaustion");
	zassert_equal(ubi_device_erase_peb(g_ubi), -EROFS,
		      "Erase must be blocked after metadata exhaustion");

	struct ubi_volume_config bigger = grown;
	bigger.leb_count = grown.leb_count + 1;
	zassert_equal(ubi_volume_resize(g_ubi, vol_id, &bigger), -EROFS,
		      "Volume ops must be blocked after metadata exhaustion");

	uint8_t rdata[sizeof(persisted)] = { 0 };
	zassert_ok(ubi_leb_read(g_ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, persisted, sizeof(persisted));

	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;

	static const uint8_t allowed_v12[] = { 1, 2 };

	cfg.policy.requested_write_key_version = 2;
	cfg.policy.allowed_key_versions = allowed_v12;
	cfg.policy.allowed_key_versions_len = 2;

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

	struct ubi_volume_config post_rotation = grown;
	post_rotation.leb_count = 5;
	zassert_ok(ubi_volume_resize(g_ubi, vol_id, &post_rotation));
}

/**
 * \brief ERASE_COUNTER write-budget exhaustion.
 *
 * \details Scenario: Pre-stages BUDGET_HEADROOM + 1 dirty PEBs by writing distinct
 *          LEBs and shrinking the volume (only the VID and reserved
 *          counters move during pre-staging).  The test hook then drives
 *          the EC counter close to the hard threshold while VID and
 *          reserved are reset to zero — guaranteeing that the budget that
 *          eventually trips is the EC budget, not VID and not the
 *          reserved-area budget.  A pure-erase loop runs until -ENOSPC.
 *          KEY_ROTATE_NOW is emitted exactly once, subsequent mutations
 *          are -EROFS, and reattach with a new kv unblocks erase.
 *
 * \expect Successful erases are limited to 4; the final erase returns -ENOSPC;
 *         rotate_now_count == 1; subsequent mutations return -EROFS. Reinit with kv=2
 *         allows erase to succeed.
 */
ZTEST(ubi_secure_runtime_policy_budgets, ec_metadata_budget_exhausts_blocks_until_rotation)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	cfg.event_cb = tracking_event_cb;

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
	zassert_ok(ubi_device_get_info(g_ubi, &info));

	/* Need BUDGET_HEADROOM + 1 dirty PEBs (one extra to cover the
	 * single rejected erase attempt that triggers exhaustion). */
	const size_t needed_dirty = BUDGET_HEADROOM + 1;

	zassert_true(info.free_peb_count >= needed_dirty,
		     "Test geometry too small: free_peb_count=%zu, needed=%zu", info.free_peb_count,
		     needed_dirty);

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = needed_dirty,
	};

	int vol_id = -1;

	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	/* Pre-stage: write to each LEB once, then shrink so they all become
	 * dirty.  EC counter stays at zero (no erase yet). */
	const uint8_t pad = 0x55;

	for (size_t i = 0; i < needed_dirty; i++) {
		zassert_ok(ubi_leb_write(g_ubi, vol_id, i, &pad, sizeof(pad)));
	}

	struct ubi_volume_config shrunk = vol_cfg;
	shrunk.leb_count = 1;
	zassert_ok(ubi_volume_resize(g_ubi, vol_id, &shrunk));

	/* Drive the EC counter close to the hard threshold; reset reserved
	 * and VID so they cannot trip first. */
	ubi_secure_test_set_metadata_counters(g_ubi, 0, BUDGET_NOW_THRESHOLD - BUDGET_HEADROOM, 0);

	int last_ret = 0;
	size_t erases_done = 0;

	for (size_t i = 0; i < needed_dirty; i++) {
		last_ret = ubi_device_erase_peb(g_ubi);
		if (last_ret != 0) {
			break;
		}
		erases_done++;
	}

	zassert_equal(last_ret, -ENOSPC, "Expected -ENOSPC from erase, got %d", last_ret);
	zassert_equal(erases_done, BUDGET_HEADROOM - 1,
		      "Expected exactly %u successful erases before exhaustion, got %zu",
		      (unsigned)(BUDGET_HEADROOM - 1), erases_done);
	zassert_equal(ts.rotate_now_count, 1, "Expected exactly one KEY_ROTATE_NOW, got %zu",
		      ts.rotate_now_count);

	zassert_equal(ubi_device_erase_peb(g_ubi), -EROFS,
		      "Erase must be blocked after EC metadata exhaustion");
	zassert_equal(ubi_leb_write(g_ubi, vol_id, 0, &pad, sizeof(pad)), -EROFS,
		      "Writes must be blocked after EC metadata exhaustion");

	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;

	static const uint8_t allowed_v12[] = { 1, 2 };

	cfg.policy.requested_write_key_version = 2;
	cfg.policy.allowed_key_versions = allowed_v12;
	cfg.policy.allowed_key_versions_len = 2;

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
	zassert_ok(ubi_device_erase_peb(g_ubi));
}

/**
 * \brief VOLUME_IDENTIFIER write-budget exhaustion.
 *
 * \details Scenario: Pre-stages the VID counter close to the hard threshold (and
 *          resets reserved + EC to zero so they cannot trip first), then
 *          writes to distinct LEBs.  Each write advances the global VID
 *          counter by one and the per-{kv, vol_id} LEB counter by one
 *          (well below the LEB-domain budget).  After BUDGET_HEADROOM
 *          successful writes the VID pre-check rejects the call with
 *          -ENOSPC and KEY_ROTATE_NOW is emitted.
 *
 * \expect Successful writes are limited to 4; the final write returns -ENOSPC;
 *         rotate_now_count == 1; subsequent mutations return -EROFS. Reinit with kv=2
 *         allows write to succeed.
 */
ZTEST(ubi_secure_runtime_policy_budgets, vid_metadata_budget_exhausts_blocks_until_rotation)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	cfg.event_cb = tracking_event_cb;

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
	zassert_ok(ubi_device_get_info(g_ubi, &info));

	const size_t needed_lebs = BUDGET_HEADROOM + 1;

	zassert_true(info.free_peb_count >= needed_lebs,
		     "Test geometry too small: free_peb_count=%zu, needed=%zu", info.free_peb_count,
		     needed_lebs);

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = needed_lebs,
	};

	int vol_id = -1;

	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	ubi_secure_test_set_metadata_counters(g_ubi, 0, 0, BUDGET_NOW_THRESHOLD - BUDGET_HEADROOM);

	const uint8_t pad = 0x77;
	int last_ret = 0;
	size_t writes_done = 0;

	for (size_t i = 0; i < needed_lebs; i++) {
		last_ret = ubi_leb_write(g_ubi, vol_id, i, &pad, sizeof(pad));
		if (last_ret != 0) {
			break;
		}
		writes_done++;
	}

	zassert_equal(last_ret, -ENOSPC, "Expected -ENOSPC from leb_write, got %d", last_ret);
	zassert_equal(writes_done, BUDGET_HEADROOM - 1,
		      "Expected exactly %u successful writes before VID exhaustion, got %zu",
		      (unsigned)(BUDGET_HEADROOM - 1), writes_done);
	zassert_equal(ts.rotate_now_count, 1, "Expected exactly one KEY_ROTATE_NOW, got %zu",
		      ts.rotate_now_count);

	zassert_equal(ubi_leb_write(g_ubi, vol_id, 0, &pad, sizeof(pad)), -EROFS,
		      "Writes must be blocked after VID exhaustion");
	zassert_equal(ubi_device_erase_peb(g_ubi), -EROFS,
		      "Erase must be blocked after VID exhaustion");

	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;

	static const uint8_t allowed_v12[] = { 1, 2 };

	cfg.policy.requested_write_key_version = 2;
	cfg.policy.allowed_key_versions = allowed_v12;
	cfg.policy.allowed_key_versions_len = 2;

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, &pad, sizeof(pad)));
}

/**
 * \brief Metadata-domain KEY_ROTATE_SOON fires before NOW.
 *
 * \details Scenario: Pre-stages the VID counter just below ROTATE_SOON_PCT, then
 *          performs leb_write calls until SOON is observed.  The pre-check
 *          must pass (usage < NOW_PCT), the post-write check must emit
 *          KEY_ROTATE_SOON and the device must keep accepting operations
 *          (no KEY_ROTATE_NOW, no sticky RO).
 *
 * \expect rotate_soon_count >= 1 before reaching the hard threshold; rotate_now_count == 0;
 *         operations keep succeeding.
 */
ZTEST(ubi_secure_runtime_policy_budgets, metadata_rotate_soon_emitted_below_now)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	cfg.event_cb = tracking_event_cb;

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
	zassert_ok(ubi_device_get_info(g_ubi, &info));

	const size_t needed_lebs = BUDGET_HEADROOM + 1;

	zassert_true(info.free_peb_count >= needed_lebs,
		     "Test geometry too small: free_peb_count=%zu, needed=%zu", info.free_peb_count,
		     needed_lebs);

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = needed_lebs,
	};

	int vol_id = -1;

	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	/* Start one tick below SOON.  The first write will fire SOON; we
	 * never approach NOW because the loop stops at SOON. */
	ubi_secure_test_set_metadata_counters(g_ubi, 0, 0, BUDGET_SOON_THRESHOLD - 1);

	const uint8_t pad = 0xA5;

	for (size_t i = 0; i < needed_lebs; i++) {
		zassert_ok(ubi_leb_write(g_ubi, vol_id, i, &pad, sizeof(pad)));
		if (ts.rotate_soon_count >= 1) {
			break;
		}
	}

	zassert_true(ts.rotate_soon_count >= 1, "Expected at least one KEY_ROTATE_SOON event");
	zassert_equal(ts.rotate_now_count, 0,
		      "KEY_ROTATE_NOW must not fire below hard threshold (got %zu)",
		      ts.rotate_now_count);
}

/**
 * \brief Metadata budget bases reset on key-version rotation.
 *
 * \details Scenario: Exhausts the VID budget under kv=1 (proxy for any metadata
 *          domain — all four use the same Kconfig limits), reattaches with
 *          kv=2 and confirms a VID-domain write that previously hit
 *          -ENOSPC now succeeds.  No new KEY_ROTATE_NOW must fire under
 *          the rotated kv.
 *
 * \expect VID exhaustion under kv=1 causes -ENOSPC. After reattach with kv=2 the same
 *         write succeeds; rotate_now_count is unchanged (no new events under kv=2).
 */
ZTEST(ubi_secure_runtime_policy_budgets, metadata_budget_resets_on_key_rotation_reattach)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	cfg.event_cb = tracking_event_cb;

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
	zassert_ok(ubi_device_get_info(g_ubi, &info));

	const size_t needed_lebs = BUDGET_HEADROOM + 1;

	zassert_true(info.free_peb_count >= needed_lebs,
		     "Test geometry too small: free_peb_count=%zu, needed=%zu", info.free_peb_count,
		     needed_lebs);

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = needed_lebs,
	};

	int vol_id = -1;

	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	ubi_secure_test_set_metadata_counters(g_ubi, 0, 0, BUDGET_NOW_THRESHOLD - BUDGET_HEADROOM);

	const uint8_t pad = 0xC3;
	int last_ret = 0;

	for (size_t i = 0; i < needed_lebs; i++) {
		last_ret = ubi_leb_write(g_ubi, vol_id, i, &pad, sizeof(pad));
		if (last_ret != 0) {
			break;
		}
	}

	zassert_equal(last_ret, -ENOSPC, "Expected -ENOSPC pre-rotation, got %d", last_ret);
	zassert_equal(ts.rotate_now_count, 1, "Expected exactly one KEY_ROTATE_NOW, got %zu",
		      ts.rotate_now_count);

	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;

	const size_t now_count_before_reattach = ts.rotate_now_count;

	static const uint8_t allowed_v12[] = { 1, 2 };

	cfg.policy.requested_write_key_version = 2;
	cfg.policy.allowed_key_versions = allowed_v12;
	cfg.policy.allowed_key_versions_len = 2;

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

	/* Overwriting an existing LEB consumes a free PEB (and dirties the
	 * old one); after rotation the fresh budget allows it. */
	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, &pad, sizeof(pad)));

	zassert_equal(ts.rotate_now_count, now_count_before_reattach,
		      "KEY_ROTATE_NOW must not refire under fresh kv (got %zu, expected %zu)",
		      ts.rotate_now_count, now_count_before_reattach);
}

/**
 * \brief LEB-domain KEY_ROTATE_SOON fires before NOW.
 *
 * \details Scenario: Repeatedly write+erase the same LEB; reset the metadata
 *          counters every iteration so only the LEB per-{kv, vol_id, lnum}
 *          counter advances.  Stop the loop as soon as KEY_ROTATE_SOON
 *          is observed.  The post-write check must emit SOON before NOW;
 *          operations must keep succeeding.
 *
 * \expect rotate_soon_count >= 1 once write/erase cycles approach the SOON threshold;
 *         rotate_now_count == 0; operations continue succeeding.
 */
ZTEST(ubi_secure_runtime_policy_budgets, leb_budget_rotate_soon_emitted_below_now)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	cfg.event_cb = tracking_event_cb;

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	int vol_id = -1;

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	const uint8_t wdata[] = { 0xAA, 0xBB, 0xCC, 0xDD };

	for (size_t i = 0; i < LEB_BUDGET_NOW_THRESHOLD; i++) {
		ubi_secure_test_set_metadata_counters(g_ubi, 0, 0, 0);

		zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata, sizeof(wdata)));
		(void)ubi_device_erase_peb(g_ubi);

		if (ts.rotate_soon_count >= 1) {
			break;
		}
	}

	zassert_true(ts.rotate_soon_count >= 1, "Expected at least one KEY_ROTATE_SOON event");
	zassert_equal(ts.rotate_now_count, 0,
		      "KEY_ROTATE_NOW must not fire below hard threshold (got %zu)",
		      ts.rotate_now_count);
}

/**
 * \brief LEB-domain write-budget exhaustion.
 *
 * \details Scenario: Repeatedly write+erase the same LEB while resetting the
 *          metadata counters every iteration so only the LEB per-{kv,
 *          vol_id, lnum} counter advances.  After ROTATE_NOW_PCT writes
 *          the LEB pre-check rejects the next write with -ENOSPC,
 *          KEY_ROTATE_NOW is emitted and sticky read-only blocks all
 *          subsequent mutation classes.
 *
 * \expect After the LEB budget reaches 100 % the final write returns -ENOSPC;
 *         rotate_now_count == 1; subsequent mutations return -EROFS.
 */
ZTEST(ubi_secure_runtime_policy_budgets, leb_budget_exhausts_blocks_until_rotation)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	cfg.event_cb = tracking_event_cb;

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	int vol_id = -1;

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	const uint8_t wdata[] = { 0x11, 0x22, 0x33, 0x44 };
	int last_ret = 0;

	for (size_t i = 0; i <= LEB_BUDGET_NOW_THRESHOLD; i++) {
		ubi_secure_test_set_metadata_counters(g_ubi, 0, 0, 0);

		last_ret = ubi_leb_write(g_ubi, vol_id, 0, wdata, sizeof(wdata));
		if (last_ret != 0) {
			break;
		}
		(void)ubi_device_erase_peb(g_ubi);
	}

	zassert_equal(last_ret, -ENOSPC, "Expected -ENOSPC from leb_write, got %d", last_ret);
	zassert_equal(ts.rotate_now_count, 1, "Expected exactly one KEY_ROTATE_NOW, got %zu",
		      ts.rotate_now_count);

	zassert_equal(ubi_leb_write(g_ubi, vol_id, 0, wdata, sizeof(wdata)), -EROFS,
		      "Writes must be blocked after LEB exhaustion");
	zassert_equal(ubi_device_erase_peb(g_ubi), -EROFS,
		      "Erase must be blocked after LEB exhaustion");
}

/**
 * \brief vid_next_counter_floor is reset to 0 when write-active kv advances.
 *
 * \details Scenario: Initialize with default kv=1, set the in-RAM VID counter to 100,
 *          create a volume so the high watermark is snapshotted into reserved metadata,
 *          deinit. Reinit with kv=2 and allowlist=[1,2]. Inspect the metadata counters.
 *
 * \expect After rotation next_vid_counter == 0 (the counter is reset on kv advance).
 */
ZTEST(ubi_secure_runtime_policy_budgets, vid_floor_resets_on_rotation)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

	/* Drive the in-RAM VID counter high before any reserved commit.
	 * The next reserved commit will snapshot this into the on-flash
	 * vid_next_counter_floor. */
	const uint64_t high_floor = 100;

	ubi_secure_test_set_metadata_counters(g_ubi, 0, 0, high_floor);

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	int vol_id = -1;

	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	uint64_t vid_after_create = 0;

	ubi_secure_test_get_metadata_counters(g_ubi, NULL, NULL, &vid_after_create);
	zassert_true(vid_after_create > high_floor,
		     "Pre-rotation: counter must have advanced past the high floor (got %llu)",
		     (unsigned long long)vid_after_create);

	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;

	static const uint8_t allowed_v12[] = { 1, 2 };

	cfg.policy.requested_write_key_version = 2;
	cfg.policy.allowed_key_versions = allowed_v12;
	cfg.policy.allowed_key_versions_len = 2;

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

	uint64_t vid_after_rotate = UINT64_MAX;

	ubi_secure_test_get_metadata_counters(g_ubi, NULL, NULL, &vid_after_rotate);
	zassert_equal(vid_after_rotate, 0,
		      "Post-rotation: next_vid_counter must reset to 0 under fresh kv (got %llu)",
		      (unsigned long long)vid_after_rotate);
}

/**
 * \brief vid_next_counter_floor is monotonic within a single write-active kv.
 *
 * Reattaching with the same kv must NOT reset the counter — the spec only
 * permits reset when the write-active key version advances.
 *
 * \details Scenario: Initialize with default kv=1, set the in-RAM VID counter to 50,
 *          create a volume so the watermark is snapshotted, deinit. Reinit with the same
 *          kv=1 and inspect the metadata counters.
 *
 * \expect After reattach next_vid_counter >= 50 (counter persists under the same kv).
 */
ZTEST(ubi_secure_runtime_policy_budgets, vid_floor_persists_within_same_kv)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

	const uint64_t high_floor = 50;

	ubi_secure_test_set_metadata_counters(g_ubi, 0, 0, high_floor);

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	int vol_id = -1;

	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

	uint64_t vid_after_reattach = 0;

	ubi_secure_test_get_metadata_counters(g_ubi, NULL, NULL, &vid_after_reattach);
	zassert_true(vid_after_reattach >= high_floor,
		     "Same-kv reattach must preserve monotonic floor (got %llu, expected >= %llu)",
		     (unsigned long long)vid_after_reattach, (unsigned long long)high_floor);
}

/**
 * \brief First VID write after rotation consumes a low counter value.
 *
 * Confirms that on-flash records under the new kv start at counter 0 — i.e.
 * the reset actually impacts subsequent writes (not just the in-RAM field).
 *
 * \details Scenario: Initialize with kv=1, set the in-RAM VID counter to 200, create
 *          a volume, deinit. Reinit with kv=2 and allowlist=[1,2]. Write to LEB 0 and
 *          retrieve the metadata counters. Deinit.
 *
 * \expect next_vid_counter == 1 (the first write under kv=2 consumes counter value 0).
 */
ZTEST(ubi_secure_runtime_policy_budgets, vid_floor_reset_writes_use_low_counters)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

	const uint64_t high_floor = 200;

	ubi_secure_test_set_metadata_counters(g_ubi, 0, 0, high_floor);

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	int vol_id = -1;

	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;

	static const uint8_t allowed_v12[] = { 1, 2 };

	cfg.policy.requested_write_key_version = 2;
	cfg.policy.allowed_key_versions = allowed_v12;
	cfg.policy.allowed_key_versions_len = 2;

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

	const uint8_t wdata[] = { 0xDE, 0xAD, 0xBE, 0xEF };

	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata, sizeof(wdata)));

	uint64_t vid_after_write = UINT64_MAX;

	ubi_secure_test_get_metadata_counters(g_ubi, NULL, NULL, &vid_after_write);
	zassert_equal(vid_after_write, 1,
		      "Post-rotation write must consume VID counter 0 (next == 1, got %llu)",
		      (unsigned long long)vid_after_write);
}

/**
 * \brief volume_create / volume_remove must not emit KEY_RETIRABLE for the
 *        still-active write key version.
 *
 * \details Scenario: Format with kv=1, then exercise the reserved metadata commit
 *          path through create + remove cycles without changing
 *          requested_write_key_version.  The reserved-PEB refcount under
 *          kv=1 must remain > 0 throughout (DEV header on every reserved
 *          PEB never goes away while the device is alive), so no
 *          KEY_RETIRABLE event for kv=1 may be emitted.  A regression in
 *          the inc-first / dec-last ordering would surface here as a
 *          spurious KEY_RETIRABLE during the dec step of the create or
 *          remove transition.
 *
 * \expect create returns 0 and key_retirable_count == 0; remove returns 0 and
 *         key_retirable_count == 0 (no spurious retirable event for the active kv).
 */
ZTEST(ubi_secure_runtime_policy_budgets, reserved_refcount_no_spurious_key_retirable)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	cfg.event_cb = tracking_event_cb;

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	int vol_id = -1;

	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));
	zassert_equal(ts.key_retirable_count, 0,
		      "volume_create must not emit KEY_RETIRABLE for active kv (got %zu)",
		      ts.key_retirable_count);

	zassert_ok(ubi_volume_remove(g_ubi, vol_id));
	zassert_equal(ts.key_retirable_count, 0,
		      "volume_remove must not emit KEY_RETIRABLE for active kv (got %zu)",
		      ts.key_retirable_count);
}

/**
 * \brief Forced rekey leaves stale free / dirty / mapped kv=N-1 objects on
 *        flash while attach eagerly upgrades reserved metadata to kv=N,
 *        and KEY_RETIRABLE for kv=N-1 fires only after every stale object
 *        has been recycled.
 *
 * \details Scenario: Sequence:
 *            1. Format with kv=1, create one volume, write LEB 0 and LEB 1.
 *            2. Overwrite LEB 0 — leaves a dirty data PEB authenticated
 *               under kv=1, while LEB 0/LEB 1 stay mapped under kv=1 and
 *               the remaining unused PEBs carry kv=1 EC headers (free
 *               pool under kv=1).
 *            3. Re-init with `requested_write_key_version = 2`,
 *               allowlist = [1, 2].  Attach must:
 *                 - eagerly upgrade reserved metadata to kv=2,
 *                 - leave stale kv=1 data objects (mapped + dirty + free)
 *                   in place,
 *                 - keep all previously written data readable,
 *                 - NOT emit KEY_RETIRABLE(kv=1) yet (stale kv=1 objects
 *                   still hold a refcount).
 *            4. New writes must succeed and bind to kv=2 (forensically
 *               unobservable here, but the write path requires the
 *               write key to be available — see the missing-key test).
 *            5. Drain: overwrite every mapped LEB and erase every dirty
 *               PEB until the kv=1 refcount reaches zero.
 *               KEY_RETIRABLE(kv=1) must then fire exactly once.
 *
 * \expect
 *  - Phase 3: dirty_peb_count > 0, both LEBs read back the kv=1 payload,
 *    no KEY_RETIRABLE event.
 *  - Phase 4: write to LEB 0 with kv=2 succeeds and reads back.
 *  - Phase 5: KEY_RETIRABLE(kv=1) emitted at least once.
 */
ZTEST(ubi_secure_runtime_policy_budgets, forced_rekey_with_stale_objects)
{
	/* Phase 1+2: Format + write + overwrite under kv=1. */
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	cfg.event_cb = tracking_event_cb;

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	int vol_id = -1;

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	const uint8_t wdata_v1_a[] = { 0xA1, 0xA2, 0xA3, 0xA4 };
	const uint8_t wdata_v1_b[] = { 0xB1, 0xB2, 0xB3, 0xB4 };

	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata_v1_a, sizeof(wdata_v1_a)));
	zassert_ok(ubi_leb_write(g_ubi, vol_id, 1, wdata_v1_b, sizeof(wdata_v1_b)));

	/* Overwrite LEB 0 — produces a dirty kv=1 data PEB. */
	const uint8_t wdata_v1_a2[] = { 0xC1, 0xC2, 0xC3, 0xC4 };

	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata_v1_a2, sizeof(wdata_v1_a2)));

	struct ubi_device_info info_before_rekey = { 0 };

	zassert_ok(ubi_device_get_info(g_ubi, &info_before_rekey));
	zassert_true(info_before_rekey.dirty_peb_count >= 1,
		     "Overwrite must leave at least one dirty kv=1 PEB (got %zu)",
		     info_before_rekey.dirty_peb_count);
	zassert_true(info_before_rekey.free_peb_count >= 1,
		     "Free pool with kv=1 EC headers must remain (got %zu)",
		     info_before_rekey.free_peb_count);

	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;

	/* Reset event tracking — only events emitted under kv=2 matter from
	 * this point on. */
	memset(&ts, 0, sizeof(ts));

	/* Phase 3: Forced rekey to kv=2 with allowlist=[1,2]. */
	static const uint8_t allowed_v12[] = { 1, 2 };

	cfg.policy.requested_write_key_version = 2;
	cfg.policy.allowed_key_versions = allowed_v12;
	cfg.policy.allowed_key_versions_len = 2;
	cfg.event_cb = tracking_event_cb;

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

	struct ubi_device_info info_after_rekey = { 0 };

	zassert_ok(ubi_device_get_info(g_ubi, &info_after_rekey));

	/* Stale kv=1 objects must coexist with the freshly upgraded kv=2
	 * reserved metadata: dirty pool from the pre-rekey overwrite is
	 * still on flash, mapped data PEBs survive untouched, and free PEBs
	 * still carry kv=1 EC headers (lazy upgrade — they only get a kv=2
	 * EC when the wear-leveling allocator next picks them). */
	zassert_true(info_after_rekey.dirty_peb_count >= 1,
		     "Forced rekey must preserve stale kv=1 dirty PEBs (got %zu)",
		     info_after_rekey.dirty_peb_count);

	/* No premature KEY_RETIRABLE — stale kv=1 still holds the refcount. */
	zassert_equal(ts.key_retirable_count, 0,
		      "KEY_RETIRABLE(kv=1) must NOT fire while stale kv=1 "
		      "objects still exist (got %zu events)",
		      ts.key_retirable_count);

	/* Mixed-kv read: previously committed data is still authentic. */
	uint8_t rdata[4] = { 0 };

	zassert_ok(ubi_leb_read(g_ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, wdata_v1_a2, sizeof(wdata_v1_a2),
			  "LEB 0 (kv=1) must remain readable after forced rekey");
	zassert_ok(ubi_leb_read(g_ubi, vol_id, 1, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, wdata_v1_b, sizeof(wdata_v1_b),
			  "LEB 1 (kv=1) must remain readable after forced rekey");

	/* Phase 4: New write under kv=2 succeeds and reads back. */
	const uint8_t wdata_v2[] = { 0xE1, 0xE2, 0xE3, 0xE4 };

	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata_v2, sizeof(wdata_v2)));
	zassert_ok(ubi_leb_read(g_ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, wdata_v2, sizeof(wdata_v2),
			  "Post-rekey write must read back unchanged");

	/* Phase 5: drain stale kv=1 — overwrite remaining kv=1 LEB and
	 * cycle write+erase until KEY_RETIRABLE(kv=1) fires or we run out
	 * of safety budget. */
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(g_ubi, &info));

	const size_t total_pebs = info.free_peb_count + info.dirty_peb_count + 2;
	const size_t max_cycles = total_pebs * 4;

	/* Overwrite LEB 1 once with kv=2 to dirty its kv=1 data PEB. */
	zassert_ok(ubi_leb_write(g_ubi, vol_id, 1, wdata_v2, sizeof(wdata_v2)));

	for (size_t i = 0; i < max_cycles && ts.key_retirable_count == 0; i++) {
		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(g_ubi, &info));

		while (info.dirty_peb_count > 0) {
			int ret = ubi_device_erase_peb(g_ubi);

			if (ret != 0) {
				break;
			}
			memset(&info, 0, sizeof(info));
			zassert_ok(ubi_device_get_info(g_ubi, &info));
		}

		if (ts.key_retirable_count > 0) {
			break;
		}

		/* Force more wear-leveling cycles by alternating overwrites. */
		const uint8_t churn[] = { (uint8_t)i, 0xAA, 0x55, 0xFF };

		(void)ubi_leb_write(g_ubi, vol_id, (i & 1u) ? 1 : 0, churn, sizeof(churn));
	}

	/* Final flush: unmap both LEBs (release their mapped PEBs into dirty
	 * pool) then erase everything. */
	(void)ubi_leb_unmap(g_ubi, vol_id, 0);
	(void)ubi_leb_unmap(g_ubi, vol_id, 1);

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(g_ubi, &info));
	while (info.dirty_peb_count > 0) {
		zassert_ok(ubi_device_erase_peb(g_ubi));
		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(g_ubi, &info));
	}

	zassert_true(ts.key_retirable_count >= 1,
		     "KEY_RETIRABLE(kv=1) must fire after all stale kv=1 objects "
		     "are recycled (got %zu)",
		     ts.key_retirable_count);
	zassert_equal(ts.key_retirable_kv, 1, "Last KEY_RETIRABLE must be for kv=1, got %u",
		      ts.key_retirable_kv);
}
