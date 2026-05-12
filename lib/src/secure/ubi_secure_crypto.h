/**
 * \file    ubi_secure_crypto.h
 * \author  Kamil Kielbasa
 * \brief   PSA Crypto wrappers for HKDF key derivation and AEAD operations.
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard -------------------------------------------------------------------------------- */

#ifndef UBI_SECURE_CRYPTO_H
#define UBI_SECURE_CRYPTO_H

/* Include files -------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_secure_types.h"

/* Public headers: */
#include <ubi_crypto.h>

/* Zephyr headers: */
#include <zephyr/sys/util.h>

/* Standard library headers: */
#include <stddef.h>
#include <stdint.h>

/* Defines -------------------------------------------------------------------------------------- */

/* Domain label strings (normative — part of on-flash compatibility).  Exposed
 * as string macros so the MAX_LABEL_SIZE computation below and the runtime
 * label-build code in ubi_secure_crypto.c share a single source of truth.
 */
#define UBI_SECURE_LABEL_PREFIX_STR "UBI"
#define UBI_SECURE_LABEL_DEVICE_HEADER_STR "DEVICE-HEADER"
#define UBI_SECURE_LABEL_VOLUME_HEADER_STR "VOLUME-HEADER"
#define UBI_SECURE_LABEL_ERASE_COUNTER_STR "ERASE-COUNTER"
#define UBI_SECURE_LABEL_VOLUME_IDENTIFIER_STR "VOLUME-IDENTIFIER"
#define UBI_SECURE_LABEL_LEB_STR "LEB"

/** Bytes contributed by the "UBI" prefix plus its trailing 0x00 separator. */
#define UBI_SECURE_LABEL_PREFIX_BYTES (sizeof(UBI_SECURE_LABEL_PREFIX_STR))

/** Bytes contributed by the 0x00 separator after the domain name. */
#define UBI_SECURE_LABEL_SEPARATOR_BYTES (1)

/** Bytes contributed by the 0x01 version byte. */
#define UBI_SECURE_LABEL_VERSION_BYTES (1)

/** Byte value of the label separator (NUL between fields). */
#define UBI_SECURE_LABEL_SEPARATOR_BYTE (0x00)

/** Byte value of the label version marker (v1). */
#define UBI_SECURE_LABEL_VERSION_BYTE (0x01)

/** Bytes contributed by the be32(volume_id) tail (LEB domain only). */
#define UBI_SECURE_LABEL_VOLUME_ID_BYTES (4)

/** Length in bytes of the longest domain name string used by build_label. */
#define UBI_SECURE_LABEL_DOMAIN_NAME_MAX                                    \
	MAX(sizeof(UBI_SECURE_LABEL_DEVICE_HEADER_STR) - 1,                 \
	    MAX(sizeof(UBI_SECURE_LABEL_VOLUME_HEADER_STR) - 1,             \
		MAX(sizeof(UBI_SECURE_LABEL_ERASE_COUNTER_STR) - 1,         \
		    MAX(sizeof(UBI_SECURE_LABEL_VOLUME_IDENTIFIER_STR) - 1, \
			sizeof(UBI_SECURE_LABEL_LEB_STR) - 1))))

/**
 * Maximum label buffer size for the HKDF-Expand label built internally by
 * the secure-domain key-derivation path.
 *
 * Derived from the actual label format
 * ("UBI" || 0x00 || domain_name || 0x00 || 0x01 [|| be32(volume_id)])
 * so adding a domain or renaming one cannot silently outgrow the buffer.
 */
#define UBI_SECURE_MAX_LABEL_SIZE                                            \
	(UBI_SECURE_LABEL_PREFIX_BYTES + UBI_SECURE_LABEL_DOMAIN_NAME_MAX +  \
	 UBI_SECURE_LABEL_SEPARATOR_BYTES + UBI_SECURE_LABEL_VERSION_BYTES + \
	 UBI_SECURE_LABEL_VOLUME_ID_BYTES)

/* Module interface function declarations ------------------------------------------------------- */

/**
 * \brief Destroy a PSA key previously created by ubi_secure_derive_domain_key()
 *        / ubi_secure_derive_leb_key().
 *
 * \param key_id PSA key identifier to destroy.
 */
void ubi_secure_destroy_key(psa_key_id_t key_id);

/**
 * \brief AEAD-encrypt (AES-128-CCM) with the given child key.
 *
 * \param[in]  key_id          PSA key for encryption.
 * \param[in]  nonce           13-byte nonce.
 * \param[in]  aad             Additional authenticated data.
 * \param      aad_len         AAD length.
 * \param[in]  plaintext       Data to encrypt.
 * \param      plaintext_len   Plaintext length.
 * \param[out] ciphertext      Output buffer (plaintext_len + 16 bytes for tag).
 * \param      ciphertext_cap  Capacity of output buffer.
 * \param[out] ciphertext_len  Actual output length.
 *
 * \retval 0    Success.
 * \retval -EIO AEAD operation failed.
 */
int ubi_secure_aead_encrypt(psa_key_id_t key_id, const uint8_t nonce[UBI_SECURE_NONCE_SIZE],
			    const uint8_t *aad, size_t aad_len, const uint8_t *plaintext,
			    size_t plaintext_len, uint8_t *ciphertext, size_t ciphertext_cap,
			    size_t *ciphertext_len);

/**
 * \brief AEAD-decrypt (AES-128-CCM) with the given child key.
 *
 * \param[in]  key_id          PSA key for decryption.
 * \param[in]  nonce           13-byte nonce.
 * \param[in]  aad             Additional authenticated data.
 * \param      aad_len         AAD length.
 * \param[in]  ciphertext      Data to decrypt (includes tag).
 * \param      ciphertext_len  Ciphertext length (payload + 16).
 * \param[out] plaintext       Output buffer.
 * \param      plaintext_cap   Capacity of output buffer.
 * \param[out] plaintext_len   Actual output length.
 *
 * \retval 0    Success (authentication verified).
 * \retval -EIO AEAD auth failure or decryption error.
 */
int ubi_secure_aead_decrypt(psa_key_id_t key_id, const uint8_t nonce[UBI_SECURE_NONCE_SIZE],
			    const uint8_t *aad, size_t aad_len, const uint8_t *ciphertext,
			    size_t ciphertext_len, uint8_t *plaintext, size_t plaintext_cap,
			    size_t *plaintext_len);

/**
 * \brief Generate a fresh random salt.
 *
 * \param[out] salt  Output buffer for 6-byte salt.
 *
 * \retval 0    Success.
 * \retval -EIO RNG failure.
 */
int ubi_secure_generate_salt(uint8_t salt[UBI_SECURE_SALT_SIZE]);

/**
 * \brief Build a 13-byte CCM nonce from domain, salt, and counter.
 *
 * nonce = domain(1) || salt(6) || counter(6)
 *
 * \param[in]  domain   Domain byte.
 * \param[in]  salt     6-byte salt.
 * \param[in]  counter  6-byte counter (big-endian).
 * \param[out] nonce    13-byte output nonce.
 */
void ubi_secure_build_nonce(uint8_t domain, const uint8_t salt[UBI_SECURE_SALT_SIZE],
			    const uint8_t counter[UBI_SECURE_COUNTER_SIZE],
			    uint8_t nonce[UBI_SECURE_NONCE_SIZE]);

/**
 * \brief Derive a child key for a non-LEB domain (DEVICE_HEADER, VOLUME_HEADER, etc.).
 *
 * Performs: get_key_id(key_version) → root, build_label(domain, 0) → label,
 *          derive_child_key(root, label) → child_key_id.
 *
 * \param[in]  crypto_cfg   Crypto configuration (for get_key_id callback).
 * \param      domain       Secure domain identifier (must not be LEB).
 * \param      key_version  Key version for root key lookup.
 * \param[out] child_key_id Receives the derived PSA key identifier.
 *
 * \retval 0    Success.
 * \retval -EIO Key derivation failure.
 */
int ubi_secure_derive_domain_key(const struct ubi_crypto_config *crypto_cfg,
				 enum ubi_secure_domain domain, uint8_t key_version,
				 psa_key_id_t *child_key_id);

/**
 * \brief Derive a child key for the LEB domain (keyed per volume_id).
 *
 * Performs: get_key_id(key_version) → root, build_label(LEB, volume_id) → label,
 *          derive_child_key(root, label) → child_key_id.
 *
 * \param[in]  crypto_cfg   Crypto configuration (for get_key_id callback).
 * \param      key_version  Key version for root key lookup.
 * \param      volume_id    Volume identifier for per-volume keying.
 * \param[out] child_key_id Receives the derived PSA key identifier.
 *
 * \retval 0    Success.
 * \retval -EIO Key derivation failure.
 */
int ubi_secure_derive_leb_key(const struct ubi_crypto_config *crypto_cfg, uint8_t key_version,
			      uint32_t volume_id, psa_key_id_t *child_key_id);

#endif /* UBI_SECURE_CRYPTO_H */
