/**
 * \file    ubi_secure_io.c
 * \author  Kamil Kielbasa
 * \brief   Secure data-PEB I/O: encrypted EC, VID, and LEB record operations.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files ------------------------------------------------------------------------------- */
#include "ubi_secure_io.h"
#include "ubi_secure_crypto.h"
#include "ubi_secure_ser.h"
#include "ubi_secure_types.h"
#include "ubi_internal.h"
#include "ubi_io.h"
#include "ubi_mem.h"

#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------ */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Module interface function definitions ------------------------------------------------------- */

int ubi_secure_ec_hdr_read(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			   size_t peb_idx, struct ubi_ec_hdr *ec_hdr,
			   struct ubi_secure_ec_auth_ctx *ec_ctx)
{
	if (mtd == NULL || crypto_cfg == NULL || ec_hdr == NULL || ec_ctx == NULL) {
		LOG_ERR("ec_hdr_read: NULL argument");
		return -EINVAL;
	}

	const struct flash_area *fa = NULL;
	int ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		return -EIO;
	}

	uint8_t raw[UBI_SECURE_EC_HDR_SIZE] = { 0 };
	const size_t offset = peb_idx * mtd->erase_block_size;

	ret = flash_area_read(fa, offset, raw, sizeof(raw));
	flash_area_close(fa);

	if (ret != 0) {
		LOG_ERR("Flash read failure at PEB %zu EC", peb_idx);
		return -EIO;
	}

	/* Deserialize prefix into local — only write output on success. */
	struct ubi_crypto_prefix32 prefix = { 0 };

	ubi_secure_prefix32_deserialize(raw, &prefix);

	if (prefix.magic != UBI_SECURE_PREFIX_MAGIC) {
		LOG_ERR("Bad magic in EC prefix at PEB %zu", peb_idx);
		return -EBADMSG;
	}

	if (prefix.domain != UBI_SECURE_DOMAIN_ERASE_COUNTER) {
		LOG_ERR("Unexpected domain %u in EC at PEB %zu", prefix.domain, peb_idx);
		return -EBADMSG;
	}

	/* Populate key_version early so callers have it even on error paths. */
	ec_ctx->key_version = prefix.key_version;

	/* Derive EC-domain child key. */
	uint32_t child_key_id = 0;

	ret = ubi_secure_derive_domain_key(crypto_cfg, UBI_SECURE_DOMAIN_ERASE_COUNTER,
					   prefix.key_version, &child_key_id);
	if (ret != 0) {
		LOG_ERR("EC key derivation failed at PEB %zu", peb_idx);
		return ret;
	}

	/* Build nonce. */
	uint8_t nonce[UBI_SECURE_NONCE_SIZE] = { 0 };

	ubi_secure_build_nonce(prefix.domain, prefix.salt, prefix.counter, nonce);

	/* Build AAD. */
	uint8_t aad[UBI_SECURE_EC_HDR_AAD_SIZE] = { 0 };

	ubi_secure_build_ec_hdr_aad(raw, (uint32_t)peb_idx, offset, aad);

	/* Decrypt. */
	const uint8_t *ct = &raw[UBI_SECURE_PREFIX_SIZE];
	uint8_t plaintext[UBI_SECURE_EC_PLAINTEXT_SIZE] = { 0 };
	size_t pt_len = 0;

	ret = ubi_secure_aead_decrypt(child_key_id, nonce, aad, sizeof(aad), ct,
				      UBI_SECURE_EC_CT_TAG_SIZE, plaintext, sizeof(plaintext),
				      &pt_len);
	ubi_secure_destroy_key(child_key_id);

	if (ret != 0) {
		LOG_ERR("EC AEAD decrypt failed at PEB %zu", peb_idx);
		ubi_secure_zeroize(plaintext, sizeof(plaintext));
		return -EBADMSG;
	}

	if (pt_len != UBI_SECURE_EC_PLAINTEXT_SIZE) {
		LOG_ERR("EC unexpected plaintext size: %zu", pt_len);
		ubi_secure_zeroize(plaintext, sizeof(plaintext));
		return -UBI_SECURE_EFORMAT;
	}

	/* Success — populate outputs. */
	memcpy(ec_hdr, plaintext, UBI_SECURE_EC_PLAINTEXT_SIZE);
	ec_ctx->ec = ec_hdr->ec;
	ec_ctx->key_version = prefix.key_version;

	ubi_secure_zeroize(plaintext, sizeof(plaintext));
	return 0;
}

int ubi_secure_ec_hdr_write(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			    size_t peb_idx, const struct ubi_ec_hdr *ec_hdr, uint8_t key_version,
			    uint64_t counter)
{
	if (mtd == NULL || crypto_cfg == NULL || ec_hdr == NULL) {
		LOG_ERR("ec_hdr_write: NULL argument");
		return -EINVAL;
	}

	/* Derive EC-domain child key. */
	uint32_t child_key_id = 0;
	int ret = ubi_secure_derive_domain_key(crypto_cfg, UBI_SECURE_DOMAIN_ERASE_COUNTER,
					       key_version, &child_key_id);

	if (ret != 0) {
		LOG_ERR("EC key derivation failed for write at PEB %zu", peb_idx);
		return ret;
	}

	/* Build prefix. */
	struct ubi_crypto_prefix32 prefix = {
		.magic = UBI_SECURE_PREFIX_MAGIC,
		.wrapper_version = UBI_SECURE_WRAPPER_VERSION,
		.domain = UBI_SECURE_DOMAIN_ERASE_COUNTER,
		.key_version = key_version,
		.flags = 0,
	};

	ret = ubi_secure_generate_salt(prefix.salt);
	if (ret != 0) {
		LOG_ERR("Salt generation failed for EC write");
		ubi_secure_destroy_key(child_key_id);
		return ret;
	}

	ubi_secure_encode_counter48(counter, prefix.counter);
	memset(prefix.reserved, 0, sizeof(prefix.reserved));

	/* Build output buffer. */
	uint8_t out_buf[UBI_SECURE_EC_HDR_SIZE] = { 0 };

	ubi_secure_prefix32_serialize(&prefix, out_buf);

	/* Build nonce. */
	uint8_t nonce[UBI_SECURE_NONCE_SIZE] = { 0 };

	ubi_secure_build_nonce(prefix.domain, prefix.salt, prefix.counter, nonce);

	/* Build AAD. */
	const size_t offset = peb_idx * mtd->erase_block_size;
	uint8_t aad[UBI_SECURE_EC_HDR_AAD_SIZE] = { 0 };

	ubi_secure_build_ec_hdr_aad(out_buf, (uint32_t)peb_idx, offset, aad);

	/* Encrypt. */
	size_t ct_len = 0;

	ret = ubi_secure_aead_encrypt(child_key_id, nonce, aad, sizeof(aad),
				      (const uint8_t *)ec_hdr, UBI_SECURE_EC_PLAINTEXT_SIZE,
				      &out_buf[UBI_SECURE_PREFIX_SIZE], UBI_SECURE_EC_CT_TAG_SIZE,
				      &ct_len);
	ubi_secure_destroy_key(child_key_id);

	if (ret != 0) {
		LOG_ERR("EC AEAD encrypt failed at PEB %zu", peb_idx);
		return ret;
	}

	/* Write to flash. */
	const struct flash_area *fa = NULL;

	ret = flash_area_open(mtd->partition_id, &fa);
	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		return -EIO;
	}

	ret = flash_area_write(fa, offset, out_buf, sizeof(out_buf));
	flash_area_close(fa);

	if (ret != 0) {
		LOG_ERR("Flash write failure at PEB %zu EC", peb_idx);
		return -EIO;
	}

	return 0;
}

int ubi_secure_vid_hdr_read(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			    size_t peb_idx, const struct ubi_secure_ec_auth_ctx *ec_ctx,
			    struct ubi_vid_hdr *vid_hdr, struct ubi_vid_secure_meta *vid_meta,
			    struct ubi_secure_vid_auth_ctx *vid_ctx)
{
	if (mtd == NULL || crypto_cfg == NULL || ec_ctx == NULL || vid_hdr == NULL ||
	    vid_meta == NULL || vid_ctx == NULL) {
		LOG_ERR("vid_hdr_read: NULL argument");
		return -EINVAL;
	}

	const struct flash_area *fa = NULL;
	int ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		return -EIO;
	}

	uint8_t raw[UBI_SECURE_DATA_VID_SIZE] = { 0 };
	const size_t offset = peb_idx * mtd->erase_block_size + UBI_SECURE_EC_HDR_SIZE;

	ret = flash_area_read(fa, offset, raw, sizeof(raw));
	flash_area_close(fa);

	if (ret != 0) {
		LOG_ERR("Flash read failure at PEB %zu VID", peb_idx);
		return -EIO;
	}

	/* Deserialize prefix into local — only write output on success. */
	struct ubi_crypto_prefix32 prefix = { 0 };

	ubi_secure_prefix32_deserialize(raw, &prefix);

	if (prefix.magic != UBI_SECURE_PREFIX_MAGIC) {
		LOG_ERR("Bad magic in VID prefix at PEB %zu", peb_idx);
		return -EBADMSG;
	}

	if (prefix.domain != UBI_SECURE_DOMAIN_VOLUME_IDENTIFIER) {
		LOG_ERR("Unexpected domain %u in VID at PEB %zu", prefix.domain, peb_idx);
		return -EBADMSG;
	}

	/* Populate key_version early so callers have it even on error paths. */
	vid_ctx->key_version = prefix.key_version;

	/* Derive VID-domain child key. */
	uint32_t child_key_id = 0;

	ret = ubi_secure_derive_domain_key(crypto_cfg, UBI_SECURE_DOMAIN_VOLUME_IDENTIFIER,
					   prefix.key_version, &child_key_id);
	if (ret != 0) {
		LOG_ERR("VID key derivation failed at PEB %zu", peb_idx);
		return ret;
	}

	/* Build nonce. */
	uint8_t nonce[UBI_SECURE_NONCE_SIZE] = { 0 };

	ubi_secure_build_nonce(prefix.domain, prefix.salt, prefix.counter, nonce);

	/* Build AAD. */
	uint8_t aad[UBI_SECURE_DATA_VID_AAD_SIZE] = { 0 };

	ubi_secure_build_data_vid_aad(raw, (uint32_t)peb_idx, offset, ec_ctx->ec,
				      ec_ctx->key_version, aad);

	/* Decrypt. */
	const uint8_t *ct = &raw[UBI_SECURE_PREFIX_SIZE];
	uint8_t plaintext[UBI_SECURE_DATA_VID_PLAINTEXT_SIZE] = { 0 };
	size_t pt_len = 0;

	ret = ubi_secure_aead_decrypt(child_key_id, nonce, aad, sizeof(aad), ct,
				      UBI_SECURE_DATA_VID_CT_TAG_SIZE, plaintext, sizeof(plaintext),
				      &pt_len);
	ubi_secure_destroy_key(child_key_id);

	if (ret != 0) {
		LOG_ERR("VID AEAD decrypt failed at PEB %zu", peb_idx);
		ubi_secure_zeroize(plaintext, sizeof(plaintext));
		return -EBADMSG;
	}

	if (pt_len != UBI_SECURE_DATA_VID_PLAINTEXT_SIZE) {
		LOG_ERR("VID unexpected plaintext size: %zu", pt_len);
		ubi_secure_zeroize(plaintext, sizeof(plaintext));
		return -UBI_SECURE_EFORMAT;
	}

	/* Success — populate outputs. */
	memcpy(vid_hdr, plaintext, UBI_SECURE_PLAIN_VID_HDR_SIZE);
	ubi_secure_vid_meta_deserialize(&plaintext[UBI_SECURE_PLAIN_VID_HDR_SIZE], vid_meta);

	vid_ctx->ec_ctx = *ec_ctx;
	vid_ctx->vid_hdr = vid_hdr;
	vid_ctx->key_version = prefix.key_version;
	vid_ctx->vid_counter = ubi_secure_decode_counter48(prefix.counter);

	ubi_secure_zeroize(plaintext, sizeof(plaintext));
	return 0;
}

int ubi_secure_vid_hdr_write(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			     size_t peb_idx, const struct ubi_secure_ec_auth_ctx *ec_ctx,
			     const struct ubi_vid_hdr *vid_hdr,
			     const struct ubi_vid_secure_meta *vid_meta, uint8_t key_version,
			     uint64_t counter)
{
	if (mtd == NULL || crypto_cfg == NULL || ec_ctx == NULL || vid_hdr == NULL ||
	    vid_meta == NULL) {
		LOG_ERR("vid_hdr_write: NULL argument");
		return -EINVAL;
	}

	/* Derive VID-domain child key. */
	uint32_t child_key_id = 0;
	int ret = ubi_secure_derive_domain_key(crypto_cfg, UBI_SECURE_DOMAIN_VOLUME_IDENTIFIER,
					       key_version, &child_key_id);

	if (ret != 0) {
		LOG_ERR("VID key derivation failed for write at PEB %zu", peb_idx);
		return ret;
	}

	/* Build prefix. */
	struct ubi_crypto_prefix32 prefix = {
		.magic = UBI_SECURE_PREFIX_MAGIC,
		.wrapper_version = UBI_SECURE_WRAPPER_VERSION,
		.domain = UBI_SECURE_DOMAIN_VOLUME_IDENTIFIER,
		.key_version = key_version,
		.flags = 0,
	};

	ret = ubi_secure_generate_salt(prefix.salt);
	if (ret != 0) {
		LOG_ERR("Salt generation failed for VID write");
		ubi_secure_destroy_key(child_key_id);
		return ret;
	}

	ubi_secure_encode_counter48(counter, prefix.counter);
	memset(prefix.reserved, 0, sizeof(prefix.reserved));

	/* Build output buffer. */
	uint8_t out_buf[UBI_SECURE_DATA_VID_SIZE] = { 0 };

	ubi_secure_prefix32_serialize(&prefix, out_buf);

	/* Build nonce. */
	uint8_t nonce[UBI_SECURE_NONCE_SIZE] = { 0 };

	ubi_secure_build_nonce(prefix.domain, prefix.salt, prefix.counter, nonce);

	/* Build AAD. */
	const size_t offset = peb_idx * mtd->erase_block_size + UBI_SECURE_EC_HDR_SIZE;
	uint8_t aad[UBI_SECURE_DATA_VID_AAD_SIZE] = { 0 };

	ubi_secure_build_data_vid_aad(out_buf, (uint32_t)peb_idx, offset, ec_ctx->ec,
				      ec_ctx->key_version, aad);

	/* Build plaintext: vid_hdr + vid_secure_meta. */
	uint8_t plaintext[UBI_SECURE_DATA_VID_PLAINTEXT_SIZE] = { 0 };

	memcpy(plaintext, vid_hdr, UBI_SECURE_PLAIN_VID_HDR_SIZE);
	ubi_secure_vid_meta_serialize(vid_meta, &plaintext[UBI_SECURE_PLAIN_VID_HDR_SIZE]);

	/* Encrypt. */
	size_t ct_len = 0;

	ret = ubi_secure_aead_encrypt(child_key_id, nonce, aad, sizeof(aad), plaintext,
				      sizeof(plaintext), &out_buf[UBI_SECURE_PREFIX_SIZE],
				      UBI_SECURE_DATA_VID_CT_TAG_SIZE, &ct_len);
	ubi_secure_destroy_key(child_key_id);

	if (ret != 0) {
		LOG_ERR("VID AEAD encrypt failed at PEB %zu", peb_idx);
		return ret;
	}

	/* Write to flash. */
	const struct flash_area *fa = NULL;

	ret = flash_area_open(mtd->partition_id, &fa);
	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		return -EIO;
	}

	ret = flash_area_write(fa, offset, out_buf, sizeof(out_buf));
	flash_area_close(fa);

	if (ret != 0) {
		LOG_ERR("Flash write failure at PEB %zu VID", peb_idx);
		return -EIO;
	}

	return 0;
}

int ubi_secure_leb_data_read(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			     size_t peb_idx, const struct ubi_secure_vid_auth_ctx *vid_ctx,
			     size_t offset, void *buf, size_t len)
{
	if (mtd == NULL || crypto_cfg == NULL || vid_ctx == NULL || vid_ctx->vid_hdr == NULL) {
		LOG_ERR("leb_data_read: NULL argument");
		return -EINVAL;
	}

	if (buf == NULL && len != 0) {
		LOG_ERR("leb_data_read: NULL buf with len %zu", len);
		return -EINVAL;
	}

	const uint32_t data_size = vid_ctx->vid_hdr->data_size;

	/* Zero-length record: nothing to read. */
	if (data_size == 0) {
		if (len != 0) {
			LOG_ERR("LEB read len %zu but data_size is 0 at PEB %zu", len, peb_idx);
			return -EINVAL;
		}
		return 0;
	}

	/* Validate requested slice fits within authenticated payload. */
	if (offset + len > data_size) {
		LOG_ERR("LEB read out of bounds: off=%zu len=%zu data_size=%u at PEB %zu", offset,
			len, data_size, peb_idx);
		return -EINVAL;
	}

	/* Read prefix from flash first to obtain the authoritative key_version. */
	const size_t leb_offset = peb_idx * mtd->erase_block_size + UBI_SECURE_LEB_OFFSET;
	const struct flash_area *fa = NULL;
	int ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		return -EIO;
	}

	uint8_t prefix_buf[UBI_SECURE_PREFIX_SIZE] = { 0 };

	ret = flash_area_read(fa, leb_offset, prefix_buf, sizeof(prefix_buf));
	if (ret != 0) {
		LOG_ERR("Flash read failure at PEB %zu LEB prefix", peb_idx);
		flash_area_close(fa);
		return -EIO;
	}

	/* Deserialize and validate prefix before any key derivation. */
	struct ubi_crypto_prefix32 prefix = { 0 };

	ubi_secure_prefix32_deserialize(prefix_buf, &prefix);

	if (prefix.magic != UBI_SECURE_PREFIX_MAGIC || prefix.domain != UBI_SECURE_DOMAIN_LEB) {
		LOG_ERR("Bad LEB prefix at PEB %zu", peb_idx);
		flash_area_close(fa);
		return -EBADMSG;
	}

	/* Derive LEB key using the prefix's authoritative key_version. */
	uint32_t child_key_id = 0;

	ret = ubi_secure_derive_leb_key(crypto_cfg, prefix.key_version, vid_ctx->vid_hdr->vol_id,
					&child_key_id);
	if (ret != 0) {
		LOG_ERR("LEB key derivation failed at PEB %zu", peb_idx);
		flash_area_close(fa);
		return ret;
	}

	/* Read ciphertext+tag. */
	const size_t ct_tag_size = (size_t)data_size + UBI_SECURE_TAG_SIZE;
	const size_t scratch_size = ct_tag_size + (size_t)data_size;
	uint8_t *scratch = NULL;

	ret = ubi_mem_scratch_alloc(scratch_size, &scratch);
	if (ret != 0) {
		LOG_ERR("Cannot allocate %zu bytes for LEB decrypt", scratch_size);
		flash_area_close(fa);
		ubi_secure_destroy_key(child_key_id);
		return -ENOMEM;
	}

	uint8_t *const ct_buf = scratch;
	uint8_t *const pt_buf = &scratch[ct_tag_size];

	ret = flash_area_read(fa, leb_offset + UBI_SECURE_PREFIX_SIZE, ct_buf, ct_tag_size);
	flash_area_close(fa);

	if (ret != 0) {
		LOG_ERR("Flash read failure at PEB %zu LEB data", peb_idx);
		ubi_mem_scratch_free(scratch);
		ubi_secure_destroy_key(child_key_id);
		return -EIO;
	}

	/* Build nonce. */
	uint8_t nonce[UBI_SECURE_NONCE_SIZE] = { 0 };

	ubi_secure_build_nonce(prefix.domain, prefix.salt, prefix.counter, nonce);

	/* Build AAD. */
	const struct ubi_vid_hdr *vh = vid_ctx->vid_hdr;
	uint8_t aad[UBI_SECURE_LEB_AAD_SIZE] = { 0 };

	ubi_secure_build_leb_aad(prefix_buf, (uint32_t)peb_idx, leb_offset, vid_ctx->ec_ctx.ec,
				 vid_ctx->ec_ctx.key_version, vh->vol_id, vh->lnum, vh->sqnum,
				 data_size, vid_ctx->key_version, aad);

	/* Decrypt full payload into pt_buf (non-overlapping with ct_buf). */
	size_t pt_len = 0;

	ret = ubi_secure_aead_decrypt(child_key_id, nonce, aad, sizeof(aad), ct_buf, ct_tag_size,
				      pt_buf, data_size, &pt_len);
	ubi_secure_destroy_key(child_key_id);

	if (ret != 0) {
		LOG_ERR("LEB AEAD decrypt failed at PEB %zu", peb_idx);
		ubi_secure_zeroize(scratch, scratch_size);
		ubi_mem_scratch_free(scratch);
		return -EBADMSG;
	}

	if (pt_len != data_size) {
		LOG_ERR("LEB unexpected plaintext size: %zu vs %u", pt_len, data_size);
		ubi_secure_zeroize(scratch, scratch_size);
		ubi_mem_scratch_free(scratch);
		return -UBI_SECURE_EFORMAT;
	}

	/* Return requested slice. */
	memcpy(buf, &pt_buf[offset], len);
	ubi_secure_zeroize(scratch, scratch_size);
	ubi_mem_scratch_free(scratch);

	return 0;
}

int ubi_secure_leb_data_write(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			      size_t peb_idx, const struct ubi_secure_ec_auth_ctx *ec_ctx,
			      const struct ubi_vid_hdr *vid_hdr, uint8_t vid_kv, const void *buf,
			      size_t len, uint8_t key_version, uint64_t counter)
{
	if (mtd == NULL || crypto_cfg == NULL || ec_ctx == NULL || vid_hdr == NULL) {
		LOG_ERR("leb_data_write: NULL argument");
		return -EINVAL;
	}

	if (buf == NULL && len != 0) {
		LOG_ERR("leb_data_write: NULL buf with len %zu", len);
		return -EINVAL;
	}

	/* Derive LEB key for {key_version, volume_id}. */
	uint32_t child_key_id = 0;
	int ret =
		ubi_secure_derive_leb_key(crypto_cfg, key_version, vid_hdr->vol_id, &child_key_id);

	if (ret != 0) {
		LOG_ERR("LEB key derivation failed for write at PEB %zu", peb_idx);
		return ret;
	}

	/* Build prefix. */
	struct ubi_crypto_prefix32 prefix = {
		.magic = UBI_SECURE_PREFIX_MAGIC,
		.wrapper_version = UBI_SECURE_WRAPPER_VERSION,
		.domain = UBI_SECURE_DOMAIN_LEB,
		.key_version = key_version,
		.flags = 0,
	};

	ret = ubi_secure_generate_salt(prefix.salt);
	if (ret != 0) {
		LOG_ERR("Salt generation failed for LEB write");
		ubi_secure_destroy_key(child_key_id);
		return ret;
	}

	ubi_secure_encode_counter48(counter, prefix.counter);
	memset(prefix.reserved, 0, sizeof(prefix.reserved));

	/* Serialize prefix. */
	uint8_t prefix_buf[UBI_SECURE_PREFIX_SIZE] = { 0 };

	ubi_secure_prefix32_serialize(&prefix, prefix_buf);

	/* Build nonce. */
	uint8_t nonce[UBI_SECURE_NONCE_SIZE] = { 0 };

	ubi_secure_build_nonce(prefix.domain, prefix.salt, prefix.counter, nonce);

	/* Build AAD. */
	const size_t leb_offset = peb_idx * mtd->erase_block_size + UBI_SECURE_LEB_OFFSET;
	uint8_t aad[UBI_SECURE_LEB_AAD_SIZE] = { 0 };

	ubi_secure_build_leb_aad(prefix_buf, (uint32_t)peb_idx, leb_offset, ec_ctx->ec,
				 ec_ctx->key_version, vid_hdr->vol_id, vid_hdr->lnum,
				 vid_hdr->sqnum, vid_hdr->data_size, vid_kv, aad);

	/* Encrypt. */
	const size_t ct_tag_size = len + UBI_SECURE_TAG_SIZE;
	const size_t ct_write_size = ROUND_UP(ct_tag_size, mtd->write_block_size);
	uint8_t *ct_buf = NULL;

	ret = ubi_mem_scratch_alloc(ct_write_size, &ct_buf);
	if (ret != 0) {
		LOG_ERR("Cannot allocate %zu bytes for LEB encrypt", ct_write_size);
		ubi_secure_destroy_key(child_key_id);
		return -ENOMEM;
	}

	/* Zero-fill padding region so flash sees deterministic bytes. */
	if (ct_write_size > ct_tag_size) {
		memset(&ct_buf[ct_tag_size], 0, ct_write_size - ct_tag_size);
	}

	size_t ct_len = 0;
	const uint8_t *plaintext = (len > 0) ? (const uint8_t *)buf : NULL;

	ret = ubi_secure_aead_encrypt(child_key_id, nonce, aad, sizeof(aad), plaintext, len, ct_buf,
				      ct_tag_size, &ct_len);
	ubi_secure_destroy_key(child_key_id);

	if (ret != 0) {
		LOG_ERR("LEB AEAD encrypt failed at PEB %zu", peb_idx);
		ubi_mem_scratch_free(ct_buf);
		return ret;
	}

	/* Write prefix + ciphertext+tag to flash. */
	const struct flash_area *fa = NULL;

	ret = flash_area_open(mtd->partition_id, &fa);
	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		ubi_mem_scratch_free(ct_buf);
		return -EIO;
	}

	ret = flash_area_write(fa, leb_offset, prefix_buf, sizeof(prefix_buf));
	if (ret != 0) {
		LOG_ERR("Flash write failure at PEB %zu LEB prefix", peb_idx);
		flash_area_close(fa);
		ubi_mem_scratch_free(ct_buf);
		return -EIO;
	}

	ret = flash_area_write(fa, leb_offset + UBI_SECURE_PREFIX_SIZE, ct_buf, ct_write_size);
	if (ret != 0) {
		LOG_ERR("Flash write failure at PEB %zu LEB data", peb_idx);
		flash_area_close(fa);
		ubi_mem_scratch_free(ct_buf);
		return -EIO;
	}

	flash_area_close(fa);
	ubi_mem_scratch_free(ct_buf);
	return 0;
}

int ubi_secure_vid_region_is_erased(const struct ubi_mtd *mtd, size_t peb_idx, bool *is_erased)
{
	if (mtd == NULL || is_erased == NULL) {
		LOG_ERR("vid_region_is_erased: NULL argument");
		return -EINVAL;
	}

	const struct flash_area *fa = NULL;
	int ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		return -EIO;
	}

	/* Read magic field of the VID region prefix. */
	uint8_t buf[sizeof(uint32_t)] = { 0 };
	const size_t offset = peb_idx * mtd->erase_block_size + UBI_SECURE_EC_HDR_SIZE;

	ret = flash_area_read(fa, offset, buf, sizeof(buf));
	flash_area_close(fa);

	if (ret != 0) {
		LOG_ERR("Flash read failure at PEB %zu VID region", peb_idx);
		return -EIO;
	}

	uint8_t erased_val = 0;

	ret = ubi_get_erased_val(mtd, &erased_val);
	if (ret != 0) {
		LOG_ERR("get_erased_val failed: %d", ret);
		return ret;
	}

	*is_erased = ubi_buf_is_erased(buf, sizeof(buf), erased_val);
	return 0;
}

int ubi_secure_leb_prefix_is_erased(const struct ubi_mtd *mtd, size_t peb_idx, bool *is_erased)
{
	if (mtd == NULL || is_erased == NULL) {
		LOG_ERR("leb_prefix_is_erased: NULL argument");
		return -EINVAL;
	}

	const struct flash_area *fa = NULL;
	int ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		return -EIO;
	}

	/* Read magic field of the LEB region prefix. */
	uint8_t buf[sizeof(uint32_t)] = { 0 };
	const size_t offset = peb_idx * mtd->erase_block_size + UBI_SECURE_LEB_OFFSET;

	ret = flash_area_read(fa, offset, buf, sizeof(buf));
	flash_area_close(fa);

	if (ret != 0) {
		LOG_ERR("Flash read failure at PEB %zu LEB prefix", peb_idx);
		return -EIO;
	}

	uint8_t erased_val = 0;

	ret = ubi_get_erased_val(mtd, &erased_val);
	if (ret != 0) {
		LOG_ERR("get_erased_val failed: %d", ret);
		return ret;
	}

	*is_erased = ubi_buf_is_erased(buf, sizeof(buf), erased_val);
	return 0;
}
