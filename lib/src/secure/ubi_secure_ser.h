/**
 * \file    ubi_secure_ser.h
 * \author  Kamil Kielbasa
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

/**
 * \brief Build AAD for a secure EC header (44 bytes).
 *
 * Layout: prefix32(32) + be32(peb_index)(4) + be64(flash_offset)(8)
 *
 * \param[in]  prefix       Serialized prefix bytes (32).
 * \param      peb_index    Data PEB physical index.
 * \param      flash_offset EC header offset from partition start.
 * \param[out] aad          Output buffer (at least UBI_SECURE_EC_HDR_AAD_SIZE).
 */
void ubi_secure_build_ec_hdr_aad(const uint8_t prefix[UBI_SECURE_PREFIX_SIZE], uint32_t peb_index,
				 uint64_t flash_offset, uint8_t aad[UBI_SECURE_EC_HDR_AAD_SIZE]);

/**
 * \brief Build AAD for a secure data-PEB VID header (53 bytes).
 *
 * Layout: prefix32(32) + be32(peb_index)(4) + be64(flash_offset)(8)
 *         + be64(ec)(8) + parent_ec_key_version(1)
 *
 * \param[in]  prefix       Serialized prefix bytes (32).
 * \param      peb_index    Data PEB physical index.
 * \param      flash_offset VID header offset from partition start.
 * \param      ec           Authenticated erase counter from EC header.
 * \param      parent_ec_kv Authenticated EC-header key_version.
 * \param[out] aad          Output buffer (at least UBI_SECURE_DATA_VID_AAD_SIZE).
 */
void ubi_secure_build_data_vid_aad(const uint8_t prefix[UBI_SECURE_PREFIX_SIZE], uint32_t peb_index,
				   uint64_t flash_offset, uint64_t ec, uint8_t parent_ec_kv,
				   uint8_t aad[UBI_SECURE_DATA_VID_AAD_SIZE]);

/**
 * \brief Build AAD for a secure LEB record, single-tag (74 bytes).
 *
 * Layout: prefix32(32) + be32(peb_index)(4) + be64(flash_offset)(8)
 *         + be64(ec)(8) + parent_ec_kv(1) + be32(vol_id)(4) + be32(lnum)(4)
 *         + be64(sqnum)(8) + be32(data_size)(4) + parent_vid_kv(1)
 *
 * \param[in]  prefix         Serialized prefix bytes (32).
 * \param      peb_index      Data PEB physical index.
 * \param      flash_offset   LEB data offset from partition start.
 * \param      ec             Authenticated erase counter.
 * \param      parent_ec_kv   Authenticated EC-header key_version.
 * \param      vol_id         Authenticated volume identifier.
 * \param      lnum           Authenticated logical erase block number.
 * \param      sqnum          Authenticated sequence number.
 * \param      data_size      Authenticated payload size.
 * \param      parent_vid_kv  Authenticated VID-header key_version.
 * \param[out] aad            Output buffer (at least UBI_SECURE_LEB_AAD_SIZE).
 */
void ubi_secure_build_leb_aad(const uint8_t prefix[UBI_SECURE_PREFIX_SIZE], uint32_t peb_index,
			      uint64_t flash_offset, uint64_t ec, uint8_t parent_ec_kv,
			      uint32_t vol_id, uint32_t lnum, uint64_t sqnum, uint32_t data_size,
			      uint8_t parent_vid_kv, uint8_t aad[UBI_SECURE_LEB_AAD_SIZE]);

/**
 * \brief Serialize vid_secure_meta to a byte buffer.
 *
 * \param[in]  meta Secure VID meta structure.
 * \param[out] buf  Output buffer (at least UBI_SECURE_VID_META_SIZE).
 */
void ubi_secure_vid_meta_serialize(const struct ubi_vid_secure_meta *meta, uint8_t *buf);

/**
 * \brief Deserialize vid_secure_meta from a byte buffer.
 *
 * \param[in]  buf  Input buffer (at least UBI_SECURE_VID_META_SIZE).
 * \param[out] meta Output meta structure.
 */
void ubi_secure_vid_meta_deserialize(const uint8_t *buf, struct ubi_vid_secure_meta *meta);

#endif /* UBI_SECURE_SER_H */
