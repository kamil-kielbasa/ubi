/**
 * \file    ubi_secure_crypto.h
 * \brief   PSA Crypto wrappers for HKDF key derivation and AEAD operations.
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard ------------------------------------------------------------------------------- */
#ifndef UBI_SECURE_CRYPTO_H
#define UBI_SECURE_CRYPTO_H

/* Include files ------------------------------------------------------------------------------- */
#include "ubi_secure_types.h"

#include <stddef.h>
#include <stdint.h>

/* Function declarations ----------------------------------------------------------------------- */

/**
 * \brief Derive a 16-byte child key via HKDF-SHA-256 from a PSA root key.
 *
 * Performs HKDF-Extract(salt="", IKM) → PRK, then HKDF-Expand(PRK, label, 16).
 *
 * \param[in]  root_key_id  PSA key identifier for IKM[v].
 * \param[in]  label        Label bytes for HKDF-Expand (normative).
 * \param      label_len    Length of label in bytes.
 * \param[out] child_key_id Receives the derived PSA key identifier.
 *
 * \retval 0       Success.
 * \retval -EIO    PSA key derivation failure.
 * \retval -ENOMEM PSA key allocation failure.
 */
int ubi_secure_derive_child_key(uint32_t root_key_id, const uint8_t *label, size_t label_len,
				uint32_t *child_key_id);

/**
 * \brief Destroy a PSA key previously created by ubi_secure_derive_child_key().
 *
 * \param key_id PSA key identifier to destroy.
 */
void ubi_secure_destroy_key(uint32_t key_id);

/**
 * \brief Build the normative HKDF label for a given domain.
 *
 * Format: "UBI" || 0x00 || domain_name || 0x00 || 0x01
 * For LEB domain: "UBI" || 0x00 || "LEB" || 0x00 || 0x01 || be32(volume_id)
 *
 * \param[in]  domain     Secure domain identifier.
 * \param      volume_id  Volume identifier (only used for LEB domain).
 * \param[out] label      Output buffer (must be at least 24 bytes).
 * \param      label_cap  Capacity of label buffer.
 * \param[out] label_len  Actual label length written.
 *
 * \retval 0       Success.
 * \retval -EINVAL Unknown domain.
 * \retval -ENOSPC Buffer too small.
 */
int ubi_secure_build_label(enum ubi_secure_domain domain, uint32_t volume_id, uint8_t *label,
			   size_t label_cap, size_t *label_len);

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
int ubi_secure_aead_encrypt(uint32_t key_id, const uint8_t nonce[UBI_SECURE_NONCE_SIZE],
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
int ubi_secure_aead_decrypt(uint32_t key_id, const uint8_t nonce[UBI_SECURE_NONCE_SIZE],
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

#endif /* UBI_SECURE_CRYPTO_H */
