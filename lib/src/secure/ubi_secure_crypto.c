/**
 * \file    ubi_secure_crypto.c
 * \brief   PSA Crypto wrappers for HKDF key derivation and AEAD operations.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files ------------------------------------------------------------------------------- */
#include "ubi_secure_crypto.h"
#include "ubi_secure_types.h"

#include <psa/crypto.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/byteorder.h>

#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------ */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Domain label strings (normative — part of on-flash compatibility). */
static const char LABEL_PREFIX[] = "UBI";
static const char LABEL_DEVICE_HEADER[] = "DEVICE-HEADER";
static const char LABEL_VOLUME_HEADER[] = "VOLUME-HEADER";
static const char LABEL_ERASE_COUNTER[] = "ERASE-COUNTER";
static const char LABEL_VOLUME_IDENTIFIER[] = "VOLUME-IDENTIFIER";
static const char LABEL_LEB[] = "LEB";

/* Module interface function definitions ------------------------------------------------------- */

int ubi_secure_build_label(enum ubi_secure_domain domain, uint32_t volume_id, uint8_t *label,
			   size_t label_cap, size_t *label_len)
{
	__ASSERT_NO_MSG(label != NULL);
	__ASSERT_NO_MSG(label_len != NULL);

	const char *domain_name = NULL;
	size_t domain_name_len = 0;

	switch (domain) {
	case UBI_SECURE_DOMAIN_DEVICE_HEADER:
		domain_name = LABEL_DEVICE_HEADER;
		domain_name_len = sizeof(LABEL_DEVICE_HEADER) - 1;
		break;
	case UBI_SECURE_DOMAIN_VOLUME_HEADER:
		domain_name = LABEL_VOLUME_HEADER;
		domain_name_len = sizeof(LABEL_VOLUME_HEADER) - 1;
		break;
	case UBI_SECURE_DOMAIN_ERASE_COUNTER:
		domain_name = LABEL_ERASE_COUNTER;
		domain_name_len = sizeof(LABEL_ERASE_COUNTER) - 1;
		break;
	case UBI_SECURE_DOMAIN_VOLUME_IDENTIFIER:
		domain_name = LABEL_VOLUME_IDENTIFIER;
		domain_name_len = sizeof(LABEL_VOLUME_IDENTIFIER) - 1;
		break;
	case UBI_SECURE_DOMAIN_LEB:
		domain_name = LABEL_LEB;
		domain_name_len = sizeof(LABEL_LEB) - 1;
		break;
	default:
		LOG_ERR("Unknown secure domain: %d", (int)domain);
		return -EINVAL;
	}

	/* "UBI" || 0x00 || domain_name || 0x00 || 0x01 [|| be32(volume_id) for LEB] */
	size_t needed = sizeof(LABEL_PREFIX) /* includes NUL → acts as 0x00 separator */
			+ domain_name_len + 1 /* 0x00 */
			+ 1; /* 0x01 */

	if (domain == UBI_SECURE_DOMAIN_LEB) {
		needed += 4; /* be32(volume_id) */
	}

	if (needed > label_cap) {
		LOG_ERR("Label buffer too small: need %zu, have %zu", needed, label_cap);
		return -ENOSPC;
	}

	size_t pos = 0;

	/* "UBI" + 0x00 */
	memcpy(&label[pos], LABEL_PREFIX, sizeof(LABEL_PREFIX) - 1);
	pos += sizeof(LABEL_PREFIX) - 1;
	label[pos++] = 0x00;

	/* domain_name + 0x00 */
	memcpy(&label[pos], domain_name, domain_name_len);
	pos += domain_name_len;
	label[pos++] = 0x00;

	/* 0x01 */
	label[pos++] = 0x01;

	/* For LEB: be32(volume_id) */
	if (domain == UBI_SECURE_DOMAIN_LEB) {
		sys_put_be32(volume_id, &label[pos]);
		pos += 4;
	}

	*label_len = pos;
	return 0;
}

int ubi_secure_derive_child_key(uint32_t root_key_id, const uint8_t *label, size_t label_len,
				uint32_t *child_key_id)
{
	__ASSERT_NO_MSG(label != NULL);
	__ASSERT_NO_MSG(child_key_id != NULL);

	psa_status_t status = PSA_ERROR_GENERIC_ERROR;
	psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;

	status = psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	if (status != PSA_SUCCESS) {
		LOG_ERR("HKDF setup failed: %d", (int)status);
		goto abort;
	}

	/* Extract step: salt = "" (zero-length), IKM = root key. */
	status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, NULL, 0);
	if (status != PSA_SUCCESS) {
		LOG_ERR("HKDF salt input failed: %d", (int)status);
		goto abort;
	}

	status = psa_key_derivation_input_key(&op, PSA_KEY_DERIVATION_INPUT_SECRET, root_key_id);
	if (status != PSA_SUCCESS) {
		LOG_ERR("HKDF secret input failed: %d", (int)status);
		goto abort;
	}

	/* Expand step: info = label. */
	status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, label,
						label_len);
	if (status != PSA_SUCCESS) {
		LOG_ERR("HKDF info input failed: %d", (int)status);
		goto abort;
	}

	/* Output the child key as a volatile PSA key suitable for AES-128-CCM. */
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
	psa_set_key_algorithm(&attr, PSA_ALG_CCM);
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, UBI_SECURE_KEY_SIZE * 8);

	status = psa_key_derivation_output_key(&attr, &op, child_key_id);
	psa_reset_key_attributes(&attr);

	if (status != PSA_SUCCESS) {
		LOG_ERR("HKDF output key failed: %d", (int)status);
		goto abort;
	}

	psa_key_derivation_abort(&op);
	return 0;

abort:
	psa_key_derivation_abort(&op);
	return -EIO;
}

void ubi_secure_destroy_key(uint32_t key_id)
{
	(void)psa_destroy_key(key_id);
}

int ubi_secure_aead_encrypt(uint32_t key_id, const uint8_t nonce[UBI_SECURE_NONCE_SIZE],
			    const uint8_t *aad, size_t aad_len, const uint8_t *plaintext,
			    size_t plaintext_len, uint8_t *ciphertext, size_t ciphertext_cap,
			    size_t *ciphertext_len)
{
	__ASSERT_NO_MSG(nonce != NULL);
	__ASSERT_NO_MSG(ciphertext != NULL);
	__ASSERT_NO_MSG(ciphertext_len != NULL);

	const psa_status_t status = psa_aead_encrypt(key_id, PSA_ALG_CCM, nonce,
						     UBI_SECURE_NONCE_SIZE, aad, aad_len, plaintext,
						     plaintext_len, ciphertext, ciphertext_cap,
						     ciphertext_len);
	if (status != PSA_SUCCESS) {
		LOG_ERR("AEAD encrypt failed: %d", (int)status);
		return -EIO;
	}

	return 0;
}

int ubi_secure_aead_decrypt(uint32_t key_id, const uint8_t nonce[UBI_SECURE_NONCE_SIZE],
			    const uint8_t *aad, size_t aad_len, const uint8_t *ciphertext,
			    size_t ciphertext_len, uint8_t *plaintext, size_t plaintext_cap,
			    size_t *plaintext_len)
{
	__ASSERT_NO_MSG(nonce != NULL);
	__ASSERT_NO_MSG(plaintext != NULL);
	__ASSERT_NO_MSG(plaintext_len != NULL);

	const psa_status_t status = psa_aead_decrypt(key_id, PSA_ALG_CCM, nonce,
						     UBI_SECURE_NONCE_SIZE, aad, aad_len,
						     ciphertext, ciphertext_len, plaintext,
						     plaintext_cap, plaintext_len);
	if (status != PSA_SUCCESS) {
		LOG_ERR("AEAD decrypt failed: %d", (int)status);
		return -EIO;
	}

	return 0;
}

int ubi_secure_generate_salt(uint8_t salt[UBI_SECURE_SALT_SIZE])
{
	__ASSERT_NO_MSG(salt != NULL);

	const psa_status_t status = psa_generate_random(salt, UBI_SECURE_SALT_SIZE);

	if (status != PSA_SUCCESS) {
		LOG_ERR("RNG failure: %d", (int)status);
		return -EIO;
	}

	return 0;
}

void ubi_secure_build_nonce(uint8_t domain, const uint8_t salt[UBI_SECURE_SALT_SIZE],
			    const uint8_t counter[UBI_SECURE_COUNTER_SIZE],
			    uint8_t nonce[UBI_SECURE_NONCE_SIZE])
{
	__ASSERT_NO_MSG(salt != NULL);
	__ASSERT_NO_MSG(counter != NULL);
	__ASSERT_NO_MSG(nonce != NULL);

	nonce[0] = domain;
	memcpy(&nonce[1], salt, UBI_SECURE_SALT_SIZE);
	memcpy(&nonce[1 + UBI_SECURE_SALT_SIZE], counter, UBI_SECURE_COUNTER_SIZE);
}
