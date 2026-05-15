/**
 * \file    ubi_secure_test_hooks.h
 * \author  Kamil Kielbasa
 * \brief   Secure backend test hooks — fault injection for crypto operations.
 *
 * \details Provides controllable failure stages for secure backend testing.
 *          Only compiled when CONFIG_UBI_SECURE_TEST_FAULT_INJECTION is enabled.
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard -------------------------------------------------------------------------------- */

#ifndef UBI_SECURE_TEST_HOOKS_H
#define UBI_SECURE_TEST_HOOKS_H

#if defined(CONFIG_UBI_SECURE_TEST_FAULT_INJECTION)

#pragma GCC diagnostic push
#pragma GCC diagnostic warning "-Wcpp"
#warning "UBI secure test hooks enabled — do not use in production builds"
#pragma GCC diagnostic pop

/* Include files -------------------------------------------------------------------------------- */

/* Standard library headers: */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Types and type definitions ------------------------------------------------------------------- */

/**
 * \brief Secure fault injection stages.
 *
 * Each stage corresponds to a crypto operation that can be forced to fail
 * during testing. Use ubi_secure_test_hook_set() to arm a specific stage.
 */
enum ubi_secure_test_hook_stage {
	UBI_SECURE_HOOK_GET_KEY_ID_FAIL = 0, /*!< get_key_id callback returns error. */
	UBI_SECURE_HOOK_RNG_FAIL = 1, /*!< psa_generate_random() returns error. */
	UBI_SECURE_HOOK_AEAD_ENCRYPT_FAIL = 2, /*!< AEAD encrypt returns error. */
	UBI_SECURE_HOOK_AEAD_DECRYPT_FAIL = 3, /*!< AEAD decrypt returns error. */
	UBI_SECURE_HOOK_HKDF_FAIL = 4, /*!< HKDF key derivation fails. */
	UBI_SECURE_HOOK_FRESHNESS_REJECT = 5, /*!< check_freshness returns REJECT. */
	UBI_SECURE_HOOK_FRESHNESS_SYNC_FAIL = 6, /*!< sync_freshness returns error. */
	UBI_SECURE_HOOK_COUNT, /*!< Number of hook stages. */
};

/* Module interface function declarations ------------------------------------------------------- */

/**
 * \brief Reset all secure test hooks to their default (disabled) state.
 */
void ubi_secure_test_hook_reset(void);

/**
 * \brief Arm or disarm a specific hook stage.
 *
 * When armed, the next occurrence of the corresponding crypto operation
 * will fail. After triggering, the hook is automatically disarmed (one-shot).
 *
 * \param[in] stage  Hook stage to configure.
 * \param[in] armed  true to arm, false to disarm.
 */
void ubi_secure_test_hook_set(enum ubi_secure_test_hook_stage stage, bool armed);

/**
 * \brief Check and consume a hook trigger.
 *
 * Called by the secure backend at the corresponding operation. Returns true
 * if the hook was armed (and disarms it); returns false otherwise.
 *
 * \param[in] stage  Hook stage to check.
 *
 * \retval true   Hook was armed — operation should fail.
 * \retval false  Hook not armed — proceed normally.
 */
bool ubi_secure_test_hook_check(enum ubi_secure_test_hook_stage stage);

/* Forward declaration so we don't need to pull in ubi_internal.h. */
struct ubi_device;

/**
 * \brief Test-only override of the global metadata counters.
 *
 * Allows tests to drive any metadata-domain counter close to the
 * ROTATE_NOW threshold using only a handful of real flash operations,
 * which is necessary on geometries (e.g. native_sim) where the free-PEB
 * count is far smaller than CONFIG_UBI_SECURE_METADATA_COUNTER_BUDGET.
 *
 * Each parameter sets the corresponding `next_*_counter` field on the
 * UBI device.  Subsequent metadata writes will use these values as their
 * starting AEAD counter (the on-flash records remain self-consistent
 * because they embed the counter value used at write time).
 *
 * \param[in,out] ubi      UBI device handle.
 * \param[in]     dev_hdr  New value for next_res_peb_counter.
 * \param[in]     ec       New value for next_ec_counter.
 * \param[in]     vid      New value for next_vid_counter.
 */
void ubi_secure_test_set_metadata_counters(struct ubi_device *ubi, uint64_t dev_hdr, uint64_t ec,
					   uint64_t vid);

/**
 * \brief Test-only read of the global metadata counters.
 *
 * Mirrors \ref ubi_secure_test_set_metadata_counters; intended for tests
 * that need to verify counter state directly (e.g. that
 * `next_vid_counter` resets to 0 on key-version rotation).
 *
 * Any output pointer may be NULL to skip that field.
 *
 * \param[in]  ubi      UBI device handle.
 * \param[out] dev_hdr  Current value of next_res_peb_counter (or NULL).
 * \param[out] ec       Current value of next_ec_counter (or NULL).
 * \param[out] vid      Current value of next_vid_counter (or NULL).
 */
void ubi_secure_test_get_metadata_counters(const struct ubi_device *ubi, uint64_t *dev_hdr,
					   uint64_t *ec, uint64_t *vid);

/**
 * \brief Test-only override of the per-LEB write-counter floor.
 *
 * On its next mutation, every LEB read of the previous
 * `vid_meta.leb_write_counter` is clamped up to at least \p floor.
 * This lets tests drive the per-LEB AEAD counter close to the 48-bit
 * limit (`UBI_SECURE_COUNTER_MAX`) using a single real write, instead
 * of having to perform 2^48 chunk encryptions, which is the only way
 * to exercise the chunked-write overflow guard at
 * `leb_prepare_new_mapping()` end-to-end.
 *
 * Setting \p floor to 0 disables the override.
 *
 * \param[in] floor  Minimum value to substitute for the recovered
 *                   per-LEB write counter on the next mutation.
 */
void ubi_secure_test_set_leb_write_counter_floor(uint64_t floor);

/**
 * \brief Read the current per-LEB write-counter floor override.
 *
 * Used by the secure backend to clamp recovered counters; tests should
 * normally use \ref ubi_secure_test_set_leb_write_counter_floor instead.
 *
 * \return Current floor value (0 if the override is disabled).
 */
uint64_t ubi_secure_test_get_leb_write_counter_floor(void);

/**
 * \brief Test-only read of a volume's cached AEAD counter floor (RAM mirror).
 *
 * Exposes \ref ubi_volume::cached_leb_write_counter and \ref
 * ubi_volume::cached_leb_total_auth_bytes for tests verifying that the
 * per-volume counter cache survives unmap+erase, cold attach, and anchor
 * rewrite scenarios.  Returns -ENOENT if the volume is not found.
 *
 * Any output pointer may be NULL to skip that field.
 *
 * \param[in]  ubi               UBI device handle.
 * \param[in]  vol_id            Volume identifier.
 * \param[out] write_counter     Cached leb_write_counter (or NULL).
 * \param[out] total_auth_bytes  Cached leb_total_auth_bytes (or NULL).
 *
 * \retval 0        Cache values written to outputs.
 * \retval -ENOENT  Volume not found.
 * \retval -EINVAL  NULL device handle.
 */
int ubi_secure_test_get_volume_cached_counter(struct ubi_device *ubi, int vol_id,
					      uint64_t *write_counter, uint64_t *total_auth_bytes);

/**
 * \brief Test-only resolution of the live PEB index for a volume mapping.
 *
 * Returns the physical eraseblock currently mapped for the given
 * \p vol_id and \p lnum, or the hidden anchor PEB when \p lnum is
 * passed as \c SIZE_MAX.  Lets tests target a specific PEB for
 * \ref ubi_secure_test_read_vid_meta_from_peb without depending on the
 * internal layout of the EBA tree or the anchor field.
 *
 * \param[in]  ubi       UBI device handle.
 * \param[in]  vol_id    Volume identifier.
 * \param[in]  lnum      Logical eraseblock number, or \c SIZE_MAX to
 *                       request the volume's hidden anchor PEB.
 * \param[out] out_pnum  Resolved physical eraseblock index.
 *
 * \retval 0        Mapping resolved; \p *out_pnum is valid.
 * \retval -ENOENT  Volume not found, anchor not allocated, or LEB not mapped.
 * \retval -EINVAL  NULL device handle or NULL output pointer.
 */
int ubi_secure_test_get_peb_for_lnum(struct ubi_device *ubi, int vol_id, size_t lnum,
				     size_t *out_pnum);

/**
 * \brief Test-only authenticated read of VID secure metadata from any PEB.
 *
 * Drives the standard EC + VID read/authentication path against the
 * given physical eraseblock and surfaces the authenticated counter
 * fields needed by per-volume floor-continuity tests.  The PEB must
 * host a valid secure VID record (user LEB mapping or hidden anchor);
 * any authentication or I/O failure is propagated to the caller.
 *
 * Any output pointer may be NULL to skip that field.
 *
 * \param[in]  ubi               UBI device handle.
 * \param[in]  pnum              Physical eraseblock index.
 * \param[out] write_counter     Authenticated leb_write_counter (or NULL).
 * \param[out] total_auth_bytes  Authenticated leb_total_auth_bytes (or NULL).
 * \param[out] sqnum             Authenticated VID sequence number (or NULL).
 *
 * \retval 0         Success.
 * \retval -EIO      Flash or crypto failure.
 * \retval -EBADMSG  Authentication failure.
 * \retval -EINVAL   NULL device handle.
 */
int ubi_secure_test_read_vid_meta_from_peb(struct ubi_device *ubi, size_t pnum,
					   uint64_t *write_counter, uint64_t *total_auth_bytes,
					   uint64_t *sqnum);

#endif /* CONFIG_UBI_SECURE_TEST_FAULT_INJECTION */
#endif /* UBI_SECURE_TEST_HOOKS_H */
