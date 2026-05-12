/**
 * \file    ubi_secure_policy.h
 * \author  Kamil Kielbasa
 * \brief   Crypto-policy helpers (allowlist lookup) shared across the secure
 *          backend.
 *
 * \details Header-only helpers that operate on \ref ubi_crypto_policy directly,
 *          so they can be used from sites that do not (yet) hold a
 *          \ref ubi_device handle (e.g. scan-time code, key derivation).
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard -------------------------------------------------------------------------------- */

#ifndef UBI_SECURE_POLICY_H
#define UBI_SECURE_POLICY_H

/* Include files -------------------------------------------------------------------------------- */

/* Public headers: */
#include <ubi_crypto.h>

/* Zephyr headers: */
#include <zephyr/sys/__assert.h>

/* Standard library headers: */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* Helpers -------------------------------------------------------------------------------------- */

/**
 * \brief Find the allowlist slot for a given key version.
 *
 * Single source of truth for the allowlist linear scan; replaces the inline
 * loops previously duplicated in derive_domain_key, derive_leb_key and
 * res_peb_scan.
 *
 * \param[in] policy Crypto policy (must not be NULL).
 * \param[in] kv     Key version to look up.
 *
 * \retval >= 0     Slot index (0 .. allowed_key_versions_len - 1).
 * \retval -ENOENT  Key version not found in the allowlist.
 */
static inline int ubi_secure_policy_kv_slot(const struct ubi_crypto_policy *policy, uint8_t kv)
{
	__ASSERT_NO_MSG(policy != NULL);

	for (size_t i = 0; i < policy->allowed_key_versions_len; i++) {
		if (policy->allowed_key_versions[i] == kv) {
			return (int)i;
		}
	}
	return -ENOENT;
}

#endif /* UBI_SECURE_POLICY_H */
