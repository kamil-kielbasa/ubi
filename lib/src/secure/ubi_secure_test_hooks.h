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

#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */
#endif /* UBI_SECURE_TEST_HOOKS_H */
