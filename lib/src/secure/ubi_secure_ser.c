/**
 * \file    ubi_secure_ser.c
 * \author  Kamil Kielbasa
 * \brief   Serialization and AAD construction for secure on-flash records.
 *
 * Function definitions in this file are grouped by on-flash domain to mirror
 * the layout in ubi_secure_ser.h:
 *
 *   1. Common helpers (prefix32, counter48).
 *   2. DEV  header  : AAD builder + secure-meta serialize/deserialize.
 *   3. VOL  header  : AAD builder.
 *   4. EC   header  : AAD builder.
 *   5. VID  header  : AAD builder + secure-meta serialize/deserialize.
 *   6. LEB  record  : AAD builder.
 *   7. LEB  chunk   : AAD builder (CONFIG_UBI_CRYPTO_LEB_CHUNKED).
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_secure_ser.h"

/* Zephyr headers: */
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/byteorder.h>

/* Standard library headers: */
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Prefix32 field offsets (all derived from struct layout). */
#define PREFIX32_OFF_MAGIC (0)
#define PREFIX32_OFF_VERSION (PREFIX32_OFF_MAGIC + sizeof(uint32_t)) /* 4  */
#define PREFIX32_OFF_DOMAIN (PREFIX32_OFF_VERSION + sizeof(uint8_t)) /* 5  */
#define PREFIX32_OFF_KV (PREFIX32_OFF_DOMAIN + sizeof(uint8_t)) /* 6  */
#define PREFIX32_OFF_FLAGS (PREFIX32_OFF_KV + sizeof(uint8_t)) /* 7  */
#define PREFIX32_OFF_SALT (PREFIX32_OFF_FLAGS + sizeof(uint8_t)) /* 8  */
#define PREFIX32_OFF_COUNTER (PREFIX32_OFF_SALT + UBI_SECURE_SALT_SIZE) /* 14 */
#define PREFIX32_OFF_RESERVED (PREFIX32_OFF_COUNTER + UBI_SECURE_COUNTER_SIZE) /* 20 */

/* Dev meta field offsets. */
#define DEV_META_OFF_KV (0)
#define DEV_META_OFF_RESERVED (DEV_META_OFF_KV + sizeof(uint8_t)) /* 1  */
#define DEV_META_OFF_RESERVED_LEN (7)
#define DEV_META_OFF_COUNTER (DEV_META_OFF_RESERVED + DEV_META_OFF_RESERVED_LEN) /* 8 */

/* VID secure meta field offsets. */
#define VID_META_OFF_LEB_WRITE_COUNTER (0)
#define VID_META_OFF_LEB_TOTAL_AUTH_BYTES (VID_META_OFF_LEB_WRITE_COUNTER + sizeof(uint64_t))

/* Common helpers ------------------------------------------------------------------------------- */

void ubi_secure_prefix32_serialize(const struct ubi_crypto_prefix32 *prefix, uint8_t *buf)
{
	if (prefix == NULL || buf == NULL) {
		LOG_ERR("prefix32_serialize: NULL argument");
		return;
	}

	sys_put_be32(prefix->magic, &buf[PREFIX32_OFF_MAGIC]);
	buf[PREFIX32_OFF_VERSION] = prefix->wrapper_version;
	buf[PREFIX32_OFF_DOMAIN] = prefix->domain;
	buf[PREFIX32_OFF_KV] = prefix->key_version;
	buf[PREFIX32_OFF_FLAGS] = prefix->flags;
	memcpy(&buf[PREFIX32_OFF_SALT], prefix->salt, UBI_SECURE_SALT_SIZE);
	memcpy(&buf[PREFIX32_OFF_COUNTER], prefix->counter, UBI_SECURE_COUNTER_SIZE);
	memcpy(&buf[PREFIX32_OFF_RESERVED], prefix->reserved, UBI_SECURE_PREFIX_RESERVED_SIZE);
}

void ubi_secure_prefix32_deserialize(const uint8_t *buf, struct ubi_crypto_prefix32 *prefix)
{
	if (buf == NULL || prefix == NULL) {
		LOG_ERR("prefix32_deserialize: NULL argument");
		return;
	}

	prefix->magic = sys_get_be32(&buf[PREFIX32_OFF_MAGIC]);
	prefix->wrapper_version = buf[PREFIX32_OFF_VERSION];
	prefix->domain = buf[PREFIX32_OFF_DOMAIN];
	prefix->key_version = buf[PREFIX32_OFF_KV];
	prefix->flags = buf[PREFIX32_OFF_FLAGS];
	memcpy(prefix->salt, &buf[PREFIX32_OFF_SALT], UBI_SECURE_SALT_SIZE);
	memcpy(prefix->counter, &buf[PREFIX32_OFF_COUNTER], UBI_SECURE_COUNTER_SIZE);
	memcpy(prefix->reserved, &buf[PREFIX32_OFF_RESERVED], UBI_SECURE_PREFIX_RESERVED_SIZE);
}

void ubi_secure_encode_counter48(uint64_t value, uint8_t buf[UBI_SECURE_COUNTER_SIZE])
{
	if (buf == NULL || value > UBI_SECURE_COUNTER_MAX) {
		LOG_ERR("encode_counter48: invalid argument");
		return;
	}

	buf[0] = (uint8_t)(value >> 40);
	buf[1] = (uint8_t)(value >> 32);
	buf[2] = (uint8_t)(value >> 24);
	buf[3] = (uint8_t)(value >> 16);
	buf[4] = (uint8_t)(value >> 8);
	buf[5] = (uint8_t)(value);
}

uint64_t ubi_secure_decode_counter48(const uint8_t buf[UBI_SECURE_COUNTER_SIZE])
{
	if (buf == NULL) {
		LOG_ERR("decode_counter48: NULL argument");
		return 0;
	}

	return ((uint64_t)buf[0] << 40) | ((uint64_t)buf[1] << 32) | ((uint64_t)buf[2] << 24) |
	       ((uint64_t)buf[3] << 16) | ((uint64_t)buf[4] << 8) | ((uint64_t)buf[5]);
}

/* DEV header ----------------------------------------------------------------------------------- */

void ubi_secure_build_dev_hdr_aad(const struct ubi_secure_dev_hdr_aad_input *input,
				  uint8_t aad[UBI_SECURE_DEV_HDR_AAD_SIZE])
{
	if (input == NULL || input->prefix == NULL || aad == NULL) {
		LOG_ERR("build_dev_hdr_aad: NULL argument");
		return;
	}

	size_t pos = 0;

	memcpy(&aad[pos], input->prefix, UBI_SECURE_PREFIX_SIZE);
	pos += UBI_SECURE_PREFIX_SIZE;

	sys_put_be32(input->peb_index, &aad[pos]);
	pos += sizeof(uint32_t);

	sys_put_be64(input->flash_offset, &aad[pos]);
	pos += sizeof(uint64_t);

	__ASSERT_NO_MSG(pos == UBI_SECURE_DEV_HDR_AAD_SIZE);
}

void ubi_secure_dev_meta_serialize(const struct ubi_dev_secure_meta *meta, uint8_t *buf)
{
	if (meta == NULL || buf == NULL) {
		LOG_ERR("dev_meta_serialize: NULL argument");
		return;
	}

	buf[DEV_META_OFF_KV] = meta->write_active_key_version;
	memset(&buf[DEV_META_OFF_RESERVED], 0, DEV_META_OFF_RESERVED_LEN);
	sys_put_be64(meta->vid_next_counter_floor, &buf[DEV_META_OFF_COUNTER]);
}

void ubi_secure_dev_meta_deserialize(const uint8_t *buf, struct ubi_dev_secure_meta *meta)
{
	if (buf == NULL || meta == NULL) {
		LOG_ERR("dev_meta_deserialize: NULL argument");
		return;
	}

	meta->write_active_key_version = buf[DEV_META_OFF_KV];
	memset(meta->reserved0, 0, sizeof(meta->reserved0));
	meta->vid_next_counter_floor = sys_get_be64(&buf[DEV_META_OFF_COUNTER]);
}

/* VOL header ----------------------------------------------------------------------------------- */

void ubi_secure_build_vol_hdr_aad(const struct ubi_secure_vol_hdr_aad_input *input,
				  uint8_t aad[UBI_SECURE_VOL_HDR_AAD_SIZE])
{
	if (input == NULL || input->prefix == NULL || aad == NULL) {
		LOG_ERR("build_vol_hdr_aad: NULL argument");
		return;
	}

	size_t pos = 0;

	memcpy(&aad[pos], input->prefix, UBI_SECURE_PREFIX_SIZE);
	pos += UBI_SECURE_PREFIX_SIZE;

	sys_put_be32(input->peb_index, &aad[pos]);
	pos += sizeof(uint32_t);

	sys_put_be64(input->flash_offset, &aad[pos]);
	pos += sizeof(uint64_t);

	sys_put_be64(input->device_revision, &aad[pos]);
	pos += sizeof(uint64_t);

	aad[pos] = input->parent_kv;
	pos += sizeof(uint8_t);

	__ASSERT_NO_MSG(pos == UBI_SECURE_VOL_HDR_AAD_SIZE);
}

/* EC header ------------------------------------------------------------------------------------ */

void ubi_secure_build_ec_hdr_aad(const struct ubi_secure_ec_hdr_aad_input *input,
				 uint8_t aad[UBI_SECURE_EC_HDR_AAD_SIZE])
{
	if (input == NULL || input->prefix == NULL || aad == NULL) {
		LOG_ERR("build_ec_hdr_aad: NULL argument");
		return;
	}

	size_t pos = 0;

	memcpy(&aad[pos], input->prefix, UBI_SECURE_PREFIX_SIZE);
	pos += UBI_SECURE_PREFIX_SIZE;

	sys_put_be32(input->peb_index, &aad[pos]);
	pos += sizeof(uint32_t);

	sys_put_be64(input->flash_offset, &aad[pos]);
	pos += sizeof(uint64_t);

	__ASSERT_NO_MSG(pos == UBI_SECURE_EC_HDR_AAD_SIZE);
}

/* VID header ----------------------------------------------------------------------------------- */

void ubi_secure_build_vid_hdr_aad(const struct ubi_secure_vid_hdr_aad_input *input,
				  uint8_t aad[UBI_SECURE_VID_HDR_AAD_SIZE])
{
	if (input == NULL || input->prefix == NULL || aad == NULL) {
		LOG_ERR("build_vid_hdr_aad: NULL argument");
		return;
	}

	size_t pos = 0;

	memcpy(&aad[pos], input->prefix, UBI_SECURE_PREFIX_SIZE);
	pos += UBI_SECURE_PREFIX_SIZE;

	sys_put_be32(input->peb_index, &aad[pos]);
	pos += sizeof(uint32_t);

	sys_put_be64(input->flash_offset, &aad[pos]);
	pos += sizeof(uint64_t);

	sys_put_be64(input->ec, &aad[pos]);
	pos += sizeof(uint64_t);

	aad[pos] = input->parent_ec_kv;
	pos += sizeof(uint8_t);

	__ASSERT_NO_MSG(pos == UBI_SECURE_VID_HDR_AAD_SIZE);
}

void ubi_secure_vid_meta_serialize(const struct ubi_vid_secure_meta *meta, uint8_t *buf)
{
	if (meta == NULL || buf == NULL) {
		LOG_ERR("vid_meta_serialize: NULL argument");
		return;
	}

	sys_put_be64(meta->leb_write_counter, &buf[VID_META_OFF_LEB_WRITE_COUNTER]);
	sys_put_be64(meta->leb_total_auth_bytes, &buf[VID_META_OFF_LEB_TOTAL_AUTH_BYTES]);
}

void ubi_secure_vid_meta_deserialize(const uint8_t *buf, struct ubi_vid_secure_meta *meta)
{
	if (buf == NULL || meta == NULL) {
		LOG_ERR("vid_meta_deserialize: NULL argument");
		return;
	}

	meta->leb_write_counter = sys_get_be64(&buf[VID_META_OFF_LEB_WRITE_COUNTER]);
	meta->leb_total_auth_bytes = sys_get_be64(&buf[VID_META_OFF_LEB_TOTAL_AUTH_BYTES]);
}

/* LEB record ----------------------------------------------------------------------------------- */

void ubi_secure_build_leb_aad(const struct ubi_secure_leb_aad_input *input,
			      uint8_t aad[UBI_SECURE_LEB_AAD_SIZE])
{
	if (input == NULL || input->prefix == NULL || aad == NULL) {
		LOG_ERR("build_leb_aad: NULL argument");
		return;
	}

	size_t pos = 0;

	memcpy(&aad[pos], input->prefix, UBI_SECURE_PREFIX_SIZE);
	pos += UBI_SECURE_PREFIX_SIZE;

	sys_put_be32(input->peb_index, &aad[pos]);
	pos += sizeof(uint32_t);

	sys_put_be64(input->flash_offset, &aad[pos]);
	pos += sizeof(uint64_t);

	sys_put_be64(input->ec, &aad[pos]);
	pos += sizeof(uint64_t);

	aad[pos] = input->parent_ec_kv;
	pos += sizeof(uint8_t);

	sys_put_be32(input->vol_id, &aad[pos]);
	pos += sizeof(uint32_t);

	sys_put_be32(input->lnum, &aad[pos]);
	pos += sizeof(uint32_t);

	sys_put_be64(input->sqnum, &aad[pos]);
	pos += sizeof(uint64_t);

	sys_put_be32(input->data_size, &aad[pos]);
	pos += sizeof(uint32_t);

	aad[pos] = input->parent_vid_kv;
	pos += sizeof(uint8_t);

	__ASSERT_NO_MSG(pos == UBI_SECURE_LEB_AAD_SIZE);
}

/* LEB chunk ------------------------------------------------------------------------------------ */

#if defined(CONFIG_UBI_CRYPTO_LEB_CHUNKED)
void ubi_secure_build_leb_chunk_aad(const struct ubi_secure_leb_chunk_aad_input *input,
				    uint8_t aad[UBI_SECURE_LEB_CHUNK_AAD_SIZE])
{
	if (input == NULL || aad == NULL) {
		LOG_ERR("build_leb_chunk_aad: NULL argument");
		return;
	}

	/* Reuse single-tag AAD for the first 74 bytes. */
	ubi_secure_build_leb_aad(&input->leb, aad);

	/* Append be32(chunk_index) at offset 74 → total 78. */
	sys_put_be32(input->chunk_index, &aad[UBI_SECURE_LEB_AAD_SIZE]);
}
#endif /* CONFIG_UBI_CRYPTO_LEB_CHUNKED */
