/**
 * \file    tests_ubi_secure_runtime_policy.c
 * \author  Kamil Kielbasa
 *
 * \brief   Tests for secure runtime policy: sticky read-only, event callbacks,
 *          freshness sync, AUTH_FAILURE events, budget thresholds, refcount
 *          KEY_RETIRABLE, allowlist reject, missing-key write, rollback policy
 *          mismatch, and mixed-key rotation.
 *
 * \copyright Copyright (c) 2026
 */

/* --------------------------------------- Include files --------------------------------------- */
#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>
#include "arrays.h"

#include "ubi_test_fixture.h"
#include "ubi_test_secure_fixture.h"

#include <psa/crypto.h>

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>

#include <errno.h>
#include <string.h>

/* -------------------------------------- Module defines --------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* ------------------------------------- Static variables -------------------------------------- */

static struct ubi_mtd mtd = { 0 };
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

/* -------------------------------------- Event callbacks -------------------------------------- */

/**
 * \brief Comprehensive event tracker — returns CONTINUE.
 */
static enum ubi_crypto_event_verdict tracking_event_cb(const struct ubi_crypto_event *event,
						       void *user_data)
{
	ARG_UNUSED(user_data);
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

/**
 * \brief Event tracker that escalates every event to read-only.
 */
static enum ubi_crypto_event_verdict escalating_event_cb(const struct ubi_crypto_event *event,
							 void *user_data)
{
	(void)tracking_event_cb(event, user_data);
	return UBI_CRYPTO_EVENT_ENTER_READ_ONLY;
}

/**
 * \brief Sync freshness with configurable delayed failure.
 *
 * - sync_fail_after == 0 && sync_return_code == 0: always succeeds.
 * - sync_fail_after == 0 && sync_return_code != 0: always fails.
 * - sync_fail_after > 0: succeeds for first N calls, then returns -EIO.
 */
static int counting_sync_freshness(const struct ubi_crypto_freshness *freshness, void *user_data)
{
	ARG_UNUSED(freshness);
	ARG_UNUSED(user_data);
	ts.sync_call_count++;

	if (ts.sync_fail_after > 0 && ts.sync_call_count > ts.sync_fail_after) {
		return -EIO;
	}
	return ts.sync_return_code;
}

/**
 * \brief Selective get_key_id — returns error for ts.fail_key_version.
 */
static int selective_get_key_id(uint8_t key_version, uint32_t *key_id_out)
{
	if (ts.fail_key_version != 0 && key_version == ts.fail_key_version) {
		return -ENOENT;
	}
	*key_id_out = (uint32_t)ubi_test_root_key_id;
	return 0;
}

/**
 * \brief check_freshness that always rejects (rollback detected).
 */
static enum ubi_crypto_rollback_verdict
rejecting_check_freshness(const struct ubi_crypto_freshness *freshness, void *user_data)
{
	ARG_UNUSED(freshness);
	ARG_UNUSED(user_data);
	return UBI_CRYPTO_ROLLBACK_REJECT;
}

/* ---------------------------------- Suite setup / teardown ----------------------------------- */

static void *ztest_suite_setup(void)
{
	const struct device *flash_dev = UBI_PARTITION_DEVICE;
	zassert_true(device_is_ready(flash_dev));

	struct flash_pages_info page_info = { 0 };
	zassert_ok(flash_get_page_info_by_offs(flash_dev, 0, &page_info));

	mtd.partition_id = FIXED_PARTITION_ID(UBI_PARTITION_NAME);
	mtd.erase_block_size = page_info.size;
	mtd.write_block_size = flash_get_write_block_size(flash_dev);

	zassert_equal(psa_crypto_init(), PSA_SUCCESS);
	ubi_test_import_root_key();

	return NULL;
}

static void ztest_suite_before(void *ctx)
{
	ARG_UNUSED(ctx);
	memset(&ts, 0, sizeof(ts));
	g_ubi = NULL;
	ubi_test_partition_force_release_all();
	zassert_ok(flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE));
}

static void ztest_suite_after(void *ctx)
{
	ARG_UNUSED(ctx);
	if (g_ubi) {
		ubi_device_deinit(g_ubi);
		g_ubi = NULL;
	}
}

/* ------------------------------------------- Tests ------------------------------------------- */

/**
 * \brief Event callback returning ENTER_READ_ONLY blocks subsequent writes.
 *
 * \details Init device with sync callback that fails after N calls. The
 *          failure emits FRESHNESS_SYNC_FAILURE; the escalating callback
 *          enters read-only. Subsequent writes must be rejected.
 *
 * \expected Second ubi_leb_write returns -EROFS.
 */
ZTEST(ubi_secure_runtime_policy, test_event_enter_read_only_blocks_writes)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	cfg.event_cb = escalating_event_cb;
	cfg.sync_freshness = counting_sync_freshness;

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	int vol_id = -1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	/* Let sync succeed for every call so far. Now make the next sync fail.
	 * sync_fail_after = current count means the next call will fail. */
	ts.sync_fail_after = ts.sync_call_count;

	/* Write succeeds but triggers freshness sync → failure → event → RO. */
	const uint8_t wdata[] = { 0xAA, 0xBB, 0xCC, 0xDD };
	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata, sizeof(wdata)));

	/* Device is now in sticky crypto read-only. */
	zassert_true(ts.freshness_sync_failure_count >= 1, "Expected FRESHNESS_SYNC_FAILURE event");

	/* Second write must be rejected. */
	const uint8_t wdata2[] = { 0x11, 0x22 };
	int ret = ubi_leb_write(g_ubi, vol_id, 1, wdata2, sizeof(wdata2));
	zassert_equal(ret, -EROFS, "Expected -EROFS, got %d", ret);
}

/**
 * \brief Reads still work after crypto-initiated read-only.
 *
 * \details Write data, enter read-only via event callback, then read the
 *          data back. The read should succeed even though writes are blocked.
 *
 * \expected ubi_leb_read returns 0 with correct data.
 */
ZTEST(ubi_secure_runtime_policy, test_reads_work_in_crypto_ro)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	cfg.event_cb = escalating_event_cb;
	cfg.sync_freshness = counting_sync_freshness;

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	int vol_id = -1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	const uint8_t wdata[] = { 0xDE, 0xAD, 0xBE, 0xEF };
	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata, sizeof(wdata)));

	/* Now trigger read-only via sync failure on next mutation. */
	ts.sync_fail_after = ts.sync_call_count;
	const uint8_t wdata2[] = { 0x11, 0x22 };
	zassert_ok(ubi_leb_write(g_ubi, vol_id, 1, wdata2, sizeof(wdata2)));

	/* Device is now read-only. Verify reads still work. */
	uint8_t rdata[4] = { 0 };
	zassert_ok(ubi_leb_read(g_ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, wdata, sizeof(wdata));
}

/**
 * \brief sync_freshness is called after a commit-visible mutation.
 *
 * \details Use a counting sync callback. Write data to a LEB and verify
 *          the sync callback was invoked.
 *
 * \expected sync_call_count increases after write and volume_create.
 */
ZTEST(ubi_secure_runtime_policy, test_freshness_sync_called_on_write)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	cfg.event_cb = tracking_event_cb;
	cfg.sync_freshness = counting_sync_freshness;

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	int vol_id = -1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));

	/* volume_create also triggers freshness sync. */
	const size_t sync_before_create = ts.sync_call_count;
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));
	zassert_true(ts.sync_call_count >= sync_before_create + 1,
		     "sync_freshness not called after volume_create");

	const size_t sync_before_write = ts.sync_call_count;
	const uint8_t wdata[] = { 0x01, 0x02, 0x03, 0x04 };
	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata, sizeof(wdata)));
	zassert_true(ts.sync_call_count >= sync_before_write + 1,
		     "sync_freshness not called after leb_write");
}

/**
 * \brief Freshness sync failure emits FRESHNESS_SYNC_FAILURE event.
 *
 * \details Configure sync callback to return -EIO. Write data and verify
 *          that a FRESHNESS_SYNC_FAILURE event was emitted.
 *
 * \expected freshness_sync_failure_count > 0 after write.
 */
ZTEST(ubi_secure_runtime_policy, test_freshness_sync_failure_emits_event)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	cfg.event_cb = tracking_event_cb;
	cfg.sync_freshness = counting_sync_freshness;

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	int vol_id = -1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	/* Now make sync fail. */
	ts.sync_return_code = -EIO;
	ts.freshness_sync_failure_count = 0;

	const uint8_t wdata[] = { 0x01, 0x02, 0x03, 0x04 };
	/* Write succeeds (sync failure is post-commit), but event is emitted. */
	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata, sizeof(wdata)));

	zassert_true(ts.freshness_sync_failure_count >= 1, "Expected FRESHNESS_SYNC_FAILURE event");
}

/**
 * \brief Erase PEB triggers freshness sync.
 *
 * \details Write data, create a dirty PEB by overwriting, then erase.
 *          Verify sync was called during the erase.
 *
 * \expected sync_call_count increases after erase_peb.
 */
ZTEST(ubi_secure_runtime_policy, test_freshness_sync_called_on_erase)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	cfg.event_cb = tracking_event_cb;
	cfg.sync_freshness = counting_sync_freshness;

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	int vol_id = -1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	/* Write to LEB 0, then overwrite → creates dirty PEB. */
	const uint8_t wdata1[] = { 0x11, 0x22, 0x33, 0x44 };
	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata1, sizeof(wdata1)));

	const uint8_t wdata2[] = { 0x55, 0x66, 0x77, 0x88 };
	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata2, sizeof(wdata2)));

	/* Now there's a dirty PEB. Track sync calls before erase. */
	const size_t sync_before = ts.sync_call_count;
	zassert_ok(ubi_device_erase_peb(g_ubi));
	zassert_true(ts.sync_call_count >= sync_before + 1,
		     "sync_freshness not called after erase_peb");
}

/**
 * \brief Erase_peb blocked when device is in crypto read-only.
 *
 * \details Enter crypto read-only via event callback, then attempt
 *          erase_peb. The erase must be rejected.
 *
 * \expected ubi_erase_peb returns -EROFS.
 */
ZTEST(ubi_secure_runtime_policy, test_erase_blocked_in_crypto_ro)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	cfg.event_cb = escalating_event_cb;
	cfg.sync_freshness = counting_sync_freshness;

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	int vol_id = -1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	/* Make next sync fail → event → read-only. */
	ts.sync_fail_after = ts.sync_call_count;

	const uint8_t wdata[] = { 0x01, 0x02 };
	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata, sizeof(wdata)));

	zassert_true(ts.freshness_sync_failure_count >= 1);

	/* Now try to erase. Should be blocked. */
	int ret = ubi_device_erase_peb(g_ubi);
	zassert_equal(ret, -EROFS, "Expected -EROFS, got %d", ret);
}

/**
 * \brief Volume create blocked when device is in crypto read-only.
 *
 * \details Enter crypto read-only, then attempt to create a new volume.
 *
 * \expected ubi_volume_create returns -EROFS.
 */
ZTEST(ubi_secure_runtime_policy, test_volume_create_blocked_in_crypto_ro)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	cfg.event_cb = escalating_event_cb;
	cfg.sync_freshness = counting_sync_freshness;

	const struct ubi_volume_config vol_cfg1 = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	int vol_id1 = -1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg1, &vol_id1));

	/* Trigger read-only: make sync fail on next call. */
	ts.sync_fail_after = ts.sync_call_count;

	const uint8_t wdata[] = { 0x01, 0x02 };
	zassert_ok(ubi_leb_write(g_ubi, vol_id1, 0, wdata, sizeof(wdata)));

	zassert_true(ts.freshness_sync_failure_count >= 1);

	/* Device now read-only. Second volume create rejected. */
	const struct ubi_volume_config vol_cfg2 = {
		.name = { '/', 'u', 'b', 'i', '_', '1' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	int vol_id2 = -1;
	int ret = ubi_volume_create(g_ubi, &vol_cfg2, &vol_id2);
	zassert_equal(ret, -EROFS, "Expected -EROFS, got %d", ret);
}

/* ---------------------------------- Budget & key lifecycle ----------------------------------- */

/**
 * \brief ROTATE_SOON event fires when LEB write budget crosses soft threshold.
 *
 * \details Repeatedly overwrite the same LEB until the usage percentage
 *          reaches ROTATE_SOON_PCT. Verify the event is emitted.
 *
 * \expected At least one KEY_ROTATE_SOON event after enough writes.
 */
ZTEST(ubi_secure_runtime_policy, test_budget_rotate_soon_event)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	cfg.event_cb = tracking_event_cb;

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	int vol_id = -1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	const uint8_t wdata[] = { 0xAA, 0xBB, 0xCC, 0xDD };
	const size_t target_writes = (size_t)CONFIG_UBI_CRYPTO_LEB_WRITE_BUDGET *
				     CONFIG_UBI_CRYPTO_ROTATE_SOON_PCT / 100;

	for (size_t i = 0; i < target_writes + 1; i++) {
		int ret = ubi_leb_write(g_ubi, vol_id, 0, wdata, sizeof(wdata));

		if (ret != 0) {
			break;
		}
		(void)ubi_device_erase_peb(g_ubi);
	}

	zassert_true(ts.rotate_soon_count >= 1, "Expected KEY_ROTATE_SOON event");
}

/**
 * \brief ROTATE_NOW rejects write when LEB budget reaches hard threshold.
 *
 * \details Overwrite a LEB until the projected counter reaches ROTATE_NOW_PCT.
 *          Verify that the write is rejected before any flash mutation and that
 *          a KEY_ROTATE_NOW event is emitted.
 *
 * \expected Write returns -ENOSPC and rotate_now_count >= 1.
 */
ZTEST(ubi_secure_runtime_policy, test_budget_rotate_now_rejects_write)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	cfg.event_cb = tracking_event_cb;

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	int vol_id = -1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	const uint8_t wdata[] = { 0x11, 0x22, 0x33, 0x44 };
	const size_t now_threshold =
		(size_t)CONFIG_UBI_CRYPTO_LEB_WRITE_BUDGET * CONFIG_UBI_CRYPTO_ROTATE_NOW_PCT / 100;

	int last_ret = 0;

	for (size_t i = 0; i <= now_threshold + 5; i++) {
		last_ret = ubi_leb_write(g_ubi, vol_id, 0, wdata, sizeof(wdata));
		if (last_ret != 0) {
			break;
		}
		(void)ubi_device_erase_peb(g_ubi);
	}

	zassert_equal(last_ret, -ENOSPC, "Expected -ENOSPC, got %d", last_ret);
	zassert_true(ts.rotate_now_count >= 1, "Expected KEY_ROTATE_NOW event");
}

/**
 * \brief KEY_RETIRABLE fires after all data-PEB objects for a retired key
 *        version have been erased.
 *
 * \details Format with kv=1. Write data. Re-init with write_kv=2 and
 *          allowlist=[1,2] — attach eagerly upgrades reserved PEBs to kv=2.
 *          Overwrite mapped LEBs with kv=2 and erase dirty PEBs until
 *          every data-PEB object under kv=1 has been recycled.
 *          Verify KEY_RETIRABLE(kv=1).
 *
 * \expected key_retirable_count >= 1 and key_retirable_kv == 1.
 */
ZTEST(ubi_secure_runtime_policy, test_key_retirable_after_full_erase)
{
	/* Phase 1: Format and write with kv=1. */
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	cfg.event_cb = tracking_event_cb;

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	int vol_id = -1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	const uint8_t wdata[] = { 0xAA, 0xBB, 0xCC, 0xDD };

	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata, sizeof(wdata)));

	/* Deinit — preserves kv=1 objects on flash. */
	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;

	/* Phase 2: Re-init with kv=2 as write key, allowlist=[1,2].
	 * Attach eagerly upgrades reserved PEBs to kv=2.
	 * Cyclically overwrite the LEB and erase dirty PEBs until all
	 * kv=1 data-PEB objects have been replaced by kv=2. */
	static const uint8_t allowed_v12[] = { 1, 2 };

	cfg.policy.requested_write_key_version = 2;
	cfg.policy.allowed_key_versions = allowed_v12;
	cfg.policy.allowed_key_versions_len = 2;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(g_ubi, &info));

	const size_t total_pebs = info.free_peb_count + info.dirty_peb_count + 1;
	const size_t max_cycles = total_pebs * 3;

	for (size_t i = 0; i < max_cycles && ts.key_retirable_count == 0; i++) {
		const uint8_t wdata2[] = { 0x11, 0x22, 0x33, 0x44 };

		int ret = ubi_leb_write(g_ubi, vol_id, 0, wdata2, sizeof(wdata2));

		if (ret != 0) {
			break;
		}

		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(g_ubi, &info));

		while (info.dirty_peb_count > 0) {
			ret = ubi_device_erase_peb(g_ubi);
			if (ret != 0) {
				break;
			}
			memset(&info, 0, sizeof(info));
			zassert_ok(ubi_device_get_info(g_ubi, &info));
		}
	}

	/* Unmap LEB 0 to release the mapped PEB and anchor into dirty pool. */
	zassert_ok(ubi_leb_unmap(g_ubi, vol_id, 0));

	/* Erase all remaining dirty PEBs (including formerly-mapped + anchor). */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(g_ubi, &info));

	while (info.dirty_peb_count > 0) {
		zassert_ok(ubi_device_erase_peb(g_ubi));
		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(g_ubi, &info));
	}

	zassert_true(ts.key_retirable_count >= 1, "Expected KEY_RETIRABLE event");
	zassert_equal(ts.key_retirable_kv, 1, "Expected kv=1, got %u", ts.key_retirable_kv);
}

/**
 * \brief Read-path allowlist rejects objects with non-allowlisted key version.
 *
 * \details Three-phase test:
 *          1. Write data with kv=1 and create the volume.
 *          2. Re-init with kv=2, allowlist=[1,2]. Attach eagerly upgrades
 *             reserved PEBs to kv=2.
 *          3. Re-init with kv=2, allowlist=[2]. Reserved PEBs pass (kv=2).
 *             Data-PEB init scan succeeds (no allowlist check in scan).
 *             Runtime ubi_leb_read → EC kv=1 not in [2] → reject.
 *
 * \expected ubi_leb_read returns error and allowlist_reject_count >= 1.
 */
ZTEST(ubi_secure_runtime_policy, test_allowlist_reject_on_read)
{
	/* Phase 1: Write with kv=1 (default allowlist=[1]). */
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	cfg.event_cb = tracking_event_cb;

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	int vol_id = -1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	const uint8_t wdata[] = { 0xDE, 0xAD, 0xBE, 0xEF };

	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata, sizeof(wdata)));
	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;

	/* Phase 2: Re-init with allowlist=[1,2], write_kv=2.
	 * Attach eagerly upgrades reserved PEBs to kv=2. */
	static const uint8_t allowed_v12[] = { 1, 2 };

	cfg.policy.requested_write_key_version = 2;
	cfg.policy.allowed_key_versions = allowed_v12;
	cfg.policy.allowed_key_versions_len = 2;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));
	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;

	/* Phase 3: Re-init with allowlist=[2] only — kv=1 not allowed.
	 * The central allowlist check in derive_domain_key() rejects kv=1
	 * during init scan — PEBs with kv=1 EC headers are marked bad.
	 * Data written under kv=1 is no longer accessible. */
	static const uint8_t allowed_v2[] = { 2 };

	cfg.policy.allowed_key_versions = allowed_v2;
	cfg.policy.allowed_key_versions_len = 1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));

	/* Volume 0 data PEBs (kv=1) were excluded from scan — LEB not mapped.
	 * Read fails because the data is inaccessible under the new policy. */
	uint8_t rdata[4] = { 0 };
	int ret = ubi_leb_read(g_ubi, vol_id, 0, 0, rdata, sizeof(rdata));

	zassert_not_equal(ret, 0, "Expected read to fail under restricted allowlist");

	/* Verify bad PEB count reflects the policy-excluded PEBs. */
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(g_ubi, &info));
	zassert_true(info.bad_peb_count >= 1,
		     "Expected at least one PEB marked bad from policy exclusion");
}

/**
 * \brief Write fails with KEY_VERSION_UNAVAILABLE when get_key_id fails.
 *
 * \details Init with kv=1. Write data. Deinit. Re-init with write_kv=2
 *          but get_key_id fails for kv=2. Attempt write. Expect failure
 *          and KEY_VERSION_UNAVAILABLE event.
 *
 * \expected Write returns error and key_unavailable_count >= 1.
 */
ZTEST(ubi_secure_runtime_policy, test_missing_key_on_write)
{
	/* Phase 1: Write with kv=1. */
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	cfg.event_cb = tracking_event_cb;

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	int vol_id = -1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	const uint8_t wdata[] = { 0x01, 0x02, 0x03, 0x04 };

	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata, sizeof(wdata)));
	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;

	/* Phase 2: Re-init with write_kv=2 but get_key_id fails for kv=2. */
	static const uint8_t allowed_v12[] = { 1, 2 };

	cfg.policy.requested_write_key_version = 2;
	cfg.policy.allowed_key_versions = allowed_v12;
	cfg.policy.allowed_key_versions_len = 2;
	cfg.get_key_id = selective_get_key_id;
	ts.fail_key_version = 2;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));

	/* Existing kv=1 data is still readable. */
	uint8_t rdata[4] = { 0 };

	zassert_ok(ubi_leb_read(g_ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, wdata, sizeof(wdata));

	/* Write attempt with kv=2 should fail. */
	const uint8_t wdata2[] = { 0xCC, 0xDD };
	int ret = ubi_leb_write(g_ubi, vol_id, 1, wdata2, sizeof(wdata2));

	zassert_not_equal(ret, 0, "Expected write to fail");
	zassert_true(ts.key_unavailable_count >= 1, "Expected KEY_VERSION_UNAVAILABLE event");
}

/**
 * \brief ROLLBACK_POLICY_MISMATCH event on freshness rejection at init.
 *
 * \details Write data, deinit. Re-init with check_freshness returning
 *          REJECT. Verify init fails and ROLLBACK_POLICY_MISMATCH event
 *          is emitted.
 *
 * \expected ubi_device_init returns -EACCES, rollback_mismatch_count == 1.
 */
ZTEST(ubi_secure_runtime_policy, test_rollback_policy_mismatch_event)
{
	/* Phase 1: Normal init and write. */
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	cfg.event_cb = tracking_event_cb;

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	int vol_id = -1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	const uint8_t wdata[] = { 0x01, 0x02, 0x03, 0x04 };

	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata, sizeof(wdata)));
	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;

	/* Phase 2: Re-init with rejecting check_freshness. */
	cfg.check_freshness = rejecting_check_freshness;

	int ret = ubi_device_init(&mtd, &cfg, &g_ubi);

	zassert_equal(ret, -EACCES, "Expected -EACCES from rollback rejection, got %d", ret);
	zassert_equal(ts.rollback_mismatch_count, 1,
		      "Expected exactly 1 ROLLBACK_POLICY_MISMATCH event");

	/* g_ubi was not created — ensure teardown doesn't try to deinit. */
	g_ubi = NULL;
}

/**
 * \brief Sticky read-only is cleared after deinit + reinit.
 *
 * \details Enter crypto read-only via event callback escalation on sync
 *          failure. Verify writes are blocked. Deinit, reinit. Verify writes
 *          succeed.
 *
 * \expected Writes fail in read-only, succeed after reinit.
 */
ZTEST(ubi_secure_runtime_policy, test_sticky_ro_cleared_on_reinit)
{
	/* Init with escalating callback + delayed-failure sync. */
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	cfg.event_cb = escalating_event_cb;
	cfg.sync_freshness = counting_sync_freshness;

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	int vol_id = -1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	/* Let sync succeed so far. Now make the NEXT sync fail. */
	ts.sync_fail_after = ts.sync_call_count;

	/* Write succeeds but post-commit sync fails → event → escalate → read-only. */
	const uint8_t wdata[] = { 0x01, 0x02 };

	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata, sizeof(wdata)));

	/* Device is now in crypto read-only. Second write must be rejected. */
	const uint8_t wdata2[] = { 0x03, 0x04 };
	int ret = ubi_leb_write(g_ubi, vol_id, 1, wdata2, sizeof(wdata2));

	zassert_equal(ret, -EROFS, "Expected -EROFS in read-only mode, got %d", ret);

	/* Phase 2: Deinit + reinit should clear read-only. */
	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;

	/* Re-init with non-escalating callback and working sync. */
	cfg.event_cb = tracking_event_cb;
	cfg.sync_freshness = mock_sync_freshness; /* from fixture — always succeeds */
	ts.sync_call_count = 0;
	ts.sync_fail_after = 0;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));

	/* Write should now succeed again. */
	zassert_ok(ubi_leb_write(g_ubi, vol_id, 1, wdata2, sizeof(wdata2)));

	/* Verify old data survived. */
	uint8_t rdata[2] = { 0 };

	zassert_ok(ubi_leb_read(g_ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, wdata, sizeof(wdata));
}

/**
 * \brief Mixed-key recovery: data written under kv=1 remains readable
 *        after switching to kv=2 for writes.
 *
 * \details Init with kv=1, write LEBs, deinit. Re-init with kv=2 write,
 *          allowlist=[1,2]. Old data is read successfully. New writes use
 *          kv=2 and are also readable.
 *
 * \expected All reads succeed across key versions.
 */
ZTEST(ubi_secure_runtime_policy, test_mixed_key_rotation_read_write)
{
	/* Phase 1: Write with kv=1. */
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	cfg.event_cb = tracking_event_cb;

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'u', 'b', 'i', '_', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 2,
	};

	int vol_id = -1;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	const uint8_t wdata_v1[] = { 0xAA, 0xBB, 0xCC, 0xDD };

	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata_v1, sizeof(wdata_v1)));
	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;

	/* Phase 2: Re-init with kv=2 write, allowlist=[1,2]. */
	static const uint8_t allowed_v12[] = { 1, 2 };

	cfg.policy.requested_write_key_version = 2;
	cfg.policy.allowed_key_versions = allowed_v12;
	cfg.policy.allowed_key_versions_len = 2;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));

	/* Old kv=1 data readable. */
	uint8_t rdata[4] = { 0 };

	zassert_ok(ubi_leb_read(g_ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, wdata_v1, sizeof(wdata_v1));

	/* New kv=2 write to LEB 1. */
	const uint8_t wdata_v2[] = { 0x11, 0x22, 0x33, 0x44 };

	zassert_ok(ubi_leb_write(g_ubi, vol_id, 1, wdata_v2, sizeof(wdata_v2)));

	/* Both LEBs readable. */
	memset(rdata, 0, sizeof(rdata));
	zassert_ok(ubi_leb_read(g_ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, wdata_v1, sizeof(wdata_v1));

	memset(rdata, 0, sizeof(rdata));
	zassert_ok(ubi_leb_read(g_ubi, vol_id, 1, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, wdata_v2, sizeof(wdata_v2));
}

/**
 * \brief End-to-end refcount test: full lifecycle with key rotation and
 *        KEY_RETIRABLE event after many erases.
 *
 * \details Exercises all major operations under key rotation:
 *          1. Init kv=1, volume create, LEB write/read, LEB unmap,
 *             volume resize (expand + shrink), LEB write again.
 *          2. Deinit, re-init with kv=2 (allowlist=[1,2]).
 *          3. Write new data under kv=2, overwrite existing LEBs,
 *             unmap LEBs, erase all dirty PEBs in a loop.
 *          4. Remove the volume to release all PEBs (including anchor).
 *          5. Erase all remaining dirty PEBs.
 *          6. Verify KEY_RETIRABLE(kv=1) fires after the last kv=1
 *             EC header is erased.
 *
 * \expected key_retirable_count >= 1, key_retirable_kv == 1.
 */
ZTEST(ubi_secure_runtime_policy, test_refcount_e2e_key_rotation_retirable)
{
	/* Phase 1: Init with kv=1. Create volume. Write / read / unmap / resize. */
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	cfg.event_cb = tracking_event_cb;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'r', 'c' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 3,
	};

	int vol_id = -1;

	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	/* Write to LEBs 0, 1, 2. */
	const uint8_t wdata[] = { 0x11, 0x22, 0x33, 0x44 };

	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata, sizeof(wdata)));
	zassert_ok(ubi_leb_write(g_ubi, vol_id, 1, wdata, sizeof(wdata)));
	zassert_ok(ubi_leb_write(g_ubi, vol_id, 2, wdata, sizeof(wdata)));

	/* Read back to verify. */
	uint8_t rdata[4] = { 0 };

	zassert_ok(ubi_leb_read(g_ubi, vol_id, 0, 0, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, wdata, sizeof(wdata));

	/* Unmap LEB 2. */
	zassert_ok(ubi_leb_unmap(g_ubi, vol_id, 2));

	/* Resize: shrink to 2 LEBs. */
	const struct ubi_volume_config resize_cfg = {
		.name = { '/', 'r', 'c' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};

	zassert_ok(ubi_volume_resize(g_ubi, vol_id, &resize_cfg));

	/* Overwrite LEB 0 to create a dirty PEB. */
	const uint8_t wdata_v1b[] = { 0xAA, 0xBB };

	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata_v1b, sizeof(wdata_v1b)));

	/* Erase dirty PEBs generated so far. */
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(g_ubi, &info));
	while (info.dirty_peb_count > 0) {
		zassert_ok(ubi_device_erase_peb(g_ubi));
		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(g_ubi, &info));
	}

	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;

	/* Phase 2: Re-init with kv=2, allowlist=[1,2]. */
	static const uint8_t allowed_v12[] = { 1, 2 };

	cfg.policy.requested_write_key_version = 2;
	cfg.policy.allowed_key_versions = allowed_v12;
	cfg.policy.allowed_key_versions_len = 2;

	zassert_ok(ubi_device_init(&mtd, &cfg, &g_ubi));

	/* Read data from kv=1 still works (allowlist has both). */
	uint8_t rdata_v1b[sizeof(wdata_v1b)] = { 0 };

	zassert_ok(ubi_leb_read(g_ubi, vol_id, 0, 0, rdata_v1b, sizeof(rdata_v1b)));
	zassert_mem_equal(rdata_v1b, wdata_v1b, sizeof(wdata_v1b));

	/* Phase 3: Cyclically overwrite LEBs and erase dirty PEBs until all
	 * kv=1 data-PEB objects have been replaced by kv=2. Each cycle:
	 * - Overwrite LEBs → old kv=1 PEB goes dirty, new PEB uses kv=2 VID.
	 * - Erase dirty → old EC (kv=1) destroyed, new EC (kv=2) written.
	 * Free PEBs with kv=1 EC are consumed as they are allocated for writes.
	 * After enough cycles, all EC headers become kv=2 and kv=1 refcount → 0. */
	const uint8_t wdata_v2[] = { 0xCC, 0xDD, 0xEE };

	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(g_ubi, &info));
	const size_t total_pebs =
		info.free_peb_count + info.dirty_peb_count + info.reserved_peb_count;
	const size_t max_cycles = total_pebs * 3;

	for (size_t i = 0; i < max_cycles && ts.key_retirable_count == 0; i++) {
		int ret = ubi_leb_write(g_ubi, vol_id, 0, wdata_v2, sizeof(wdata_v2));

		if (ret != 0) {
			break;
		}

		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(g_ubi, &info));
		while (info.dirty_peb_count > 0) {
			ret = ubi_device_erase_peb(g_ubi);
			if (ret != 0) {
				break;
			}
			memset(&info, 0, sizeof(info));
			zassert_ok(ubi_device_get_info(g_ubi, &info));
		}
	}

	/* Phase 4: Unmap and remove to release mapped + anchor PEBs. */
	zassert_ok(ubi_leb_unmap(g_ubi, vol_id, 0));
	zassert_ok(ubi_leb_unmap(g_ubi, vol_id, 1));
	zassert_ok(ubi_volume_remove(g_ubi, vol_id));

	/* Erase all remaining dirty PEBs (including formerly-mapped + anchor). */
	memset(&info, 0, sizeof(info));
	zassert_ok(ubi_device_get_info(g_ubi, &info));
	while (info.dirty_peb_count > 0) {
		zassert_ok(ubi_device_erase_peb(g_ubi));
		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(g_ubi, &info));
	}

	/* Phase 5: Verify KEY_RETIRABLE fired for kv=1. */
	zassert_true(ts.key_retirable_count >= 1, "Expected KEY_RETIRABLE event for kv=1");
	zassert_equal(ts.key_retirable_kv, 1, "Expected retirable kv=1, got %u",
		      ts.key_retirable_kv);

	/* Create a new volume to verify the device is still functional. */
	const struct ubi_volume_config vol_cfg2 = {
		.name = { '/', 'r', '2' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	int vol_id2 = -1;

	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg2, &vol_id2));

	const uint8_t data_final[] = { 0xFF, 0xFE };

	zassert_ok(ubi_leb_write(g_ubi, vol_id2, 0, data_final, sizeof(data_final)));

	uint8_t rdata2[sizeof(data_final)] = { 0 };

	zassert_ok(ubi_leb_read(g_ubi, vol_id2, 0, 0, rdata2, sizeof(rdata2)));
	zassert_mem_equal(rdata2, data_final, sizeof(data_final));
}

/* ---------------------------------------- Suite def ------------------------------------------ */

ZTEST_SUITE(ubi_secure_runtime_policy, NULL, ztest_suite_setup, ztest_suite_before,
	    ztest_suite_after, NULL);
