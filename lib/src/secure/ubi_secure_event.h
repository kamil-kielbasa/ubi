/**
 * \file    ubi_secure_event.h
 * \author  Kamil Kielbasa
 * \brief   Secure backend event emission and freshness sync helpers.
 *
 * \details Inline helpers called from ubi_secure_volume.c, ubi_secure_leb.c,
 *          ubi_secure_runtime.c, and ubi_core_init.c to emit crypto events
 *          and drive freshness sync cadence.
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard ------------------------------------------------------------------------------- */
#ifndef UBI_SECURE_EVENT_H
#define UBI_SECURE_EVENT_H

/* Include files ------------------------------------------------------------------------------- */
#include "ubi_internal.h"
#include "ubi_secure_test_hooks.h"
#include "ubi_secure_types.h"

#include <ubi_crypto.h>

#include <zephyr/sys/__assert.h>

#include <errno.h>

/* Defines ------------------------------------------------------------------------------------- */

/** Percentage base for budget calculations. */
#define UBI_SECURE_PERCENT_BASE (100U)

/* Helpers ------------------------------------------------------------------------------------- */

/**
 * \brief Find the allowlist slot for a given key version.
 *
 * \param[in] ubi  UBI device (caller holds mutex).
 * \param[in] kv   Key version to look up.
 *
 * \retval >= 0     Slot index (0 .. allowed_key_versions_len - 1).
 * \retval -ENOENT  Key version not found in the allowlist.
 */
static inline int ubi_secure_kv_slot(const struct ubi_device *ubi, uint8_t kv)
{
	__ASSERT_NO_MSG(ubi != NULL);
	__ASSERT_NO_MSG(ubi->crypto_cfg != NULL);

	const struct ubi_crypto_policy *p = &ubi->crypto_cfg->policy;

	for (size_t i = 0; i < p->allowed_key_versions_len; i++) {
		if (p->allowed_key_versions[i] == kv) {
			return (int)i;
		}
	}
	return -ENOENT;
}

/**
 * \brief Increment PEB refcount for a key version.
 *
 * Called when a new EC header is written (format, erase-rewrite) or when
 * an existing EC header is discovered during init scan.
 *
 * \param[in,out] ubi  UBI device (caller holds mutex).
 * \param[in]     kv   Key version of the EC header.
 */
static inline void ubi_secure_key_refcount_inc(struct ubi_device *ubi, uint8_t kv)
{
	__ASSERT_NO_MSG(ubi != NULL);

	const int slot = ubi_secure_kv_slot(ubi, kv);

	if (slot >= 0) {
		ubi->key_peb_refcount[slot]++;
	}
}

/**
 * \brief Build an ubi_crypto_freshness snapshot from current device state.
 *
 * \param[in] ubi  UBI device (caller holds mutex).
 *
 * \return Freshness descriptor.
 */
static inline struct ubi_crypto_freshness
ubi_secure_freshness_snapshot(const struct ubi_device *ubi)
{
	__ASSERT_NO_MSG(ubi != NULL);

	return (struct ubi_crypto_freshness){
		.device_revision = ubi->cached_device_revision,
		.global_sqnum = ubi->global_sqnum,
	};
}

/**
 * \brief Emit a crypto event and handle the callback verdict.
 *
 * Calls the application-provided event_cb with the given event.  If the
 * callback returns UBI_CRYPTO_EVENT_ENTER_READ_ONLY, the sticky crypto
 * read-only flag is set.
 *
 * \param[in,out] ubi    UBI device (caller holds mutex).
 * \param[in]     event  Event payload to emit.
 */
static inline void ubi_secure_emit_event(struct ubi_device *ubi,
					 const struct ubi_crypto_event *event)
{
	__ASSERT_NO_MSG(ubi != NULL);
	__ASSERT_NO_MSG(event != NULL);

	if (ubi->crypto_cfg == NULL || ubi->crypto_cfg->event_cb == NULL) {
		return;
	}

	const enum ubi_crypto_event_verdict verdict =
		ubi->crypto_cfg->event_cb(event, ubi->crypto_cfg->user_data);

	if (verdict == UBI_CRYPTO_EVENT_ENTER_READ_ONLY) {
		ubi->read_only_crypto = true;
	}
}

/**
 * \brief Decrement PEB refcount for a key version and emit KEY_RETIRABLE if zero.
 *
 * Called when a PEB is erased, destroying the old EC header's key version.
 * If the refcount reaches zero and the key version is not the write-active
 * version, a KEY_RETIRABLE event is emitted.
 *
 * \param[in,out] ubi  UBI device (caller holds mutex).
 * \param[in]     kv   Key version of the destroyed EC header.
 */
static inline void ubi_secure_key_refcount_dec_and_check(struct ubi_device *ubi, uint8_t kv)
{
	__ASSERT_NO_MSG(ubi != NULL);

	const int slot = ubi_secure_kv_slot(ubi, kv);

	if (slot < 0) {
		return;
	}

	if (ubi->key_peb_refcount[slot] > 0) {
		ubi->key_peb_refcount[slot]--;
	}

	if (ubi->key_peb_refcount[slot] == 0 &&
	    kv != ubi->crypto_cfg->policy.requested_write_key_version) {
		const struct ubi_crypto_event event = {
			.type = UBI_CRYPTO_EVENT_KEY_RETIRABLE,
			.freshness = ubi_secure_freshness_snapshot(ubi),
			.rotation = { .key_version = kv },
		};

		ubi_secure_emit_event(ubi, &event);
	}
}

/**
 * \brief Attempt freshness sync after a commit-visible mutation.
 *
 * Implements the delta-based freshness sync cadence:
 * - delta == 0: sync after every commit.
 * - delta > 0: sync when mutations_since_sync reaches delta.
 *
 * On sync failure, emits FRESHNESS_SYNC_FAILURE and optionally enters
 * read-only based on CONFIG_UBI_CRYPTO_STRICT_RO_ON_FRESHNESS_SYNC_FAILURE.
 *
 * \param[in,out] ubi  UBI device (caller holds mutex).
 */
static inline void ubi_secure_maybe_sync_freshness(struct ubi_device *ubi)
{
	__ASSERT_NO_MSG(ubi != NULL);

	if (ubi->crypto_cfg == NULL || ubi->crypto_cfg->sync_freshness == NULL) {
		return;
	}

	const size_t delta = CONFIG_UBI_CRYPTO_FRESHNESS_SYNC_DELTA;

	ubi->freshness_mutations_since_sync++;

	if (delta > 0 && ubi->freshness_mutations_since_sync < delta) {
		return;
	}

	const struct ubi_crypto_freshness freshness = ubi_secure_freshness_snapshot(ubi);

	const int rc = ubi->crypto_cfg->sync_freshness(&freshness, ubi->crypto_cfg->user_data);

	ubi->freshness_mutations_since_sync = 0;

	if (rc != 0
#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)
	    || ubi_secure_test_hook_check(UBI_SECURE_HOOK_FRESHNESS_SYNC_FAIL)
#endif
	) {
		const struct ubi_crypto_event event = {
			.type = UBI_CRYPTO_EVENT_FRESHNESS_SYNC_FAILURE,
			.freshness = freshness,
			.sync = { .sync_errno = rc },
		};

		ubi_secure_emit_event(ubi, &event);

#if defined(CONFIG_UBI_CRYPTO_STRICT_RO_ON_FRESHNESS_SYNC_FAILURE)
		ubi->read_only_crypto = true;
#endif
	}
}

/**
 * \brief Check LEB usage budgets and emit rotation events if thresholds crossed.
 *
 * Computes the maximum usage percentage across the counter budget
 * (UBI_CRYPTO_LEB_WRITE_BUDGET) and the byte budget
 * (UBI_CRYPTO_LEB_TOTAL_AUTH_BYTES_BUDGET). If the percentage crosses
 * ROTATE_SOON_PCT or ROTATE_NOW_PCT, the corresponding event is emitted.
 *
 * \param[in,out] ubi        UBI device (caller holds mutex).
 * \param[in]     kv         Key version used for this write.
 * \param[in]     vol_id     Volume ID.
 * \param[in]     counter    New leb_write_counter value after this write.
 * \param[in]     auth_bytes New leb_total_auth_bytes after this write.
 */
/**
 * \brief Compute usage percentage from a value and budget.
 *
 * Returns the integer percentage clamped to UBI_SECURE_PERCENT_BASE.
 * Returns UBI_SECURE_PERCENT_BASE when the budget is zero.
 *
 * \param[in] value   Current usage value.
 * \param[in] budget  Budget limit.
 *
 * \return Usage percentage (0 .. UBI_SECURE_PERCENT_BASE).
 */
static inline unsigned int ubi_secure_usage_pct(uint64_t value, uint64_t budget)
{
	if (budget == 0) {
		return UBI_SECURE_PERCENT_BASE;
	}

	const unsigned int pct = (unsigned int)((value * UBI_SECURE_PERCENT_BASE) / budget);

	return (pct > UBI_SECURE_PERCENT_BASE) ? UBI_SECURE_PERCENT_BASE : pct;
}

static inline void ubi_secure_check_leb_budget(struct ubi_device *ubi, uint8_t kv, size_t vol_id,
					       uint64_t counter, uint64_t auth_bytes)
{
	__ASSERT_NO_MSG(ubi != NULL);

	if (ubi->crypto_cfg == NULL || ubi->crypto_cfg->event_cb == NULL) {
		return;
	}

	const unsigned int counter_pct =
		ubi_secure_usage_pct(counter, CONFIG_UBI_CRYPTO_LEB_WRITE_BUDGET);
	const unsigned int bytes_pct =
		ubi_secure_usage_pct(auth_bytes, CONFIG_UBI_CRYPTO_LEB_TOTAL_AUTH_BYTES_BUDGET);
	const uint8_t usage_pct = (uint8_t)((counter_pct > bytes_pct) ? counter_pct : bytes_pct);

	if (usage_pct >= CONFIG_UBI_CRYPTO_ROTATE_NOW_PCT) {
		const struct ubi_crypto_event event = {
			.type = UBI_CRYPTO_EVENT_KEY_ROTATE_NOW,
			.freshness = ubi_secure_freshness_snapshot(ubi),
			.rotation = { .key_version = kv,
				      .volume_id = (uint32_t)vol_id,
				      .usage_pct = usage_pct },
		};

		ubi_secure_emit_event(ubi, &event);
	} else if (usage_pct >= CONFIG_UBI_CRYPTO_ROTATE_SOON_PCT) {
		const struct ubi_crypto_event event = {
			.type = UBI_CRYPTO_EVENT_KEY_ROTATE_SOON,
			.freshness = ubi_secure_freshness_snapshot(ubi),
			.rotation = { .key_version = kv,
				      .volume_id = (uint32_t)vol_id,
				      .usage_pct = usage_pct },
		};

		ubi_secure_emit_event(ubi, &event);
	}
}

/**
 * \brief Check whether projected usage would exhaust the LEB budget.
 *
 * Computes the maximum of counter-budget and byte-budget usage percentages.
 * Returns true when the projected values reach or exceed ROTATE_NOW_PCT.
 *
 * \param[in] projected_counter  Counter value after the planned write.
 * \param[in] projected_bytes    Auth bytes after the planned write.
 *
 * \retval true   Budget exhausted — write must be rejected.
 * \retval false  OK to proceed.
 */
static inline bool ubi_secure_budget_would_exhaust(uint64_t projected_counter,
						   uint64_t projected_bytes)
{
	const unsigned int counter_pct =
		ubi_secure_usage_pct(projected_counter, CONFIG_UBI_CRYPTO_LEB_WRITE_BUDGET);
	const unsigned int bytes_pct = ubi_secure_usage_pct(
		projected_bytes, CONFIG_UBI_CRYPTO_LEB_TOTAL_AUTH_BYTES_BUDGET);
	const unsigned int usage_pct = (counter_pct > bytes_pct) ? counter_pct : bytes_pct;

	return usage_pct >= CONFIG_UBI_CRYPTO_ROTATE_NOW_PCT;
}

/**
 * \brief Handle a write-path I/O error: emit RNG/KEY events and enforce
 *        strict read-only.
 *
 * Called after a write-path function (ec_hdr_write, vid_hdr_write,
 * leb_data_write) returns an error. Detects RNG and key-unavailable
 * failures by their sentinel error codes and emits the corresponding
 * crypto event. Sets read_only_crypto when the strict Kconfig policy
 * requires it.
 *
 * \param[in,out] ubi   UBI device (caller holds mutex).
 * \param[in]     ret   Error code from the I/O write function.
 * \param[in]     pnum  PEB index where the failure occurred.
 *
 * \retval 0    A crypto event was emitted for the recognized error code.
 * \retval ret  Unrecognized error code — no event emitted, original error returned.
 */
static inline int ubi_secure_handle_write_error(struct ubi_device *ubi, int ret, uint32_t pnum)
{
	__ASSERT_NO_MSG(ubi != NULL);

	switch (ret) {
	case -UBI_SECURE_ENORAND: {
		const struct ubi_crypto_event ev = {
			.type = UBI_CRYPTO_EVENT_RNG_FAILURE,
			.freshness = ubi_secure_freshness_snapshot(ubi),
			.rng = { .rng_errno = ret },
		};
		ubi_secure_emit_event(ubi, &ev);
#if defined(CONFIG_UBI_CRYPTO_STRICT_RO_ON_RNG_FAILURE)
		ubi->read_only_crypto = true;
#endif
		return 0;
	}
	case -UBI_SECURE_ENOKEY: {
		const struct ubi_crypto_event ev = {
			.type = UBI_CRYPTO_EVENT_KEY_VERSION_UNAVAILABLE,
			.freshness = ubi_secure_freshness_snapshot(ubi),
			.key = { .key_version =
					 ubi->crypto_cfg->policy.requested_write_key_version },
		};
		ubi_secure_emit_event(ubi, &ev);
		return 0;
	}
	default:
		return ret;
	}
}

/**
 * \brief Handle a read-path I/O error: emit AUTH_FAILURE, FORMAT_VIOLATION,
 *        or KEY_VERSION_UNAVAILABLE events.
 *
 * Called after a read-path function returns an error. Distinguishes AEAD
 * auth failures from key-unavailable and format failures by error code.
 *
 * \param[in,out] ubi     UBI device (caller holds mutex).
 * \param[in]     ret     Error code from the I/O read function.
 * \param[in]     pnum    PEB index where the failure occurred.
 * \param[in]     domain  Secure domain of the failing object.
 * \param[in]     kv      Key version from the on-flash prefix (0 if unknown).
 *
 * \retval 0    A crypto event was emitted for the recognized error code.
 * \retval ret  Unrecognized error code — no event emitted, original error returned.
 */
static inline int ubi_secure_handle_read_error(struct ubi_device *ubi, int ret, uint32_t pnum,
					       uint8_t domain, uint8_t kv)
{
	__ASSERT_NO_MSG(ubi != NULL);

	switch (ret) {
	case -UBI_SECURE_ENOKEY: {
		const struct ubi_crypto_event ev = {
			.type = UBI_CRYPTO_EVENT_KEY_VERSION_UNAVAILABLE,
			.freshness = ubi_secure_freshness_snapshot(ubi),
			.key = { .key_version = kv },
		};
		ubi_secure_emit_event(ubi, &ev);
		return 0;
	}
	case -EBADMSG: {
		const struct ubi_crypto_event ev = {
			.type = UBI_CRYPTO_EVENT_AUTH_FAILURE,
			.freshness = ubi_secure_freshness_snapshot(ubi),
			.auth = { .peb_index = pnum, .domain = domain },
		};
		ubi_secure_emit_event(ubi, &ev);
		return 0;
	}
	case -UBI_SECURE_EFORMAT: {
		const struct ubi_crypto_event ev = {
			.type = UBI_CRYPTO_EVENT_FORMAT_VIOLATION,
			.freshness = ubi_secure_freshness_snapshot(ubi),
			.auth = { .peb_index = pnum, .domain = domain },
		};
		ubi_secure_emit_event(ubi, &ev);
		return 0;
	}
	default:
		return ret;
	}
}

/**
 * \brief Check key version against the allowlist; emit event if rejected.
 *
 * Returns true if the key version is allowlisted, false otherwise. When
 * rejected, emits KEY_VERSION_NOT_ALLOWLISTED.
 *
 * \param[in,out] ubi   UBI device (caller holds mutex).
 * \param[in]     kv    Key version from on-flash prefix.
 * \param[in]     pnum  PEB index.
 *
 * \retval true   Key version is allowed.
 * \retval false  Key version rejected — event emitted.
 */
static inline bool ubi_secure_check_allowlist(struct ubi_device *ubi, uint8_t kv, uint32_t pnum)
{
	__ASSERT_NO_MSG(ubi != NULL);

	if (ubi_secure_kv_slot(ubi, kv) >= 0) {
		return true;
	}

	const struct ubi_crypto_event ev = {
		.type = UBI_CRYPTO_EVENT_KEY_VERSION_NOT_ALLOWLISTED,
		.freshness = ubi_secure_freshness_snapshot(ubi),
		.key = { .key_version = kv },
	};
	ubi_secure_emit_event(ubi, &ev);
	return false;
}

#endif /* UBI_SECURE_EVENT_H */
