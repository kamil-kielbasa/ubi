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

/* Include files -------------------------------------------------------------------------------- */

#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>
#include "arrays.h"

#include "ubi_test_fixture.h"
#include "ubi_test_secure_fixture.h"

#include "ubi_secure_test_hooks.h"

#include <psa/crypto.h>

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>

#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

/* Module types and type definitiones ----------------------------------------------------------- */

/* Module interface variables and constants ----------------------------------------------------- */
#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

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
	(void)freshness;
	(void)user_data;
	ts.sync_call_count++;

	if (ts.sync_fail_after > 0 && ts.sync_call_count > ts.sync_fail_after) {
		return -EIO;
	}
	return ts.sync_return_code;
}

/**
 * \brief Selective get_key_id — returns error for ts.fail_key_version.
 */
static int selective_get_key_id(uint8_t key_version, psa_key_id_t *key_id_out)
{
	if (ts.fail_key_version != 0 && key_version == ts.fail_key_version) {
		return -ENOENT;
	}
	*key_id_out = ubi_test_root_key_id;
	return 0;
}

/**
 * \brief check_freshness that always rejects (rollback detected).
 */
static enum ubi_crypto_rollback_verdict
rejecting_check_freshness(const struct ubi_crypto_freshness *freshness, void *user_data)
{
	(void)freshness;
	(void)user_data;
	return UBI_CRYPTO_ROLLBACK_REJECT;
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
/**
 * \brief Event callback returning ENTER_READ_ONLY blocks subsequent writes.
 *
 * \details Scenario: Init device with sync callback that fails after N calls. The
 *          failure emits FRESHNESS_SYNC_FAILURE; the escalating callback
 *          enters read-only. Subsequent writes must be rejected.
 *
 * \expect Second ubi_leb_write returns -EROFS.
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

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
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
 * \details Scenario: Write data, enter read-only via event callback, then read the
 *          data back. The read should succeed even though writes are blocked.
 *
 * \expect ubi_leb_read returns 0 with correct data.
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

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
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
 * \details Scenario: Use a counting sync callback. Write data to a LEB and verify
 *          the sync callback was invoked.
 *
 * \expect sync_call_count increases after write and volume_create.
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

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

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
 * \details Scenario: Configure sync callback to return -EIO. Write data and verify
 *          that a FRESHNESS_SYNC_FAILURE event was emitted.
 *
 * \expect freshness_sync_failure_count > 0 after write.
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

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
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
 * \details Scenario: Write data, create a dirty PEB by overwriting, then erase.
 *          Verify sync was called during the erase.
 *
 * \expect sync_call_count increases after erase_peb.
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

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
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
 * \details Scenario: Enter crypto read-only via event callback, then attempt
 *          erase_peb. The erase must be rejected.
 *
 * \expect ubi_erase_peb returns -EROFS.
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

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
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
 * \details Scenario: Enter crypto read-only, then attempt to create a new volume.
 *
 * \expect ubi_volume_create returns -EROFS.
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

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
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

/**
 * \brief KEY_RETIRABLE fires after all data-PEB objects for a retired key
 *        version have been erased.
 *
 * \details Scenario: Format with kv=1. Write data. Re-init with write_kv=2 and
 *          allowlist=[1,2] — attach eagerly upgrades reserved PEBs to kv=2.
 *          Overwrite mapped LEBs with kv=2 and erase dirty PEBs until
 *          every data-PEB object under kv=1 has been recycled.
 *          Verify KEY_RETIRABLE(kv=1).
 *
 * \expect key_retirable_count >= 1 and key_retirable_kv == 1.
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

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
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

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

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
 * \details Scenario: Three-phase test:
 *          1. Write data with kv=1 and create the volume.
 *          2. Re-init with kv=2, allowlist=[1,2]. Attach eagerly upgrades
 *             reserved PEBs to kv=2.
 *          3. Re-init with kv=2, allowlist=[2]. Reserved PEBs pass (kv=2).
 *             Data-PEB init scan succeeds (no allowlist check in scan).
 *             Runtime ubi_leb_read → EC kv=1 not in [2] → reject.
 *
 * \expect ubi_leb_read returns error and allowlist_reject_count >= 1.
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

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
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

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;

	/* Phase 3: Re-init with allowlist=[2] only — kv=1 not allowed.
	 * The central allowlist check in derive_domain_key() rejects kv=1
	 * during init scan — PEBs with kv=1 EC headers are marked bad.
	 * Data written under kv=1 is no longer accessible. */
	static const uint8_t allowed_v2[] = { 2 };

	cfg.policy.allowed_key_versions = allowed_v2;
	cfg.policy.allowed_key_versions_len = 1;

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

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
 * \details Scenario: Init with kv=1. Write data. Deinit. Re-init with write_kv=2
 *          but get_key_id fails for kv=2. Attempt write. Expect failure
 *          and KEY_VERSION_UNAVAILABLE event.
 *
 * \expect Write returns error and key_unavailable_count >= 1.
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

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
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

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

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
 * \details Scenario: Write data, deinit. Re-init with check_freshness returning
 *          REJECT. Verify init fails and ROLLBACK_POLICY_MISMATCH event
 *          is emitted.
 *
 * \expect ubi_device_init returns -EACCES, rollback_mismatch_count == 1.
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

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
	zassert_ok(ubi_volume_create(g_ubi, &vol_cfg, &vol_id));

	const uint8_t wdata[] = { 0x01, 0x02, 0x03, 0x04 };

	zassert_ok(ubi_leb_write(g_ubi, vol_id, 0, wdata, sizeof(wdata)));
	zassert_ok(ubi_device_deinit(g_ubi));
	g_ubi = NULL;

	/* Phase 2: Re-init with rejecting check_freshness. */
	cfg.check_freshness = rejecting_check_freshness;

	int ret = ubi_device_init(&flash, &cfg, &g_ubi);

	zassert_equal(ret, -EACCES, "Expected -EACCES from rollback rejection, got %d", ret);
	zassert_equal(ts.rollback_mismatch_count, 1,
		      "Expected exactly 1 ROLLBACK_POLICY_MISMATCH event");

	/* g_ubi was not created — ensure teardown doesn't try to deinit. */
	g_ubi = NULL;
}

/**
 * \brief Sticky read-only is cleared after deinit + reinit.
 *
 * \details Scenario: Enter crypto read-only via event callback escalation on sync
 *          failure. Verify writes are blocked. Deinit, reinit. Verify writes
 *          succeed.
 *
 * \expect Writes fail in read-only, succeed after reinit.
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

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
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

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

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
 * \details Scenario: Init with kv=1, write LEBs, deinit. Re-init with kv=2 write,
 *          allowlist=[1,2]. Old data is read successfully. New writes use
 *          kv=2 and are also readable.
 *
 * \expect All reads succeed across key versions.
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

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));
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

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

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
 * \details Scenario: Exercises all major operations under key rotation:
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
 * \expect key_retirable_count >= 1, key_retirable_kv == 1.
 */
ZTEST(ubi_secure_runtime_policy, test_refcount_e2e_key_rotation_retirable)
{
	/* Phase 1: Init with kv=1. Create volume. Write / read / unmap / resize. */
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	cfg.event_cb = tracking_event_cb;

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

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

	zassert_ok(ubi_device_init(&flash, &cfg, &g_ubi));

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

/*
 * The metadata-domain budget tests rely on ubi_secure_test_set_metadata_counters()
 * to advance the global next_*_counter values close to ROTATE_NOW_PCT in a
 * single step.  This keeps the tests usable on small geometries
 * (e.g. native_sim has ~14 free PEBs while CONFIG_UBI_CRYPTO_METADATA_COUNTER_BUDGET
 * is 100) and lets each test isolate exactly one metadata domain by
 * preloading the others to zero.
 */

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
ZTEST(ubi_secure_runtime_policy, test_reserved_metadata_budget_exhausts_blocks_until_rotation)
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
ZTEST(ubi_secure_runtime_policy, test_ec_metadata_budget_exhausts_blocks_until_rotation)
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
ZTEST(ubi_secure_runtime_policy, test_vid_metadata_budget_exhausts_blocks_until_rotation)
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
ZTEST(ubi_secure_runtime_policy, test_metadata_rotate_soon_emitted_below_now)
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
ZTEST(ubi_secure_runtime_policy, test_metadata_budget_resets_on_key_rotation_reattach)
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

/*
 * The LEB budget tracks per-{kv, vol_id, lnum} write counter and
 * authenticated bytes.  These tests overwrite the same lnum repeatedly
 * (write + erase) so the per-LEB counter rises by one per iteration.
 *
 * The hook ubi_secure_test_set_metadata_counters() is called every
 * iteration to reset the EC and reserved counters to zero — that keeps
 * any metadata-domain budget from tripping first and lets each test
 * assert that the LEB budget alone caused the failure.
 */

#define LEB_BUDGET_NOW_THRESHOLD \
	((size_t)CONFIG_UBI_CRYPTO_LEB_WRITE_BUDGET * CONFIG_UBI_CRYPTO_ROTATE_NOW_PCT / 100)
#define LEB_BUDGET_SOON_THRESHOLD \
	((size_t)CONFIG_UBI_CRYPTO_LEB_WRITE_BUDGET * CONFIG_UBI_CRYPTO_ROTATE_SOON_PCT / 100)

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
ZTEST(ubi_secure_runtime_policy, test_leb_budget_rotate_soon_emitted_below_now)
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
ZTEST(ubi_secure_runtime_policy, test_leb_budget_exhausts_blocks_until_rotation)
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

/*
 * When the write-active key version advances, the authenticated
 * `vid_next_counter_floor` is reinitialized to 0:
 * `K_volume_identifier[new_kv]` is a fresh HKDF child key whose 48-bit
 * nonce range is fully unused, so the counter must restart from the
 * bottom rather than skip into the middle.
 *
 * The tests below drive `next_vid_counter` to a high value under
 * kv=1, snapshot it into reserved metadata via a volume_create, and
 * then verify the on-rotation behaviour at reattach.
 */

/**
 * \brief vid_next_counter_floor is reset to 0 when write-active kv advances.
 *
 * \details Scenario: Initialize with default kv=1, set the in-RAM VID counter to 100,
 *          create a volume so the high watermark is snapshotted into reserved metadata,
 *          deinit. Reinit with kv=2 and allowlist=[1,2]. Inspect the metadata counters.
 *
 * \expect After rotation next_vid_counter == 0 (the counter is reset on kv advance).
 */
ZTEST(ubi_secure_runtime_policy, test_vid_floor_resets_on_rotation)
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
ZTEST(ubi_secure_runtime_policy, test_vid_floor_persists_within_same_kv)
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
ZTEST(ubi_secure_runtime_policy, test_vid_floor_reset_writes_use_low_counters)
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

/*
 * Reserved-PEB key-version refcount accounts for one DEV header plus one
 * VOL header per volume on every reserved PEB.  Every reserved metadata
 * commit (volume_create / volume_resize / volume_remove) transitions the
 * (kv, vol_count) state and must apply the new contribution before
 * releasing the old one ("inc-first / dec-last").  Otherwise the refcount
 * for an unchanged kv would transiently drop to zero and could spuriously
 * fire KEY_RETIRABLE for the still-active write key version.
 */

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
ZTEST(ubi_secure_runtime_policy, test_reserved_refcount_no_spurious_key_retirable)
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
ZTEST(ubi_secure_runtime_policy, test_forced_rekey_with_stale_objects)
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

ZTEST_SUITE(ubi_secure_runtime_policy, NULL, ztest_suite_setup, ztest_suite_before,
	    ztest_suite_after, NULL);
