/**
 * \file    ubi_secure_ser.h
 * \brief   Serialization and AAD construction for secure on-flash records.
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard ------------------------------------------------------------------------------- */
#ifndef UBI_SECURE_SER_H
#define UBI_SECURE_SER_H

/* Include files ------------------------------------------------------------------------------- */
#include "ubi_secure_types.h"
#include "ubi_io.h"

#include <stddef.h>
#include <stdint.h>

/* Function declarations ----------------------------------------------------------------------- */

/**
 * \brief Serialize a prefix32 struct into a 32-byte big-endian buffer.
 *
 * \param[in]  prefix  Source prefix.
 * \param[out] buf     Output buffer (at least UBI_SECURE_PREFIX_SIZE).
 */
void ubi_secure_prefix32_serialize(const struct ubi_crypto_prefix32 *prefix, uint8_t *buf);

/**
 * \brief Deserialize a 32-byte big-endian buffer into a prefix32 struct.
 *
 * \param[in]  buf     Input buffer (at least UBI_SECURE_PREFIX_SIZE).
 * \param[out] prefix  Output prefix.
 */
void ubi_secure_prefix32_deserialize(const uint8_t *buf, struct ubi_crypto_prefix32 *prefix);

/**
 * \brief Build AAD for a secure device header (44 bytes).
 *
 * Layout: prefix32(32) + be32(peb_index)(4) + be64(flash_offset)(8)
 *
 * \param[in]  prefix       Serialized prefix bytes (32).
 * \param      peb_index    Reserved PEB physical index.
 * \param      flash_offset Device header offset from partition start.
 * \param[out] aad          Output buffer (at least UBI_SECURE_DEV_HDR_AAD_SIZE).
 */
void ubi_secure_build_dev_hdr_aad(const uint8_t prefix[UBI_SECURE_PREFIX_SIZE], uint32_t peb_index,
				  uint64_t flash_offset, uint8_t aad[UBI_SECURE_DEV_HDR_AAD_SIZE]);

/**
 * \brief Build AAD for a secure volume header (53 bytes).
 *
 * Layout: prefix32(32) + be32(peb_index)(4) + be64(flash_offset)(8)
 *         + be64(device_revision)(8) + parent_key_version(1)
 *
 * \param[in]  prefix            Serialized prefix bytes (32).
 * \param      peb_index         Reserved PEB physical index.
 * \param      flash_offset      Volume header offset from partition start.
 * \param      device_revision   Authenticated device_header.revision.
 * \param      parent_kv         Parent secure-device key_version.
 * \param[out] aad               Output buffer (at least UBI_SECURE_VOL_HDR_AAD_SIZE).
 */
void ubi_secure_build_vol_hdr_aad(const uint8_t prefix[UBI_SECURE_PREFIX_SIZE], uint32_t peb_index,
				  uint64_t flash_offset, uint64_t device_revision,
				  uint8_t parent_kv, uint8_t aad[UBI_SECURE_VOL_HDR_AAD_SIZE]);

/**
 * \brief Serialize dev_secure_meta to a byte buffer.
 *
 * \param[in]  meta Secure meta structure.
 * \param[out] buf  Output buffer (at least UBI_SECURE_DEV_META_SIZE).
 */
void ubi_secure_dev_meta_serialize(const struct ubi_dev_secure_meta *meta, uint8_t *buf);

/**
 * \brief Deserialize dev_secure_meta from a byte buffer.
 *
 * \param[in]  buf  Input buffer (at least UBI_SECURE_DEV_META_SIZE).
 * \param[out] meta Output meta structure.
 */
void ubi_secure_dev_meta_deserialize(const uint8_t *buf, struct ubi_dev_secure_meta *meta);

/**
 * \brief Encode a 48-bit counter value into 6 bytes big-endian.
 *
 * \param      value  Counter value (must fit in 48 bits).
 * \param[out] buf    6-byte output buffer.
 */
void ubi_secure_encode_counter48(uint64_t value, uint8_t buf[UBI_SECURE_COUNTER_SIZE]);

/**
 * \brief Decode a 48-bit counter value from 6 bytes big-endian.
 *
 * \param[in] buf 6-byte input buffer.
 *
 * \return Decoded counter value.
 */
uint64_t ubi_secure_decode_counter48(const uint8_t buf[UBI_SECURE_COUNTER_SIZE]);

#endif /* UBI_SECURE_SER_H */
