/**
 * \file    ubi_secure_reserved.c
 * \author  Kamil Kielbasa
 * \brief   Secure reserved-PEB management: scan, authenticate, write.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files ------------------------------------------------------------------------------- */
#include "ubi_secure_reserved.h"
#include "ubi_secure_crypto.h"
#include "ubi_secure_ser.h"
#include "ubi_secure_types.h"
#include "ubi_internal.h"
#include "ubi_io.h"

#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/byteorder.h>

#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------ */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/** Plaintext payload for secure device header: dev_hdr(32) + dev_secure_meta(16) = 48. */
#define DEV_HDR_PLAINTEXT_SIZE (UBI_DEV_HDR_SIZE + UBI_SECURE_DEV_META_SIZE)

/** Ciphertext+tag for device header: 48 + 16 = 64. */
#define DEV_HDR_CT_TAG_SIZE (DEV_HDR_PLAINTEXT_SIZE + UBI_SECURE_TAG_SIZE)

/** Plaintext payload for secure volume header: vol_hdr(48). */
#define VOL_HDR_PLAINTEXT_SIZE (UBI_VOL_HDR_SIZE)

/** Ciphertext+tag for volume header: 48 + 16 = 64. */
#define VOL_HDR_CT_TAG_SIZE (VOL_HDR_PLAINTEXT_SIZE + UBI_SECURE_TAG_SIZE)

/* Static function declarations ---------------------------------------------------------------- */

static int authenticate_dev_hdr(const uint8_t *raw, size_t peb_idx, uint64_t flash_offset,
				uint32_t child_key_id, struct ubi_dev_hdr *dev_hdr,
				struct ubi_dev_secure_meta *dev_meta,
				struct ubi_crypto_prefix32 *prefix);

static int encrypt_dev_hdr(const struct ubi_dev_hdr *dev_hdr,
			   const struct ubi_dev_secure_meta *dev_meta, uint32_t child_key_id,
			   uint8_t key_version, uint64_t counter, size_t peb_idx,
			   uint64_t flash_offset, uint8_t *out_buf);

static int encrypt_vol_hdr(const struct ubi_vol_hdr *vol_hdr, uint32_t child_key_id,
			   uint8_t key_version, uint64_t counter, size_t peb_idx,
			   uint64_t flash_offset, uint64_t device_revision, uint8_t parent_kv,
			   uint8_t *out_buf);

/* Static function definitions ----------------------------------------------------------------- */

static int authenticate_dev_hdr(const uint8_t *raw, size_t peb_idx, uint64_t flash_offset,
				uint32_t child_key_id, struct ubi_dev_hdr *dev_hdr,
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

	ubi_secure_build_dev_hdr_aad(prefix_bytes, peb_idx, flash_offset, aad);

	/* Decrypt ciphertext+tag. */
	const uint8_t *ct = &raw[UBI_SECURE_PREFIX_SIZE];
	uint8_t plaintext[DEV_HDR_PLAINTEXT_SIZE] = { 0 };
	size_t pt_len = 0;

	const int ret = ubi_secure_aead_decrypt(child_key_id, nonce, aad, sizeof(aad), ct,
						DEV_HDR_CT_TAG_SIZE, plaintext, sizeof(plaintext),
						&pt_len);
	if (ret != 0) {
		LOG_ERR("AEAD decrypt failed for dev hdr at PEB %zu", peb_idx);
		return ret;
	}

	if (pt_len != DEV_HDR_PLAINTEXT_SIZE) {
		LOG_ERR("Unexpected plaintext size: %zu", pt_len);
		return -EBADMSG;
	}

	/* Extract dev_hdr and dev_secure_meta from plaintext. */
	memcpy(dev_hdr, plaintext, UBI_DEV_HDR_SIZE);
	ubi_secure_dev_meta_deserialize(&plaintext[UBI_DEV_HDR_SIZE], dev_meta);

	return 0;
}

static int encrypt_dev_hdr(const struct ubi_dev_hdr *dev_hdr,
			   const struct ubi_dev_secure_meta *dev_meta, uint32_t child_key_id,
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

	ubi_secure_build_dev_hdr_aad(out_buf, peb_idx, flash_offset, aad);

	/* Build plaintext: dev_hdr + dev_secure_meta. */
	uint8_t plaintext[DEV_HDR_PLAINTEXT_SIZE] = { 0 };

	memcpy(plaintext, dev_hdr, UBI_DEV_HDR_SIZE);
	ubi_secure_dev_meta_serialize(dev_meta, &plaintext[UBI_DEV_HDR_SIZE]);

	/* Encrypt to output buffer after prefix. */
	size_t ct_len = 0;

	ret = ubi_secure_aead_encrypt(child_key_id, nonce, aad, sizeof(aad), plaintext,
				      sizeof(plaintext), &out_buf[UBI_SECURE_PREFIX_SIZE],
				      DEV_HDR_CT_TAG_SIZE, &ct_len);
	if (ret != 0) {
		LOG_ERR("AEAD encrypt failed for dev hdr");
	}

	return ret;
}

static int encrypt_vol_hdr(const struct ubi_vol_hdr *vol_hdr, uint32_t child_key_id,
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

	ubi_secure_build_vol_hdr_aad(out_buf, peb_idx, flash_offset, device_revision, parent_kv,
				     aad);

	size_t ct_len = 0;

	ret = ubi_secure_aead_encrypt(child_key_id, nonce, aad, sizeof(aad),
				      (const uint8_t *)vol_hdr, VOL_HDR_PLAINTEXT_SIZE,
				      &out_buf[UBI_SECURE_PREFIX_SIZE], VOL_HDR_CT_TAG_SIZE,
				      &ct_len);
	if (ret != 0) {
		LOG_ERR("AEAD encrypt failed for vol hdr");
	}

	return ret;
}

/* Module interface function definitions ------------------------------------------------------- */

int ubi_secure_res_peb_detect_mode(const struct ubi_mtd *mtd, size_t peb_idx, bool *is_secure,
				   bool *is_blank)
{
	if (mtd == NULL || is_secure == NULL || is_blank == NULL) {
		LOG_ERR("res_peb_detect_mode: NULL argument");
		return -EINVAL;
	}

	const struct flash_area *fa = NULL;
	int ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		return -EIO;
	}

	uint8_t buf[4] = { 0 };
	const size_t offset = peb_idx * mtd->erase_block_size;

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

	ret = ubi_get_erased_val(mtd, &erased_val);
	if (ret != 0) {
		LOG_ERR("get_erased_val failed: %d", ret);
		return ret;
	}

	*is_blank = ubi_buf_is_erased(buf, sizeof(buf), erased_val);
	return 0;
}

int ubi_secure_res_peb_scan(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			    struct ubi_secure_res_peb_scan *scan)
{
	if (mtd == NULL || crypto_cfg == NULL || scan == NULL) {
		LOG_ERR("res_peb_scan: NULL argument");
		return -EINVAL;
	}

	memset(scan, 0, sizeof(*scan));

	/* Derive the device-header child key for each key version in the allowlist.
	 * For scan phase, we try each allowlisted version against each PEB.
	 */
	const struct flash_area *fa = NULL;
	int ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		return -EIO;
	}

	uint8_t erased_val = 0;

	ret = ubi_get_erased_val(mtd, &erased_val);
	if (ret != 0) {
		LOG_ERR("get_erased_val failed: %d", ret);
		flash_area_close(fa);
		return ret;
	}

	uint32_t highest_revision = 0;

	for (size_t peb = 0; peb < UBI_DEV_HDR_NR_OF_RES_PEBS; peb++) {
		uint8_t raw[UBI_SECURE_DEV_HDR_SIZE] = { 0 };
		const size_t offset = peb * mtd->erase_block_size;

		ret = flash_area_read(fa, offset, raw, sizeof(raw));
		if (ret != 0) {
			LOG_ERR("Flash read failure at reserved PEB %zu", peb);
			scan->state[peb] = UBI_SECURE_RES_PEB_CORRUPT;
			scan->corrupt_count++;
			continue;
		}

		/* Check if blank. */
		if (ubi_buf_is_erased(raw, sizeof(raw), erased_val)) {
			scan->state[peb] = UBI_SECURE_RES_PEB_SPARE;
			scan->spare_count++;
			continue;
		}

		/* Check magic before trying auth. */
		const uint32_t magic = sys_get_be32(raw);

		if (magic != UBI_SECURE_PREFIX_MAGIC) {
			scan->state[peb] = UBI_SECURE_RES_PEB_CORRUPT;
			scan->corrupt_count++;
			continue;
		}

		/* Extract key_version from prefix to know which key to use. */
		const uint8_t kv = raw[UBI_SECURE_PREFIX_OFF_KEY_VERSION];

		/* Check key_version against allowlist — per spec §13.1 an
		 * on-flash key version absent from the allowlist is a policy
		 * error. Treat the PEB as corrupt so the authenticated copy
		 * (if any) still wins. */
		bool kv_allowed = false;

		for (size_t i = 0; i < crypto_cfg->policy.allowed_key_versions_len; i++) {
			if (crypto_cfg->policy.allowed_key_versions[i] == kv) {
				kv_allowed = true;
				break;
			}
		}

		if (!kv_allowed) {
			LOG_ERR("Key version %u at PEB %zu not in allowlist", kv, peb);
			scan->state[peb] = UBI_SECURE_RES_PEB_CORRUPT;
			scan->corrupt_count++;
			continue;
		}

		/* Derive child key for this version. */
		uint32_t child_key_id = 0;

		ret = ubi_secure_derive_domain_key(crypto_cfg, UBI_SECURE_DOMAIN_DEVICE_HEADER, kv,
						   &child_key_id);
		if (ret != 0) {
			LOG_ERR("Cannot derive key for version %u on PEB %zu", kv, peb);
			scan->state[peb] = UBI_SECURE_RES_PEB_CORRUPT;
			scan->corrupt_count++;
			continue;
		}

		struct ubi_dev_hdr hdr = { 0 };
		struct ubi_dev_secure_meta meta = { 0 };
		struct ubi_crypto_prefix32 prefix = { 0 };

		ret = authenticate_dev_hdr(raw, peb, offset, child_key_id, &hdr, &meta, &prefix);
		ubi_secure_destroy_key(child_key_id);

		if (ret != 0) {
			LOG_ERR("Auth failure on reserved PEB %zu", peb);
			scan->state[peb] = UBI_SECURE_RES_PEB_CORRUPT;
			scan->corrupt_count++;
			continue;
		}

		scan->state[peb] = UBI_SECURE_RES_PEB_AUTHENTICATED;
		scan->auth_count++;
		scan->revision[peb] = hdr.revision;

		/* Select highest revision as canonical. */
		if (scan->auth_count == 1 || hdr.revision > highest_revision) {
			highest_revision = hdr.revision;
			scan->dev_hdr = hdr;
			scan->dev_meta = meta;
			scan->dev_prefix = prefix;
			scan->canonical_peb_idx = peb;
		}
	}

	flash_area_close(fa);
	return 0;
}

int ubi_secure_res_peb_read_vol_hdrs(const struct ubi_mtd *mtd,
				     const struct ubi_crypto_config *crypto_cfg,
				     const struct ubi_secure_res_peb_scan *scan,
				     struct ubi_vol_hdr *vol_hdrs, size_t max_vols)
{
	if (mtd == NULL || crypto_cfg == NULL || scan == NULL || vol_hdrs == NULL) {
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
	uint32_t child_key_id = 0;
	int ret = ubi_secure_derive_domain_key(crypto_cfg, UBI_SECURE_DOMAIN_VOLUME_HEADER,
					       scan->dev_prefix.key_version, &child_key_id);
	if (ret != 0) {
		LOG_ERR("ubi_secure_derive_domain_key failed for vol hdr: %d", ret);
		return ret;
	}

	const struct flash_area *fa = NULL;

	ret = flash_area_open(mtd->partition_id, &fa);
	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		ubi_secure_destroy_key(child_key_id);
		return -EIO;
	}

	const size_t peb_offset = scan->canonical_peb_idx * mtd->erase_block_size;

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

		/* Build nonce. */
		uint8_t nonce[UBI_SECURE_NONCE_SIZE] = { 0 };

		ubi_secure_build_nonce(prefix.domain, prefix.salt, prefix.counter, nonce);

		/* Build AAD with device revision and parent key version. */
		uint8_t aad[UBI_SECURE_VOL_HDR_AAD_SIZE] = { 0 };

		ubi_secure_build_vol_hdr_aad(raw, (uint32_t)scan->canonical_peb_idx, vol_offset,
					     (uint64_t)scan->dev_hdr.revision,
					     scan->dev_prefix.key_version, aad);

		/* Decrypt. */
		uint8_t plaintext[VOL_HDR_PLAINTEXT_SIZE] = { 0 };
		size_t pt_len = 0;
		const uint8_t *ct = &raw[UBI_SECURE_PREFIX_SIZE];

		ret = ubi_secure_aead_decrypt(child_key_id, nonce, aad, sizeof(aad), ct,
					      VOL_HDR_CT_TAG_SIZE, plaintext, sizeof(plaintext),
					      &pt_len);
		if (ret != 0) {
			LOG_ERR("Vol hdr %zu auth failure", i);
			goto cleanup;
		}

		if (pt_len != VOL_HDR_PLAINTEXT_SIZE) {
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

int ubi_secure_res_peb_commit(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			      const struct ubi_dev_hdr *dev_hdr,
			      const struct ubi_dev_secure_meta *dev_meta,
			      const struct ubi_vol_hdr *vol_hdrs, size_t vol_count,
			      uint8_t key_version, uint64_t counter)
{
	if (mtd == NULL || crypto_cfg == NULL || dev_hdr == NULL || dev_meta == NULL) {
		LOG_ERR("res_peb_commit: NULL argument");
		return -EINVAL;
	}

	if (vol_count > 0 && vol_hdrs == NULL) {
		LOG_ERR("res_peb_commit: vol_hdrs NULL with vol_count > 0");
		return -EINVAL;
	}

	/* Derive device-header and volume-header child keys. */
	uint32_t dev_key_id = 0;

	int ret = ubi_secure_derive_domain_key(crypto_cfg, UBI_SECURE_DOMAIN_DEVICE_HEADER,
					       key_version, &dev_key_id);
	if (ret != 0) {
		LOG_ERR("ubi_secure_derive_domain_key failed for dev hdr commit: %d", ret);
		return ret;
	}

	uint32_t vol_key_id = 0;

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

	ret = flash_area_open(mtd->partition_id, &fa);
	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		ret = -EIO;
		goto cleanup;
	}

	size_t active_written = 0;

	for (size_t peb = 0; peb < UBI_DEV_HDR_NR_OF_RES_PEBS; peb++) {
		const size_t peb_offset = peb * mtd->erase_block_size;

		/* Erase this reserved PEB. */
		ret = flash_area_erase(fa, peb_offset, mtd->erase_block_size);
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
		ret = flash_area_write(fa, peb_offset, content, content_len);
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
