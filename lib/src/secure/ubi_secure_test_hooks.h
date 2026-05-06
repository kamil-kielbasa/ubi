/**
 * \file    ubi_secure_test_hooks.h
 * \author  Kamil Kielbasa
 * \brief   Secure backend test hooks — fault injection for crypto operations.
 *
 * \details Provides controllable failure stages for secure backend testing.
 *          Only compiled when CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION is enabled.
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard -------------------------------------------------------------------------------- */

#ifndef UBI_SECURE_TEST_HOOKS_H
#define UBI_SECURE_TEST_HOOKS_H

#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)

#pragma GCC diagnostic push
#pragma GCC diagnostic warning "-Wcpp"
#warning "UBI secure test hooks enabled — do not use in production builds"
#pragma GCC diagnostic pop

/* Include files -------------------------------------------------------------------------------- */

/* Standard library headers: */
#include <stdbool.h>
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

/* Public API ----------------------------------------------------------------------------------- */

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
 * count is far smaller than CONFIG_UBI_CRYPTO_METADATA_COUNTER_BUDGET.
 *
 * Each parameter sets the corresponding `next_*_counter` field on the
 * UBI device.  Subsequent metadata writes will use these values as their
 * starting AEAD counter (the on-flash records remain self-consistent
 * because they embed the counter value used at write time).
 *
 * \param[in,out] ubi      UBI device handle.
 * \param[in]     dev_hdr  New value for next_dev_hdr_counter.
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
 * \param[out] dev_hdr  Current value of next_dev_hdr_counter (or NULL).
 * \param[out] ec       Current value of next_ec_counter (or NULL).
 * \param[out] vid      Current value of next_vid_counter (or NULL).
 */
void ubi_secure_test_get_metadata_counters(const struct ubi_device *ubi, uint64_t *dev_hdr,
					   uint64_t *ec, uint64_t *vid);

#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */
#endif /* UBI_SECURE_TEST_HOOKS_H */
