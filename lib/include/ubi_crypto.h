/**
 * \file    ubi_crypto.h
 * \author  Kamil Kielbasa
 *
 * \brief   UBI secure backend public types and callback definitions.
 *
 * \details This header defines the types needed to configure a UBI device in
 *          secure (authenticated-encryption) mode. Plain-mode callers that pass
 *          crypto_cfg == NULL to ubi_device_init() need not include this header.
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard -------------------------------------------------------------------------------- */

#ifndef UBI_CRYPTO_H
#define UBI_CRYPTO_H

/* Include files -------------------------------------------------------------------------------- */

/* Standard library headers: */
#include <stddef.h>
#include <stdint.h>

#if defined(CONFIG_UBI_CRYPTO)
/* mbedTLS / PSA Crypto headers: */
#include <psa/crypto_types.h>
#else /* !CONFIG_UBI_CRYPTO */
/* Plain-mode public surface: applications building with CONFIG_UBI_CRYPTO=n
 * still include this header (for example to pass crypto_cfg=NULL to
 * ubi_device_init()) and must not be forced to depend on PSA.  The fallback
 * typedef matches the PSA Crypto API specification, where psa_key_id_t is a
 * 32-bit identifier. */
typedef uint32_t psa_key_id_t;
#endif /* CONFIG_UBI_CRYPTO */

/* Types and type definitions ------------------------------------------------------------------- */

/**
 * \defgroup ubi_crypto_types UBI Crypto Types
 * \brief Public types for the UBI secure backend.
 * \{
 */

/* Freshness descriptor ------------------------------------------------------------------------- */

/**
 * \brief Exported freshness descriptor.
 *
 * A (device_revision, global_sqnum) pair that the application stores in a
 * trusted, rollback-protected location (e.g. monotonic counter or secure
 * element). UBI calls the check_freshness / sync_freshness callbacks with
 * this structure.
 */
struct ubi_crypto_freshness {
	uint64_t device_revision; /*!< Monotonic device header revision. */
	uint64_t global_sqnum; /*!< Global sequence number at last commit. */
};

/* Verdicts ------------------------------------------------------------------------------------- */

/**
 * \brief Rollback-check verdict returned by the check_freshness callback.
 */
enum ubi_crypto_rollback_verdict {
	UBI_CRYPTO_ROLLBACK_ACCEPT = 0, /*!< Freshness acceptable — proceed with attach. */
	UBI_CRYPTO_ROLLBACK_REJECT = 1, /*!< Rollback detected — abort attach. */
};

/**
 * \brief Event-callback verdict returned by the event callback.
 */
enum ubi_crypto_event_verdict {
	UBI_CRYPTO_EVENT_CONTINUE = 0, /*!< Continue normal operation. */
	UBI_CRYPTO_EVENT_ENTER_READ_ONLY = 1, /*!< Enter read-only mode for the session. */
};

/* Event types ---------------------------------------------------------------------------------- */

/**
 * \brief Crypto event types emitted by the secure backend.
 *
 * The event callback receives one of these types together with an event-specific
 * payload in the ubi_crypto_event union.
 */
enum ubi_crypto_event_type {
	UBI_CRYPTO_EVENT_AUTH_FAILURE = 0, /*!< AEAD authentication failed on a
	                                                    PEB header or data record. */
	UBI_CRYPTO_EVENT_FORMAT_VIOLATION = 1, /*!< On-flash structure has valid auth
	                                                    tag but violates format rules. */
	UBI_CRYPTO_EVENT_KEY_VERSION_NOT_ALLOWLISTED = 2, /*!< Authenticated object carries a
	                                                       key version absent from the
	                                                       policy allowlist. */
	UBI_CRYPTO_EVENT_KEY_VERSION_UNAVAILABLE = 3, /*!< get_key_id callback failed —
	                                                    key material not accessible. */
	UBI_CRYPTO_EVENT_ROLLBACK_POLICY_MISMATCH = 4, /*!< On-flash freshness is behind the
	                                                    trusted reference (rollback). */
	UBI_CRYPTO_EVENT_FRESHNESS_SYNC_FAILURE = 5, /*!< sync_freshness callback returned
	                                                    a non-zero error code. */
	UBI_CRYPTO_EVENT_RNG_FAILURE = 6, /*!< Platform RNG could not produce
	                                                    a fresh salt for AEAD nonce. */
	UBI_CRYPTO_EVENT_KEY_ROTATE_SOON = 7, /*!< Usage crossed the soft threshold
	                                                    (ROTATE_SOON_PCT) — prepare a
	                                                    replacement key. */
	UBI_CRYPTO_EVENT_KEY_ROTATE_NOW = 8, /*!< Usage crossed the hard threshold
	                                                    (ROTATE_NOW_PCT) — writes may be
	                                                    rejected without a new key. */
	UBI_CRYPTO_EVENT_KEY_RETIRABLE = 9, /*!< All on-flash objects for this key
	                                                    version have been erased; the key
	                                                    can be safely destroyed. */
};

/**
 * \brief Crypto event payload.
 *
 * Carries the event type, the freshness snapshot at the time of the event,
 * and a tagged union with per-event-type details.
 */
struct ubi_crypto_event {
	enum ubi_crypto_event_type type; /*!< Event discriminator. */
	struct ubi_crypto_freshness freshness; /*!< Freshness at event time. */
	union {
		struct {
			uint32_t peb_index; /*!< PEB where auth failed. */
			uint8_t domain; /*!< Domain (EC/VID/LEB/reserved). */
		} auth; /*!< AUTH_FAILURE, FORMAT_VIOLATION. */
		struct {
			uint8_t key_version; /*!< Affected key version. */
		} key; /*!< KEY_VERSION_NOT_ALLOWLISTED,
		                                  KEY_VERSION_UNAVAILABLE. */
		struct {
			uint8_t key_version; /*!< Key version approaching limit. */
			uint32_t volume_id; /*!< Volume (for LEB domain). */
			uint8_t usage_pct; /*!< Usage percentage (0–100). */
		} rotation; /*!< KEY_ROTATE_SOON, KEY_ROTATE_NOW,
		                                  KEY_RETIRABLE. */
		struct {
			int sync_errno; /*!< errno from sync callback. */
		} sync; /*!< FRESHNESS_SYNC_FAILURE. */
		struct {
			int rng_errno; /*!< errno from PSA RNG. */
		} rng; /*!< RNG_FAILURE. */
		struct {
			uint8_t _reserved; /*!< Reserved for future use. */
		} rollback; /*!< ROLLBACK_POLICY_MISMATCH. */
	};
};

/* Policy --------------------------------------------------------------------------------------- */

/**
 * \brief Crypto policy for a secure UBI device.
 *
 * Controls which key versions are acceptable and which version to use for
 * new writes.
 */
struct ubi_crypto_policy {
	uint8_t requested_write_key_version; /*!< Key version for new writes. */
	const uint8_t *allowed_key_versions; /*!< Array of acceptable key versions. */
	size_t allowed_key_versions_len; /*!< Number of entries in the allowlist. */
};

/* Callback typedefs ---------------------------------------------------------------------------- */

/**
 * \brief Retrieve a PSA key identifier for a given key version.
 *
 * \param[in]  key_version  Key version to look up.
 * \param[out] key_id_out   PSA key identifier for the requested version.
 *
 * \retval 0       Success.
 * \retval -errno  Key version unavailable.
 */
typedef int (*ubi_crypto_get_key_id_cb_t)(uint8_t key_version, psa_key_id_t *key_id_out);

/**
 * \brief Check freshness at attach time.
 *
 * Called once after the secure backend selects the best authenticated reserved
 * generation, before any writes are enabled.
 *
 * \param[in] freshness  Current on-flash freshness descriptor.
 * \param[in] user_data  Opaque pointer from ubi_crypto_config.user_data.
 *
 * \return Verdict: accept or reject.
 */
typedef enum ubi_crypto_rollback_verdict (*ubi_crypto_check_freshness_cb_t)(
	const struct ubi_crypto_freshness *freshness, void *user_data);

/**
 * \brief Sync freshness after a commit.
 *
 * Called after each mutation commit (or every N mutations per
 * CONFIG_UBI_CRYPTO_FRESHNESS_SYNC_DELTA). The application should persist
 * the freshness descriptor in a rollback-protected store.
 *
 * \param[in] freshness  Updated on-flash freshness descriptor.
 * \param[in] user_data  Opaque pointer from ubi_crypto_config.user_data.
 *
 * \retval 0       Success.
 * \retval -errno  Sync failure (event emitted, optionally RO).
 */
typedef int (*ubi_crypto_sync_freshness_cb_t)(const struct ubi_crypto_freshness *freshness,
					      void *user_data);

/**
 * \brief Event callback for security-relevant notifications.
 *
 * \param[in] event     Event payload with type + details.
 * \param[in] user_data Opaque pointer from ubi_crypto_config.user_data.
 *
 * \return Verdict: continue or enter read-only.
 */
typedef enum ubi_crypto_event_verdict (*ubi_crypto_event_cb_t)(const struct ubi_crypto_event *event,
							       void *user_data);

/* Configuration -------------------------------------------------------------------------------- */

/**
 * \brief Crypto configuration for secure UBI device initialization.
 *
 * Passed as the crypto_cfg argument to ubi_device_init(). When non-NULL,
 * the secure backend is selected. All callback pointers must be non-NULL.
 */
struct ubi_crypto_config {
	struct ubi_crypto_policy policy; /*!< Key policy for this session. */

	ubi_crypto_get_key_id_cb_t get_key_id; /*!< Key-ID resolver. */
	ubi_crypto_check_freshness_cb_t check_freshness; /*!< Attach-time freshness check. */
	ubi_crypto_sync_freshness_cb_t sync_freshness; /*!< Post-commit freshness sync. */
	ubi_crypto_event_cb_t event_cb; /*!< Security event handler. */

	void *user_data; /*!< Opaque context passed to all callbacks. */
};

/** \} ubi_crypto_types */

/* Forward declarations ------------------------------------------------------------------------- */

struct ubi_device;

/* Public functions ----------------------------------------------------------------------------- */

/**
 * \brief Query the authenticated write-active key version of a secure UBI device.
 *
 * Returns the key version currently in force on flash for new write operations
 * (mirrors the authenticated \c write_active_key_version field of the device
 * header). The value is stable between key rotations and equals the key version
 * the secure backend will use for the next mutation.
 *
 * \param[in] ubi      UBI device handle (must be initialized in secure mode).
 * \param[out] out_kv  Receives the current write-active key version.
 *
 * \retval 0        Success.
 * \retval -EINVAL  NULL pointer.
 * \retval -ENOTSUP Device is not in secure mode.
 */
int ubi_secure_key_get_active_version(struct ubi_device *ubi, uint8_t *out_kv);

#endif /* UBI_CRYPTO_H */
