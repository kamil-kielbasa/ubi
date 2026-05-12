/**
 * \file    ubi_secure_reserved.c
 * \author  Kamil Kielbasa
 * \brief   Secure reserved-PEB management: scan, authenticate, write.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_secure_reserved.h"
#include "ubi_secure_crypto.h"
#include "ubi_secure_flash.h"
#include "ubi_secure_policy.h"
#include "ubi_secure_ser.h"
#include "ubi_secure_types.h"
#include "ubi_internal.h"
#include "ubi_plain_io.h"

/* Zephyr headers: */
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/byteorder.h>

/* mbedTLS / PSA Crypto headers: */
#include <psa/crypto.h>

/* Standard library headers: */
#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Static function declarations ----------------------------------------------------------------- */

/**
 * \brief Authenticate one reserved-PEB device-header record.
 *
 * Deserializes the wrapper prefix, verifies the AEAD tag using the parent key,
 * and on success writes the plaintext device header and secure metadata into
 * the caller-provided output structs.
 *
 * \param[in] raw          Full record buffer (prefix + ciphertext + tag).
 * \param peb_idx          Physical eraseblock index (used as part of nonce).
 * \param flash_offset     Byte offset of the record on flash (nonce input).
 * \param child_key_id     Identifier of the AEAD child key to use.
 * \param[out] dev_hdr     Decoded device header on success.
 * \param[out] dev_meta    Decoded device secure metadata on success.
 * \param[out] prefix      Decoded wrapper prefix on success.
 *
 * \return 0 on success, or negative errno (-EBADMSG on auth failure, -EIO on
 *         crypto/I/O error).
 */
static int authenticate_dev_hdr(const uint8_t *raw, size_t peb_idx, uint64_t flash_offset,
				psa_key_id_t child_key_id, struct ubi_dev_hdr *dev_hdr,
				struct ubi_dev_secure_meta *dev_meta,
				struct ubi_crypto_prefix32 *prefix);

/**
 * \brief Encrypt and serialize one reserved-PEB device-header record.
 *
 * Builds the wrapper prefix, then AEAD-encrypts the concatenated
 * (device header || device secure metadata) plaintext into out_buf.
 *
 * \param[in] dev_hdr      Device header to encrypt.
 * \param[in] dev_meta     Device secure metadata to encrypt alongside.
 * \param child_key_id     Identifier of the AEAD child key to use.
 * \param key_version      Key version stamped into the prefix.
 * \param counter          Per-domain AEAD counter (nonce input).
 * \param peb_idx          Physical eraseblock index (nonce input).
 * \param flash_offset     Byte offset of the record on flash (nonce input).
 * \param[out] out_buf     Serialized record buffer (prefix + ciphertext + tag).
 *
 * \return 0 on success, or negative errno on failure.
 */
static int encrypt_dev_hdr(const struct ubi_dev_hdr *dev_hdr,
			   const struct ubi_dev_secure_meta *dev_meta, psa_key_id_t child_key_id,
			   uint8_t key_version, uint64_t counter, size_t peb_idx,
			   uint64_t flash_offset, uint8_t *out_buf);

/**
 * \brief Encrypt and serialize one reserved-PEB volume-header record.
 *
 * \param[in] vol_hdr      Volume header to encrypt.
 * \param child_key_id     Identifier of the AEAD child key to use.
 * \param key_version      Key version stamped into the prefix.
 * \param counter          Per-domain AEAD counter (nonce input).
 * \param peb_idx          Physical eraseblock index (nonce input).
 * \param flash_offset     Byte offset of the record on flash (nonce input).
 * \param device_revision  Device revision bound into the AAD for freshness.
 * \param parent_kv        Parent key version bound into the AAD.
 * \param[out] out_buf     Serialized record buffer (prefix + ciphertext + tag).
 *
 * \return 0 on success, or negative errno on failure.
 */
static int encrypt_vol_hdr(const struct ubi_vol_hdr *vol_hdr, psa_key_id_t child_key_id,
			   uint8_t key_version, uint64_t counter, size_t peb_idx,
			   uint64_t flash_offset, uint64_t device_revision, uint8_t parent_kv,
			   uint8_t *out_buf);

/* Static function definitions ------------------------------------------------------------------ */

static int authenticate_dev_hdr(const uint8_t *raw, size_t peb_idx, uint64_t flash_offset,
				psa_key_id_t child_key_id, struct ubi_dev_hdr *dev_hdr,
				struct ubi_dev_secure_meta *dev_meta,
				struct ubi_crypto_prefix32 *prefix)
{
	__ASSERT_NO_MSG(raw != NULL);
	__ASSERT_NO_MSG(dev_hdr != NULL);
	__ASSERT_NO_MSG(dev_meta != NULL);
	__ASSERT_NO_MSG(prefix != NULL);

	/* Deserialize prefix. */
	ubi_secure_prefix32_deserialize(raw, prefix);

	if (prefix->magic != UBI_SECURE_PREFIX_MAGIC) {
		LOG_ERR("Bad magic in device header prefix");
		return -EBADMSG;
	}

	if (prefix->wrapper_version != UBI_SECURE_WRAPPER_VERSION) {
		LOG_ERR("Unsupported wrapper_version %u in device header (expected %u)",
			prefix->wrapper_version, UBI_SECURE_WRAPPER_VERSION);
		return -EBADMSG;
	}

	if (prefix->domain != UBI_SECURE_DOMAIN_DEVICE_HEADER) {
		LOG_ERR("Unexpected domain in device header: %u", prefix->domain);
		return -EBADMSG;
	}

	/* Build nonce from prefix fields. */
	uint8_t nonce[UBI_SECURE_NONCE_SIZE] = { 0 };

	ubi_secure_build_nonce(prefix->domain, prefix->salt, prefix->counter, nonce);

	/* Build AAD. */
	uint8_t prefix_bytes[UBI_SECURE_PREFIX_SIZE] = { 0 };

	memcpy(prefix_bytes, raw, UBI_SECURE_PREFIX_SIZE);

	uint8_t aad[UBI_SECURE_DEV_HDR_AAD_SIZE] = { 0 };
	const struct ubi_secure_dev_hdr_aad_input aad_input = {
		.prefix = prefix_bytes,
		.peb_index = (uint32_t)peb_idx,
		.flash_offset = flash_offset,
	};

	ubi_secure_build_dev_hdr_aad(&aad_input, aad);

	/* Decrypt ciphertext+tag. */
	const uint8_t *ct = &raw[UBI_SECURE_PREFIX_SIZE];
	uint8_t plaintext[UBI_SECURE_DEV_HDR_PLAINTEXT_SIZE] = { 0 };
	size_t pt_len = 0;

	const int ret = ubi_secure_aead_decrypt(child_key_id, nonce, aad, sizeof(aad), ct,
						UBI_SECURE_DEV_HDR_CT_TAG_SIZE, plaintext,
						sizeof(plaintext), &pt_len);
	if (ret != 0) {
		LOG_ERR("AEAD decrypt failed for dev hdr at PEB %zu", peb_idx);
		return ret;
	}

	if (pt_len != UBI_SECURE_DEV_HDR_PLAINTEXT_SIZE) {
		LOG_ERR("Unexpected plaintext size: %zu", pt_len);
		return -EBADMSG;
	}

	/* Extract dev_hdr and dev_secure_meta from plaintext. */
	memcpy(dev_hdr, plaintext, UBI_DEV_HDR_SIZE);
	ubi_secure_dev_meta_deserialize(&plaintext[UBI_DEV_HDR_SIZE], dev_meta);

	return 0;
}

static int encrypt_dev_hdr(const struct ubi_dev_hdr *dev_hdr,
			   const struct ubi_dev_secure_meta *dev_meta, psa_key_id_t child_key_id,
			   uint8_t key_version, uint64_t counter, size_t peb_idx,
			   uint64_t flash_offset, uint8_t *out_buf)
{
	__ASSERT_NO_MSG(dev_hdr != NULL);
	__ASSERT_NO_MSG(dev_meta != NULL);
	__ASSERT_NO_MSG(out_buf != NULL);

	/* Build prefix. */
	struct ubi_crypto_prefix32 prefix = {
		.magic = UBI_SECURE_PREFIX_MAGIC,
		.wrapper_version = UBI_SECURE_WRAPPER_VERSION,
		.domain = UBI_SECURE_DOMAIN_DEVICE_HEADER,
		.key_version = key_version,
		.flags = 0,
	};

	int ret = ubi_secure_generate_salt(prefix.salt);

	if (ret != 0) {
		LOG_ERR("Salt generation failed for dev hdr");
		return ret;
	}

	ubi_secure_encode_counter48(counter, prefix.counter);
	memset(prefix.reserved, 0, sizeof(prefix.reserved));

	/* Serialize prefix to output buffer. */
	ubi_secure_prefix32_serialize(&prefix, out_buf);

	/* Build nonce. */
	uint8_t nonce[UBI_SECURE_NONCE_SIZE] = { 0 };

	ubi_secure_build_nonce(prefix.domain, prefix.salt, prefix.counter, nonce);

	/* Build AAD. */
	uint8_t aad[UBI_SECURE_DEV_HDR_AAD_SIZE] = { 0 };
	const struct ubi_secure_dev_hdr_aad_input aad_input = {
		.prefix = out_buf,
		.peb_index = (uint32_t)peb_idx,
		.flash_offset = flash_offset,
	};

	ubi_secure_build_dev_hdr_aad(&aad_input, aad);

	/* Build plaintext: dev_hdr + dev_secure_meta. */
	uint8_t plaintext[UBI_SECURE_DEV_HDR_PLAINTEXT_SIZE] = { 0 };

	memcpy(plaintext, dev_hdr, UBI_DEV_HDR_SIZE);
	ubi_secure_dev_meta_serialize(dev_meta, &plaintext[UBI_DEV_HDR_SIZE]);

	/* Encrypt to output buffer after prefix. */
	size_t ct_len = 0;

	ret = ubi_secure_aead_encrypt(child_key_id, nonce, aad, sizeof(aad), plaintext,
				      sizeof(plaintext), &out_buf[UBI_SECURE_PREFIX_SIZE],
				      UBI_SECURE_DEV_HDR_CT_TAG_SIZE, &ct_len);
	if (ret != 0) {
		LOG_ERR("AEAD encrypt failed for dev hdr");
	}

	return ret;
}

static int encrypt_vol_hdr(const struct ubi_vol_hdr *vol_hdr, psa_key_id_t child_key_id,
			   uint8_t key_version, uint64_t counter, size_t peb_idx,
			   uint64_t flash_offset, uint64_t device_revision, uint8_t parent_kv,
			   uint8_t *out_buf)
{
	__ASSERT_NO_MSG(vol_hdr != NULL);
	__ASSERT_NO_MSG(out_buf != NULL);

	struct ubi_crypto_prefix32 prefix = {
		.magic = UBI_SECURE_PREFIX_MAGIC,
		.wrapper_version = UBI_SECURE_WRAPPER_VERSION,
		.domain = UBI_SECURE_DOMAIN_VOLUME_HEADER,
		.key_version = key_version,
		.flags = 0,
	};

	int ret = ubi_secure_generate_salt(prefix.salt);

	if (ret != 0) {
		LOG_ERR("Salt generation failed for vol hdr");
		return ret;
	}

	ubi_secure_encode_counter48(counter, prefix.counter);
	memset(prefix.reserved, 0, sizeof(prefix.reserved));

	ubi_secure_prefix32_serialize(&prefix, out_buf);

	uint8_t nonce[UBI_SECURE_NONCE_SIZE] = { 0 };

	ubi_secure_build_nonce(prefix.domain, prefix.salt, prefix.counter, nonce);

	uint8_t aad[UBI_SECURE_VOL_HDR_AAD_SIZE] = { 0 };
	const struct ubi_secure_vol_hdr_aad_input aad_input = {
		.prefix = out_buf,
		.peb_index = (uint32_t)peb_idx,
		.flash_offset = flash_offset,
		.device_revision = device_revision,
		.parent_kv = parent_kv,
	};

	ubi_secure_build_vol_hdr_aad(&aad_input, aad);

	size_t ct_len = 0;

	ret = ubi_secure_aead_encrypt(child_key_id, nonce, aad, sizeof(aad),
				      (const uint8_t *)vol_hdr, UBI_SECURE_VOL_HDR_PLAINTEXT_SIZE,
				      &out_buf[UBI_SECURE_PREFIX_SIZE],
				      UBI_SECURE_VOL_HDR_CT_TAG_SIZE, &ct_len);
	if (ret != 0) {
		LOG_ERR("AEAD encrypt failed for vol hdr");
	}

	return ret;
}

/* Module interface function definitions -------------------------------------------------------- */

int ubi_secure_res_peb_detect_mode(const struct ubi_flash_desc *flash, size_t peb_idx,
				   bool *is_secure, bool *is_blank)
{
	if (flash == NULL || is_secure == NULL || is_blank == NULL) {
		LOG_ERR("res_peb_detect_mode: NULL argument");
		return -EINVAL;
	}

	const struct flash_area *fa = NULL;
	int ret = flash_area_open(flash->partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		return -EIO;
	}

	uint8_t buf[4] = { 0 };
	const size_t offset = peb_idx * flash->erase_block_size;

	ret = flash_area_read(fa, offset, buf, sizeof(buf));
	flash_area_close(fa);

	if (ret != 0) {
		LOG_ERR("Flash read failure at PEB %zu", peb_idx);
		return -EIO;
	}

	const uint32_t magic = sys_get_be32(buf);

	*is_secure = (magic == UBI_SECURE_PREFIX_MAGIC);

	/* Check for blank (all erased). */
	uint8_t erased_val = 0;

	ret = ubi_get_erased_val(flash, &erased_val);
	if (ret != 0) {
		LOG_ERR("get_erased_val failed: %d", ret);
		return ret;
	}

	*is_blank = ubi_buf_is_erased(buf, sizeof(buf), erased_val);
	return 0;
}

int ubi_secure_res_peb_scan(const struct ubi_flash_desc *flash,
			    const struct ubi_crypto_config *crypto_cfg,
			    struct ubi_secure_res_peb_scan *scan)
{
	if (flash == NULL || crypto_cfg == NULL || scan == NULL) {
		LOG_ERR("res_peb_scan: NULL argument");
		return -EINVAL;
	}

	/* Build the result on the stack and publish it to *scan only on success
	 * so a partial scan never overwrites the caller's prior state.
	 */
	struct ubi_secure_res_peb_scan local = { 0 };

	/* Derive the device-header child key for each key version in the allowlist.
	 * For scan phase, we try each allowlisted version against each PEB.
	 */
	const struct flash_area *fa = NULL;
	int ret = flash_area_open(flash->partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		return -EIO;
	}

	uint8_t erased_val = 0;

	ret = ubi_get_erased_val(flash, &erased_val);
	if (ret != 0) {
		LOG_ERR("get_erased_val failed: %d", ret);
		flash_area_close(fa);
		return ret;
	}

	uint32_t highest_revision = 0;

	for (size_t peb = 0; peb < UBI_DEV_HDR_NR_OF_RES_PEBS; peb++) {
		uint8_t raw[UBI_SECURE_DEV_HDR_SIZE] = { 0 };
		const size_t offset = peb * flash->erase_block_size;

		ret = flash_area_read(fa, offset, raw, sizeof(raw));
		if (ret != 0) {
			LOG_ERR("Flash read failure at reserved PEB %zu", peb);
			local.state[peb] = UBI_SECURE_RES_PEB_CORRUPT;
			local.corrupt_count++;
			continue;
		}

		/* Check if blank. */
		if (ubi_buf_is_erased(raw, sizeof(raw), erased_val)) {
			local.state[peb] = UBI_SECURE_RES_PEB_SPARE;
			local.spare_count++;
			continue;
		}

		/* Check magic before trying auth. */
		const uint32_t magic = sys_get_be32(raw);

		if (magic != UBI_SECURE_PREFIX_MAGIC) {
			local.state[peb] = UBI_SECURE_RES_PEB_CORRUPT;
			local.corrupt_count++;
			continue;
		}

		/* Extract key_version from prefix to know which key to use. */
		const uint8_t kv = raw[UBI_SECURE_PREFIX_OFF_KEY_VERSION];

		/* Check key_version against allowlist — an on-flash key version
		 * absent from the allowlist is a policy error. Treat the PEB as
		 * corrupt so the authenticated copy (if any) still wins.
		 */
		if (ubi_secure_policy_kv_slot(&crypto_cfg->policy, kv) < 0) {
			LOG_ERR("Key version %u at PEB %zu not in allowlist", kv, peb);
			local.state[peb] = UBI_SECURE_RES_PEB_CORRUPT;
			local.corrupt_count++;
			continue;
		}

		/* Derive child key for this version. */
		psa_key_id_t child_key_id = PSA_KEY_ID_NULL;

		ret = ubi_secure_derive_domain_key(crypto_cfg, UBI_SECURE_DOMAIN_DEVICE_HEADER, kv,
						   &child_key_id);
		if (ret != 0) {
			LOG_ERR("Cannot derive key for version %u on PEB %zu", kv, peb);
			local.state[peb] = UBI_SECURE_RES_PEB_CORRUPT;
			local.corrupt_count++;
			continue;
		}

		struct ubi_dev_hdr hdr = { 0 };
		struct ubi_dev_secure_meta meta = { 0 };
		struct ubi_crypto_prefix32 prefix = { 0 };

		ret = authenticate_dev_hdr(raw, peb, offset, child_key_id, &hdr, &meta, &prefix);
		ubi_secure_destroy_key(child_key_id);

		if (ret != 0) {
			LOG_ERR("Auth failure on reserved PEB %zu", peb);
			local.state[peb] = UBI_SECURE_RES_PEB_CORRUPT;
			local.corrupt_count++;
			continue;
		}

		local.state[peb] = UBI_SECURE_RES_PEB_AUTHENTICATED;
		local.auth_count++;
		local.revision[peb] = hdr.revision;

		/* Select highest revision as canonical. */
		if (local.auth_count == 1 || hdr.revision > highest_revision) {
			highest_revision = hdr.revision;
			local.dev_hdr = hdr;
			local.dev_meta = meta;
			local.dev_prefix = prefix;
			local.canonical_peb_idx = peb;
		}
	}

	flash_area_close(fa);
	*scan = local;
	return 0;
}

int ubi_secure_res_peb_read_vol_hdrs(const struct ubi_flash_desc *flash,
				     const struct ubi_crypto_config *crypto_cfg,
				     const struct ubi_secure_res_peb_scan *scan,
				     struct ubi_vol_hdr *vol_hdrs, size_t max_vols)
{
	if (flash == NULL || crypto_cfg == NULL || scan == NULL || vol_hdrs == NULL) {
		LOG_ERR("res_peb_read_vol_hdrs: NULL argument");
		return -EINVAL;
	}

	if (scan->dev_hdr.vol_count == 0) {
		return 0;
	}

	if (scan->dev_hdr.vol_count > max_vols) {
		LOG_ERR("vol_count %u exceeds max %zu", scan->dev_hdr.vol_count, max_vols);
		return -EOVERFLOW;
	}

	/* Derive volume-header child key. */
	psa_key_id_t child_key_id = PSA_KEY_ID_NULL;
	int ret = ubi_secure_derive_domain_key(crypto_cfg, UBI_SECURE_DOMAIN_VOLUME_HEADER,
					       scan->dev_prefix.key_version, &child_key_id);
	if (ret != 0) {
		LOG_ERR("ubi_secure_derive_domain_key failed for vol hdr: %d", ret);
		return ret;
	}

	const struct flash_area *fa = NULL;

	ret = flash_area_open(flash->partition_id, &fa);
	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		ubi_secure_destroy_key(child_key_id);
		return -EIO;
	}

	const size_t peb_offset = scan->canonical_peb_idx * flash->erase_block_size;

	for (size_t i = 0; i < scan->dev_hdr.vol_count; i++) {
		uint8_t raw[UBI_SECURE_VOL_HDR_SIZE] = { 0 };
		const size_t vol_offset =
			peb_offset + UBI_SECURE_DEV_HDR_SIZE + (i * UBI_SECURE_VOL_HDR_SIZE);

		ret = flash_area_read(fa, vol_offset, raw, sizeof(raw));
		if (ret != 0) {
			LOG_ERR("Flash read vol hdr %zu failed", i);
			goto cleanup;
		}

		/* Deserialize prefix and verify. */
		struct ubi_crypto_prefix32 prefix = { 0 };

		ubi_secure_prefix32_deserialize(raw, &prefix);

		if (prefix.magic != UBI_SECURE_PREFIX_MAGIC ||
		    prefix.domain != UBI_SECURE_DOMAIN_VOLUME_HEADER) {
			LOG_ERR("Vol hdr %zu bad prefix", i);
			ret = -EBADMSG;
			goto cleanup;
		}

		if (prefix.wrapper_version != UBI_SECURE_WRAPPER_VERSION) {
			LOG_ERR("Vol hdr %zu unsupported wrapper_version %u (expected %u)", i,
				prefix.wrapper_version, UBI_SECURE_WRAPPER_VERSION);
			ret = -EBADMSG;
			goto cleanup;
		}

		/* Build nonce. */
		uint8_t nonce[UBI_SECURE_NONCE_SIZE] = { 0 };

		ubi_secure_build_nonce(prefix.domain, prefix.salt, prefix.counter, nonce);

		/* Build AAD with device revision and parent key version. */
		uint8_t aad[UBI_SECURE_VOL_HDR_AAD_SIZE] = { 0 };
		const struct ubi_secure_vol_hdr_aad_input aad_input = {
			.prefix = raw,
			.peb_index = (uint32_t)scan->canonical_peb_idx,
			.flash_offset = vol_offset,
			.device_revision = (uint64_t)scan->dev_hdr.revision,
			.parent_kv = scan->dev_prefix.key_version,
		};

		ubi_secure_build_vol_hdr_aad(&aad_input, aad);

		/* Decrypt. */
		uint8_t plaintext[UBI_SECURE_VOL_HDR_PLAINTEXT_SIZE] = { 0 };
		size_t pt_len = 0;
		const uint8_t *ct = &raw[UBI_SECURE_PREFIX_SIZE];

		ret = ubi_secure_aead_decrypt(child_key_id, nonce, aad, sizeof(aad), ct,
					      UBI_SECURE_VOL_HDR_CT_TAG_SIZE, plaintext,
					      sizeof(plaintext), &pt_len);
		if (ret != 0) {
			LOG_ERR("Vol hdr %zu auth failure", i);
			goto cleanup;
		}

		if (pt_len != UBI_SECURE_VOL_HDR_PLAINTEXT_SIZE) {
			LOG_ERR("Vol hdr %zu unexpected plaintext size: %zu", i, pt_len);
			ret = -EBADMSG;
			goto cleanup;
		}

		memcpy(&vol_hdrs[i], plaintext, UBI_VOL_HDR_SIZE);
	}

	ret = 0;

cleanup:
	flash_area_close(fa);
	ubi_secure_destroy_key(child_key_id);
	return ret;
}

int ubi_secure_res_peb_commit(const struct ubi_flash_desc *flash,
			      const struct ubi_crypto_config *crypto_cfg,
			      const struct ubi_dev_hdr *dev_hdr,
			      const struct ubi_dev_secure_meta *dev_meta,
			      const struct ubi_vol_hdr *vol_hdrs, size_t vol_count,
			      uint8_t key_version, uint64_t counter)
{
	if (flash == NULL || crypto_cfg == NULL || dev_hdr == NULL || dev_meta == NULL) {
		LOG_ERR("res_peb_commit: NULL argument");
		return -EINVAL;
	}

	if (vol_count > 0 && vol_hdrs == NULL) {
		LOG_ERR("res_peb_commit: vol_hdrs NULL with vol_count > 0");
		return -EINVAL;
	}

	/* Overflow guard: counter + 1 + vol_count must not exceed 48-bit max. */
	if (counter + vol_count > UBI_SECURE_COUNTER_MAX) {
		LOG_ERR("Reserved-PEB AEAD counter overflow");
		return -EOVERFLOW;
	}

	/* Derive device-header and volume-header child keys. */
	psa_key_id_t dev_key_id = PSA_KEY_ID_NULL;

	int ret = ubi_secure_derive_domain_key(crypto_cfg, UBI_SECURE_DOMAIN_DEVICE_HEADER,
					       key_version, &dev_key_id);
	if (ret != 0) {
		LOG_ERR("ubi_secure_derive_domain_key failed for dev hdr commit: %d", ret);
		return ret;
	}

	psa_key_id_t vol_key_id = PSA_KEY_ID_NULL;

	ret = ubi_secure_derive_domain_key(crypto_cfg, UBI_SECURE_DOMAIN_VOLUME_HEADER, key_version,
					   &vol_key_id);
	if (ret != 0) {
		LOG_ERR("ubi_secure_derive_domain_key failed for vol hdr commit: %d", ret);
		ubi_secure_destroy_key(dev_key_id);
		return ret;
	}

	/* Build the full content buffer: secure_dev_hdr + N * secure_vol_hdr. */
	const size_t content_len = UBI_SECURE_DEV_HDR_SIZE + (vol_count * UBI_SECURE_VOL_HDR_SIZE);

	/* Use a stack buffer large enough for typical configs. */
	uint8_t content[UBI_SECURE_DEV_HDR_SIZE +
			(CONFIG_UBI_MAX_NR_OF_VOLUMES * UBI_SECURE_VOL_HDR_SIZE)] = { 0 };

	if (content_len > sizeof(content)) {
		LOG_ERR("Reserved content too large: %zu > %zu", content_len, sizeof(content));
		ret = -ENOSPC;
		goto cleanup;
	}

	/* Write to each reserved PEB. */
	const struct flash_area *fa = NULL;

	ret = flash_area_open(flash->partition_id, &fa);
	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		ret = -EIO;
		goto cleanup;
	}

	size_t active_written = 0;

	for (size_t peb = 0; peb < UBI_DEV_HDR_NR_OF_RES_PEBS; peb++) {
		const size_t peb_offset = peb * flash->erase_block_size;

		/* Erase this reserved PEB. */
		ret = flash_area_erase(fa, peb_offset, flash->erase_block_size);
		if (ret != 0) {
			LOG_ERR("Erase failure on reserved PEB %zu", peb);
			continue;
		}

		/* Encrypt device header (counter, fresh salt per PEB). */
		ret = encrypt_dev_hdr(dev_hdr, dev_meta, dev_key_id, key_version, counter, peb,
				      peb_offset, content);
		if (ret != 0) {
			LOG_ERR("Encrypt dev hdr failed for PEB %zu", peb);
			continue;
		}

		/* Encrypt each volume header. */
		for (size_t v = 0; v < vol_count; v++) {
			const size_t vol_flash_offset = peb_offset + UBI_SECURE_DEV_HDR_SIZE +
							(v * UBI_SECURE_VOL_HDR_SIZE);
			/* Volume headers use an incrementing counter after the device header. */
			ret = encrypt_vol_hdr(
				&vol_hdrs[v], vol_key_id, key_version, counter + 1 + v, peb,
				vol_flash_offset, (uint64_t)dev_hdr->revision, key_version,
				&content[UBI_SECURE_DEV_HDR_SIZE + (v * UBI_SECURE_VOL_HDR_SIZE)]);
			if (ret != 0) {
				LOG_ERR("Encrypt vol hdr %zu failed for PEB %zu", v, peb);
				break;
			}
		}

		if (ret != 0) {
			continue;
		}

		/* Write the full content. */
		ret = ubi_secure_flash_write(fa, peb_offset, content, content_len);
		if (ret != 0) {
			LOG_ERR("Write failure on reserved PEB %zu", peb);
			continue;
		}

		active_written++;
	}

	flash_area_close(fa);

	if (active_written == 0) {
		LOG_ERR("No reserved PEBs written");
		ret = -EIO;
		goto cleanup;
	}

	if (active_written < UBI_SECURE_RES_PEB_NR_ACTIVE) {
		LOG_ERR("Degraded: only %zu of %u reserved PEBs written", active_written,
			UBI_SECURE_RES_PEB_NR_ACTIVE);
		ret = -EROFS;
	} else {
		ret = 0;
	}

cleanup:
	ubi_secure_destroy_key(dev_key_id);
	ubi_secure_destroy_key(vol_key_id);
	return ret;
}
