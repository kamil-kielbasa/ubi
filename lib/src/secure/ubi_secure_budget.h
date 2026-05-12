/**
 * \file    ubi_secure_budget.h
 * \author  Kamil Kielbasa
 * \brief   Per-domain AEAD usage-budget enforcement.
 *
 * \details The secure backend bounds the lifetime of every HKDF child key
 *          with a write-budget.  Two budget kinds are tracked:
 *
 *          * Metadata: DEVICE_HEADER, VOLUME_HEADER, ERASE_COUNTER and
 *            VOLUME_IDENTIFIER, scoped per write-active key version.
 *            Every metadata write advances exactly one global counter and
 *            consumes one record of a domain-specific authenticated-byte
 *            size; the budget is shared across all instances of that
 *            domain.
 *          * LEB: scoped per {kv, vol_id, lnum}.  The caller maintains the
 *            counter and the authenticated-byte total persistently in the
 *            VID record; the budget tracks raw counter and bytes.
 *
 *          For both kinds, hard exhaustion (>= ROTATE_NOW_PCT) emits
 *          KEY_ROTATE_NOW, sets sticky read_only_crypto and forces the
 *          caller to bail out with -ENOSPC before any flash mutation.
 *          Soft crossing (>= ROTATE_SOON_PCT) emits KEY_ROTATE_SOON post-
 *          write and lets the operation complete.
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard -------------------------------------------------------------------------------- */

#ifndef UBI_SECURE_BUDGET_H
#define UBI_SECURE_BUDGET_H

/* Include files -------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_secure_types.h"

/* Standard library headers: */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations ------------------------------------------------------------------------- */

struct ubi_device;

/* Module interface function declarations ------------------------------------------------------- */

/**
 * \brief Capture per-domain metadata budget bases.
 *
 * Run once from the secure attach/format paths after counters have been
 * populated and before any new write under the active kv occurs.  When
 * \p rotation_happened is true the bases capture the current global
 * counter values so subsequent (current - base) yields invocations under
 * the freshly installed key version only.  When false the bases stay at
 * zero so the cumulative budget under the unchanged kv carries forward.
 *
 * \param[in,out] ubi               UBI device.
 * \param[in]     rotation_happened true if a new write-active kv was just installed.
 */
void ubi_secure_budget_bases_init(struct ubi_device *ubi, bool rotation_happened);

/**
 * \brief Pre-write metadata-domain budget check.
 *
 * Subtracts the per-domain base from \p projected_counter, multiplies by
 * the domain's per-record authenticated-byte size and compares against
 * the metadata Kconfig limits.  If either dimension would cross
 * ROTATE_NOW_PCT the function emits KEY_ROTATE_NOW, sets
 * ubi->read_only_crypto and returns -ENOSPC so the caller bails out
 * before mutating flash.
 *
 * \param[in,out] ubi                UBI device (caller holds mutex).
 * \param[in]     domain             One of DEVICE_HEADER, VOLUME_HEADER,
 *                                   ERASE_COUNTER, VOLUME_IDENTIFIER.
 * \param[in]     projected_counter  Raw global counter value the write
 *                                   would reach (e.g. next_*_counter + 1).
 * \param[in]     kv                 Write-active key version (used in event).
 * \param[in]     vol_id             Volume id (used in event; 0 if N/A).
 *
 * \retval 0       Budget OK; caller may proceed.
 * \retval -ENOSPC Hard threshold reached; event emitted, RO set.
 */
int ubi_secure_budget_metadata_pre(struct ubi_device *ubi, enum ubi_secure_domain domain,
				   uint64_t projected_counter, uint8_t kv, uint32_t vol_id);

/**
 * \brief Post-write metadata-domain budget check.
 *
 * Same projection as the pre-check, called after the write succeeded.
 * Emits KEY_ROTATE_SOON when usage crosses ROTATE_SOON_PCT and re-emits
 * KEY_ROTATE_NOW defensively if the post-write usage exceeds NOW.
 *
 * \param[in,out] ubi          UBI device (caller holds mutex).
 * \param[in]     domain       One of DEVICE_HEADER, VOLUME_HEADER,
 *                             ERASE_COUNTER, VOLUME_IDENTIFIER.
 * \param[in]     post_counter Raw global counter value reached after the
 *                             write completed (e.g. ubi->next_*_counter).
 * \param[in]     kv           Write-active key version (used in event).
 * \param[in]     vol_id       Volume id (used in event; 0 if N/A).
 */
void ubi_secure_budget_metadata_post(struct ubi_device *ubi, enum ubi_secure_domain domain,
				     uint64_t post_counter, uint8_t kv, uint32_t vol_id);

/**
 * \brief Pre-write LEB-domain budget check.
 *
 * Compares the projected post-write per-{kv, vol_id, lnum} counter and
 * authenticated-byte total against the LEB Kconfig limits.  Same
 * KEY_ROTATE_NOW + sticky-RO + -ENOSPC behaviour as the metadata
 * pre-check on hard exhaustion.
 *
 * \param[in,out] ubi                UBI device.
 * \param[in]     kv                 Write-active key version.
 * \param[in]     vol_id             Volume id.
 * \param[in]     projected_counter  Per-LEB counter value the write would reach.
 * \param[in]     projected_bytes    Per-LEB authenticated-byte total it would reach.
 *
 * \retval 0       Budget OK.
 * \retval -ENOSPC Hard threshold reached; event emitted, RO set.
 */
int ubi_secure_budget_leb_pre(struct ubi_device *ubi, uint8_t kv, uint32_t vol_id,
			      uint64_t projected_counter, uint64_t projected_bytes);

/**
 * \brief Post-write LEB-domain budget check.
 *
 * Emits KEY_ROTATE_SOON when usage crosses ROTATE_SOON_PCT.  Defensively
 * also re-emits KEY_ROTATE_NOW if the post-write usage exceeds NOW.
 *
 * \param[in,out] ubi          UBI device (caller holds mutex).
 * \param[in]     kv           Write-active key version.
 * \param[in]     vol_id       Volume id.
 * \param[in]     post_counter Per-LEB counter value reached after the write.
 * \param[in]     post_bytes   Per-LEB authenticated-byte total reached.
 */
void ubi_secure_budget_leb_post(struct ubi_device *ubi, uint8_t kv, uint32_t vol_id,
				uint64_t post_counter, uint64_t post_bytes);

#endif /* UBI_SECURE_BUDGET_H */
