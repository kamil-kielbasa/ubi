/**
 * \file    ubi_secure_budget.c
 * \author  Kamil Kielbasa
 * \brief   Per-domain AEAD usage-budget enforcement.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_secure_budget.h"
#include "ubi_secure_event.h"
#include "ubi_secure_types.h"
#include "ubi_internal.h"

/* Public headers: */
#include <ubi_crypto.h>

/* Zephyr headers: */
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>

/* Standard library headers: */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Module defines ------------------------------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/**
 * \brief Per-record authenticated-byte size (AAD + plaintext) for the
 *        DEVICE_HEADER metadata domain.
 *
 * Derived from the AAD and on-flash record-size macros so any change to
 * the wire format propagates automatically.  The value is BUILD_ASSERTed
 * below to pin it to the spec.
 */
#define DEV_AUTH_BYTES (UBI_SECURE_DEV_HDR_AAD_SIZE + UBI_SECURE_DEV_HDR_PLAINTEXT_SIZE)

/**
 * \brief Per-record authenticated-byte size (AAD + plaintext) for the
 *        VOLUME_HEADER metadata domain.
 *
 * Derived from the AAD and on-flash record-size macros so any change to
 * the wire format propagates automatically.  The value is BUILD_ASSERTed
 * below to pin it to the spec.
 */
#define VOL_AUTH_BYTES (UBI_SECURE_VOL_HDR_AAD_SIZE + UBI_SECURE_VOL_HDR_PLAINTEXT_SIZE)

/**
 * \brief Per-record authenticated-byte size (AAD + plaintext) for the
 *        ERASE_COUNTER metadata domain.
 */
#define EC_AUTH_BYTES (UBI_SECURE_EC_HDR_AAD_SIZE + UBI_SECURE_EC_PLAINTEXT_SIZE)

/**
 * \brief Per-record authenticated-byte size (AAD + plaintext) for the
 *        VOLUME_IDENTIFIER metadata domain.
 */
#define VID_AUTH_BYTES (UBI_SECURE_VID_HDR_AAD_SIZE + UBI_SECURE_VID_HDR_PLAINTEXT_SIZE)

BUILD_ASSERT(DEV_AUTH_BYTES == 92, "DEV per-record AAD+plaintext drift");
BUILD_ASSERT(VOL_AUTH_BYTES == 101, "VOL per-record AAD+plaintext drift");
BUILD_ASSERT(EC_AUTH_BYTES == 60, "EC per-record AAD+plaintext drift");
BUILD_ASSERT(VID_AUTH_BYTES == 101, "VID per-record AAD+plaintext drift");

/* Static function declarations ----------------------------------------------------------------- */

/**
 * \brief Resolve the per-metadata-domain budget base.
 *
 * Each metadata domain has its own base.  DEVICE_HEADER and VOLUME_HEADER
 * are tracked separately even though they share the on-flash AEAD counter
 * (next_dev_hdr_counter): they use distinct HKDF child keys and the
 * VOLUME_HEADER per-record AAD is larger, so the VOL bytes-budget fills
 * faster than the DEV one.
 *
 * \param[in] ubi    UBI device.
 * \param[in] domain One of the four metadata domains.
 *
 * \return RAM-only base counter for \p domain.
 */
static uint64_t metadata_domain_base(const struct ubi_device *ubi, enum ubi_secure_domain domain);

/**
 * \brief Per-record authenticated-byte size for a metadata domain.
 *
 * \param[in] domain One of the four metadata domains.
 *
 * \return DEV_AUTH_BYTES / VOL_AUTH_BYTES / EC_AUTH_BYTES / VID_AUTH_BYTES.
 */
static uint16_t metadata_auth_bytes_per_record(enum ubi_secure_domain domain);

/**
 * \brief Compute the maximum of counter-pct and bytes-pct.
 *
 * \param[in] counter        Current effective counter value.
 * \param[in] counter_budget Counter-dimension budget cap (must be > 0).
 * \param[in] bytes          Current effective authenticated-byte total.
 * \param[in] bytes_budget   Bytes-dimension budget cap (must be > 0).
 *
 * \return max(counter / counter_budget, bytes / bytes_budget) in percent.
 */
static uint8_t usage_pct(uint64_t counter, uint64_t counter_budget, uint64_t bytes,
			 uint64_t bytes_budget);

/**
 * \brief Emit KEY_ROTATE_NOW + set sticky read-only.
 *
 * \param[in,out] ubi    UBI device (caller holds mutex).
 * \param[in]     kv     Write-active key version (carried in event).
 * \param[in]     vol_id Volume id (carried in event; 0 if N/A).
 *
 * \retval -ENOSPC Always — caller must propagate.
 */
static int trip_now(struct ubi_device *ubi, uint8_t kv, uint32_t vol_id);

/**
 * \brief Emit a KEY_ROTATE_NOW event with the given usage percentage and latch
 *        the device into crypto-RO.  Shared body of \ref trip_now and
 *        \ref trip_overflow.
 *
 * \param[in,out] ubi    UBI device.
 * \param[in]     kv     Key version (carried in event).
 * \param[in]     vol_id Volume id (carried in event; 0 if N/A).
 * \param[in]     pct    Effective usage percentage to report in the event.
 */
static void emit_rotate_now(struct ubi_device *ubi, uint8_t kv, uint32_t vol_id, uint8_t pct);

/**
 * \brief Emit KEY_ROTATE_NOW (usage_pct = 100), latch crypto-RO, and return
 *        \c -EOVERFLOW.  Used when the budget pre-check observes that a
 *        projected AEAD counter would exceed \ref UBI_SECURE_COUNTER_MAX.
 *
 * \param[in,out] ubi    UBI device.
 * \param[in]     kv     Write-active key version (carried in event).
 * \param[in]     vol_id Volume id (carried in event; 0 if N/A).
 *
 * \retval -EOVERFLOW Always — caller must propagate.
 */
static int trip_overflow(struct ubi_device *ubi, uint8_t kv, uint32_t vol_id);

/**
 * \brief Emit KEY_ROTATE_SOON or KEY_ROTATE_NOW based on \p pct.
 *
 * \param[in,out] ubi    UBI device.
 * \param[in]     kv     Key version (carried in event).
 * \param[in]     vol_id Volume id (carried in event; 0 if N/A).
 * \param[in]     pct    Effective usage percentage (>= ROTATE_SOON_PCT).
 */
static void emit_post_event(struct ubi_device *ubi, uint8_t kv, uint32_t vol_id, uint8_t pct);

/**
 * \brief Translate raw global \p projected_counter into the effective
 *        (counter, bytes) pair evaluated by the metadata budget.
 *
 * Subtracts the per-domain base and multiplies by the per-record
 * authenticated-byte size for the domain.
 *
 * \param[in]  ubi               UBI device.
 * \param[in]  domain            One of the four metadata domains.
 * \param[in]  projected_counter Raw global counter value the write would reach.
 * \param[out] out_counter       Effective (counter - base).
 * \param[out] out_bytes         Effective bytes = out_counter × auth_bytes_per_record.
 */
static void metadata_effective(const struct ubi_device *ubi, enum ubi_secure_domain domain,
			       uint64_t projected_counter, uint64_t *out_counter,
			       uint64_t *out_bytes);

/**
 * \brief Validate that \p domain is one of the four metadata domains.
 *
 * Logs an error and returns false otherwise; callers in the public API
 * use this to reject invalid arguments at the system boundary instead of
 * relying on assertions that can be compiled out.
 *
 * \param[in] func   Calling function name (for diagnostics).
 * \param[in] domain Domain to validate.
 *
 * \retval true  \p domain is a metadata domain.
 * \retval false \p domain is invalid; LOG_ERR already emitted.
 */
static bool metadata_domain_valid(const char *func, enum ubi_secure_domain domain);

/* Static function definitions ------------------------------------------------------------------ */

static uint64_t metadata_domain_base(const struct ubi_device *ubi, enum ubi_secure_domain domain)
{
	__ASSERT_NO_MSG(ubi != NULL);

	switch (domain) {
	case UBI_SECURE_DOMAIN_DEVICE_HEADER:
		return ubi->budget_base_dev;
	case UBI_SECURE_DOMAIN_VOLUME_HEADER:
		return ubi->budget_base_vol;
	case UBI_SECURE_DOMAIN_ERASE_COUNTER:
		return ubi->budget_base_ec;
	case UBI_SECURE_DOMAIN_VOLUME_IDENTIFIER:
		return ubi->budget_base_vid;
	default:
		__ASSERT(false, "Invalid metadata budget domain %d", (int)domain);
		return 0;
	}
}

static uint16_t metadata_auth_bytes_per_record(enum ubi_secure_domain domain)
{
	/* Metadata records (DEV_HDR, VOL_HDR, EC_HDR, VID_HDR) have a constant
	 * authenticated-byte size per write, so the budget is expressed as
	 * (invocations * bytes_per_record).  The LEB domain has no fixed
	 * AUTH_BYTES constant: each LEB write authenticates AAD plus a
	 * payload of caller-chosen length, so the LEB-budget path projects
	 * leb_total_auth_bytes directly from the cached per-volume counter
	 * floor rather than multiplying invocations by a constant. */

	switch (domain) {
	case UBI_SECURE_DOMAIN_DEVICE_HEADER:
		return DEV_AUTH_BYTES;
	case UBI_SECURE_DOMAIN_VOLUME_HEADER:
		return VOL_AUTH_BYTES;
	case UBI_SECURE_DOMAIN_ERASE_COUNTER:
		return EC_AUTH_BYTES;
	case UBI_SECURE_DOMAIN_VOLUME_IDENTIFIER:
		return VID_AUTH_BYTES;
	default:
		__ASSERT(false, "Invalid metadata budget domain %d", (int)domain);
		return 0;
	}
}

static uint8_t usage_pct(uint64_t counter, uint64_t counter_budget, uint64_t bytes,
			 uint64_t bytes_budget)
{
	__ASSERT_NO_MSG(counter_budget > 0);
	__ASSERT_NO_MSG(bytes_budget > 0);

	const uint8_t counter_pct = ubi_secure_usage_pct(counter, counter_budget);
	const uint8_t bytes_pct = ubi_secure_usage_pct(bytes, bytes_budget);

	return (counter_pct > bytes_pct) ? counter_pct : bytes_pct;
}

static void emit_rotate_now(struct ubi_device *ubi, uint8_t kv, uint32_t vol_id, uint8_t pct)
{
	__ASSERT_NO_MSG(ubi != NULL);

	const struct ubi_crypto_event ev = {
		.type = UBI_CRYPTO_EVENT_KEY_ROTATE_NOW,
		.freshness = ubi_secure_freshness_get_snapshot(ubi),
		.rotation = { .key_version = kv, .volume_id = vol_id, .usage_pct = pct },
	};

	ubi_secure_event_emit(ubi, &ev);
	ubi->read_only_crypto = true;
}

static int trip_now(struct ubi_device *ubi, uint8_t kv, uint32_t vol_id)
{
	emit_rotate_now(ubi, kv, vol_id, (uint8_t)CONFIG_UBI_CRYPTO_ROTATE_NOW_PCT);
	return -ENOSPC;
}

/* Returns -EOVERFLOW after emitting KEY_ROTATE_NOW (usage_pct = 100) and
 * latching the device into crypto-RO.  Used by the budget pre-checks below to
 * trip the same hard-rotation behaviour as a regular budget exhaustion when a
 * write would push the 48-bit AEAD counter past UBI_SECURE_COUNTER_MAX.
 */
static int trip_overflow(struct ubi_device *ubi, uint8_t kv, uint32_t vol_id)
{
	emit_rotate_now(ubi, kv, vol_id, (uint8_t)UBI_SECURE_PERCENT_BASE);
	return -EOVERFLOW;
}

static void emit_post_event(struct ubi_device *ubi, uint8_t kv, uint32_t vol_id, uint8_t pct)
{
	__ASSERT_NO_MSG(ubi != NULL);

	const enum ubi_crypto_event_type type = (pct >= CONFIG_UBI_CRYPTO_ROTATE_NOW_PCT) ?
							UBI_CRYPTO_EVENT_KEY_ROTATE_NOW :
							UBI_CRYPTO_EVENT_KEY_ROTATE_SOON;

	const struct ubi_crypto_event ev = {
		.type = type,
		.freshness = ubi_secure_freshness_get_snapshot(ubi),
		.rotation = { .key_version = kv, .volume_id = vol_id, .usage_pct = pct },
	};

	ubi_secure_event_emit(ubi, &ev);
}

static void metadata_effective(const struct ubi_device *ubi, enum ubi_secure_domain domain,
			       uint64_t projected_counter, uint64_t *out_counter,
			       uint64_t *out_bytes)
{
	__ASSERT_NO_MSG(ubi != NULL);
	__ASSERT_NO_MSG(out_counter != NULL);
	__ASSERT_NO_MSG(out_bytes != NULL);

	const uint64_t base = metadata_domain_base(ubi, domain);
	const uint64_t effective = (projected_counter > base) ? (projected_counter - base) : 0;

	*out_counter = effective;
	*out_bytes = effective * (uint64_t)metadata_auth_bytes_per_record(domain);
}

static bool metadata_domain_valid(const char *func, enum ubi_secure_domain domain)
{
	if (domain == UBI_SECURE_DOMAIN_DEVICE_HEADER ||
	    domain == UBI_SECURE_DOMAIN_VOLUME_HEADER ||
	    domain == UBI_SECURE_DOMAIN_ERASE_COUNTER ||
	    domain == UBI_SECURE_DOMAIN_VOLUME_IDENTIFIER) {
		return true;
	}

	LOG_ERR("%s: invalid metadata domain %d", func, (int)domain);
	return false;
}

/* Module interface function definitions -------------------------------------------------------- */

void ubi_secure_budget_bases_init(struct ubi_device *ubi, bool rotation_happened)
{
	if (ubi == NULL) {
		LOG_ERR("budget_init_bases: NULL ubi");
		return;
	}

	if (rotation_happened) {
		/*
		 * New write-active kv just installed.  All future writes use
		 * new HKDF child keys so the per-domain budget restarts.
		 * Bases capture the current global counter values so
		 * subsequent (current - base) yields invocations under the
		 * new kv only.  DEV and VOL share next_dev_hdr_counter on
		 * flash but get separate base copies so the subtraction
		 * stays per-domain.
		 */
		ubi->budget_base_dev = ubi->next_dev_hdr_counter;
		ubi->budget_base_vol = ubi->next_dev_hdr_counter;
		ubi->budget_base_ec = ubi->next_ec_counter;
		ubi->budget_base_vid = ubi->next_vid_counter;
	} else {
		/*
		 * Same write-active kv as on flash.  The on-flash counter
		 * already represents cumulative invocations under this kv;
		 * keep the bases at zero so (current - 0) reflects the full
		 * history.
		 */
		ubi->budget_base_dev = 0;
		ubi->budget_base_vol = 0;
		ubi->budget_base_ec = 0;
		ubi->budget_base_vid = 0;
	}
}

int ubi_secure_budget_metadata_pre(struct ubi_device *ubi, enum ubi_secure_domain domain,
				   uint64_t projected_counter, uint8_t kv, uint32_t vol_id)
{
	if (ubi == NULL) {
		LOG_ERR("budget_metadata_pre: NULL ubi");
		return -EINVAL;
	}
	if (!metadata_domain_valid(__func__, domain)) {
		return -EINVAL;
	}

	if (ubi->crypto_cfg == NULL) {
		return 0;
	}

	if (projected_counter > UBI_SECURE_COUNTER_MAX) {
		LOG_ERR("Metadata AEAD counter overflow: domain=%d kv=%u vol_id=%u", (int)domain,
			(unsigned)kv, (unsigned)vol_id);
		return trip_overflow(ubi, kv, vol_id);
	}

	uint64_t counter = 0;
	uint64_t bytes = 0;

	metadata_effective(ubi, domain, projected_counter, &counter, &bytes);

	const uint8_t pct = usage_pct(counter, (uint64_t)CONFIG_UBI_CRYPTO_METADATA_COUNTER_BUDGET,
				      bytes,
				      (uint64_t)CONFIG_UBI_CRYPTO_METADATA_TOTAL_AUTH_BYTES_BUDGET);

	if (pct < CONFIG_UBI_CRYPTO_ROTATE_NOW_PCT) {
		return 0;
	}

	LOG_ERR("Metadata budget exhausted: domain=%d kv=%u pct=%u — entering crypto RO",
		(int)domain, (unsigned)kv, pct);
	return trip_now(ubi, kv, vol_id);
}

void ubi_secure_budget_metadata_post(struct ubi_device *ubi, enum ubi_secure_domain domain,
				     uint64_t post_counter, uint8_t kv, uint32_t vol_id)
{
	if (ubi == NULL) {
		LOG_ERR("budget_metadata_post: NULL ubi");
		return;
	}
	if (!metadata_domain_valid(__func__, domain)) {
		return;
	}

	if (ubi->crypto_cfg == NULL || ubi->crypto_cfg->event_cb == NULL) {
		return;
	}

	uint64_t counter = 0;
	uint64_t bytes = 0;

	metadata_effective(ubi, domain, post_counter, &counter, &bytes);

	const uint8_t pct = usage_pct(counter, (uint64_t)CONFIG_UBI_CRYPTO_METADATA_COUNTER_BUDGET,
				      bytes,
				      (uint64_t)CONFIG_UBI_CRYPTO_METADATA_TOTAL_AUTH_BYTES_BUDGET);

	if (pct < CONFIG_UBI_CRYPTO_ROTATE_SOON_PCT) {
		return;
	}

	emit_post_event(ubi, kv, vol_id, pct);
}

int ubi_secure_budget_leb_pre(struct ubi_device *ubi, uint8_t kv, uint32_t vol_id,
			      uint64_t projected_counter, uint64_t projected_bytes)
{
	if (ubi == NULL) {
		LOG_ERR("budget_leb_pre: NULL ubi");
		return -EINVAL;
	}

	if (ubi->crypto_cfg == NULL) {
		return 0;
	}

	if (projected_counter > UBI_SECURE_COUNTER_MAX) {
		LOG_ERR("LEB AEAD counter overflow: kv=%zu vol_id=%zu", (size_t)kv, (size_t)vol_id);
		return trip_overflow(ubi, kv, vol_id);
	}

	const uint8_t pct = usage_pct(projected_counter,
				      (uint64_t)CONFIG_UBI_CRYPTO_LEB_WRITE_BUDGET, projected_bytes,
				      (uint64_t)CONFIG_UBI_CRYPTO_LEB_TOTAL_AUTH_BYTES_BUDGET);

	if (pct < CONFIG_UBI_CRYPTO_ROTATE_NOW_PCT) {
		return 0;
	}

	LOG_ERR("LEB budget exhausted: kv=%u vol_id=%u pct=%u — entering crypto RO", (unsigned)kv,
		(unsigned)vol_id, pct);
	return trip_now(ubi, kv, vol_id);
}

void ubi_secure_budget_leb_post(struct ubi_device *ubi, uint8_t kv, uint32_t vol_id,
				uint64_t post_counter, uint64_t post_bytes)
{
	if (ubi == NULL) {
		LOG_ERR("budget_leb_post: NULL ubi");
		return;
	}

	if (ubi->crypto_cfg == NULL || ubi->crypto_cfg->event_cb == NULL) {
		return;
	}

	const uint8_t pct = usage_pct(post_counter, (uint64_t)CONFIG_UBI_CRYPTO_LEB_WRITE_BUDGET,
				      post_bytes,
				      (uint64_t)CONFIG_UBI_CRYPTO_LEB_TOTAL_AUTH_BYTES_BUDGET);

	if (pct < CONFIG_UBI_CRYPTO_ROTATE_SOON_PCT) {
		return;
	}

	emit_post_event(ubi, kv, vol_id, pct);
}
