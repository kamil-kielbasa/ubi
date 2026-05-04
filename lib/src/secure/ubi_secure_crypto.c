/**
 * \file    ubi_secure_crypto.c
 * \author  Kamil Kielbasa
 * \brief   PSA Crypto wrappers for HKDF key derivation and AEAD operations.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_secure_crypto.h"
#include "ubi_secure_test_hooks.h"
#include "ubi_secure_types.h"

/* Third-party headers: */
#include <psa/crypto.h>

/* Zephyr headers: */
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/byteorder.h>

/* Standard library headers: */
#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Domain label strings (normative — part of on-flash compatibility). */
static const char LABEL_PREFIX[] = "UBI";
static const char LABEL_DEVICE_HEADER[] = "DEVICE-HEADER";
static const char LABEL_VOLUME_HEADER[] = "VOLUME-HEADER";
static const char LABEL_ERASE_COUNTER[] = "ERASE-COUNTER";
static const char LABEL_VOLUME_IDENTIFIER[] = "VOLUME-IDENTIFIER";
static const char LABEL_LEB[] = "LEB";

/* Module interface function definitions -------------------------------------------------------- */

int ubi_secure_build_label(enum ubi_secure_domain domain, uint32_t volume_id, uint8_t *label,
			   size_t label_cap, size_t *label_len)
{
	if (label == NULL || label_len == NULL) {
		LOG_ERR("build_label: NULL argument");
		return -EINVAL;
	}

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
	if (label == NULL || child_key_id == NULL) {
		LOG_ERR("derive_child_key: NULL argument");
		return -EINVAL;
	}

	psa_status_t status = PSA_ERROR_GENERIC_ERROR;
	psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;

#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)
	if (ubi_secure_test_hook_check(UBI_SECURE_HOOK_HKDF_FAIL)) {
		LOG_WRN("HKDF fault injected");
		return -EIO;
	}
#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */

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
	LOG_ERR("Key derivation aborted");
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
	if (nonce == NULL || ciphertext == NULL || ciphertext_len == NULL) {
		LOG_ERR("aead_encrypt: NULL argument");
		return -EINVAL;
	}

#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)
	if (ubi_secure_test_hook_check(UBI_SECURE_HOOK_AEAD_ENCRYPT_FAIL)) {
		LOG_WRN("AEAD encrypt fault injected");
		return -EIO;
	}
#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */

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
	if (nonce == NULL || plaintext == NULL || plaintext_len == NULL) {
		LOG_ERR("aead_decrypt: NULL argument");
		return -EINVAL;
	}

#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)
	if (ubi_secure_test_hook_check(UBI_SECURE_HOOK_AEAD_DECRYPT_FAIL)) {
		LOG_WRN("AEAD decrypt fault injected");
		return -EIO;
	}
#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */

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
	if (salt == NULL) {
		LOG_ERR("generate_salt: NULL argument");
		return -EINVAL;
	}

#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)
	if (ubi_secure_test_hook_check(UBI_SECURE_HOOK_RNG_FAIL)) {
		LOG_WRN("RNG fault injected");
		return -UBI_SECURE_ENORAND;
	}
#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */

	const psa_status_t status = psa_generate_random(salt, UBI_SECURE_SALT_SIZE);

	if (status != PSA_SUCCESS) {
		LOG_ERR("RNG failure: %d", (int)status);
		return -UBI_SECURE_ENORAND;
	}

	return 0;
}

void ubi_secure_build_nonce(uint8_t domain, const uint8_t salt[UBI_SECURE_SALT_SIZE],
			    const uint8_t counter[UBI_SECURE_COUNTER_SIZE],
			    uint8_t nonce[UBI_SECURE_NONCE_SIZE])
{
	if (salt == NULL || counter == NULL || nonce == NULL) {
		LOG_ERR("build_nonce: NULL argument");
		return;
	}

	nonce[0] = domain;
	memcpy(&nonce[1], salt, UBI_SECURE_SALT_SIZE);
	memcpy(&nonce[1 + UBI_SECURE_SALT_SIZE], counter, UBI_SECURE_COUNTER_SIZE);
}

int ubi_secure_derive_domain_key(const struct ubi_crypto_config *crypto_cfg,
				 enum ubi_secure_domain domain, uint8_t key_version,
				 uint32_t *child_key_id)
{
	if (crypto_cfg == NULL || child_key_id == NULL) {
		LOG_ERR("derive_domain_key: NULL argument");
		return -EINVAL;
	}

	/* Central allowlist gate — every key derivation must pass through here. */
	bool kv_allowed = false;

	for (size_t i = 0; i < crypto_cfg->policy.allowed_key_versions_len; i++) {
		if (crypto_cfg->policy.allowed_key_versions[i] == key_version) {
			kv_allowed = true;
			break;
		}
	}

	if (!kv_allowed) {
		LOG_ERR("Key version %u not in allowlist (domain %d)", key_version, (int)domain);
		return -UBI_SECURE_ENOKEY;
	}

#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)
	if (ubi_secure_test_hook_check(UBI_SECURE_HOOK_GET_KEY_ID_FAIL)) {
		LOG_WRN("get_key_id fault injected");
		return -UBI_SECURE_ENOKEY;
	}
#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */

	uint32_t root_key_id = 0;
	int ret = crypto_cfg->get_key_id(key_version, &root_key_id);

	if (ret != 0) {
		LOG_ERR("get_key_id failed for version %u: %d", key_version, ret);
		return -UBI_SECURE_ENOKEY;
	}

	uint8_t label[UBI_SECURE_MAX_LABEL_SIZE] = { 0 };
	size_t label_len = 0;

	ret = ubi_secure_build_label(domain, 0, label, sizeof(label), &label_len);
	if (ret != 0) {
		LOG_ERR("build_label failed for domain %d: %d", (int)domain, ret);
		return ret;
	}

	ret = ubi_secure_derive_child_key(root_key_id, label, label_len, child_key_id);
	if (ret != 0) {
		LOG_ERR("derive_child_key failed for domain %d: %d", (int)domain, ret);
	}

	return ret;
}

int ubi_secure_derive_leb_key(const struct ubi_crypto_config *crypto_cfg, uint8_t key_version,
			      uint32_t volume_id, uint32_t *child_key_id)
{
	if (crypto_cfg == NULL || child_key_id == NULL) {
		LOG_ERR("derive_leb_key: NULL argument");
		return -EINVAL;
	}

	/* Central allowlist gate — every key derivation must pass through here. */
	bool kv_allowed = false;

	for (size_t i = 0; i < crypto_cfg->policy.allowed_key_versions_len; i++) {
		if (crypto_cfg->policy.allowed_key_versions[i] == key_version) {
			kv_allowed = true;
			break;
		}
	}

	if (!kv_allowed) {
		LOG_ERR("Key version %u not in allowlist (LEB domain, vol %u)", key_version,
			volume_id);
		return -UBI_SECURE_ENOKEY;
	}

#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)
	if (ubi_secure_test_hook_check(UBI_SECURE_HOOK_GET_KEY_ID_FAIL)) {
		LOG_WRN("get_key_id fault injected");
		return -UBI_SECURE_ENOKEY;
	}
#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */

	uint32_t root_key_id = 0;
	int ret = crypto_cfg->get_key_id(key_version, &root_key_id);

	if (ret != 0) {
		LOG_ERR("get_key_id failed for version %u: %d", key_version, ret);
		return -UBI_SECURE_ENOKEY;
	}

	uint8_t label[UBI_SECURE_MAX_LABEL_SIZE] = { 0 };
	size_t label_len = 0;

	ret = ubi_secure_build_label(UBI_SECURE_DOMAIN_LEB, volume_id, label, sizeof(label),
				     &label_len);
	if (ret != 0) {
		LOG_ERR("build_label failed for LEB domain vol %u: %d", volume_id, ret);
		return ret;
	}

	ret = ubi_secure_derive_child_key(root_key_id, label, label_len, child_key_id);
	if (ret != 0) {
		LOG_ERR("derive_child_key failed for LEB domain vol %u: %d", volume_id, ret);
	}

	return ret;
}
