/**
 * \file    ubi_secure_types.h
 * \author  Kamil Kielbasa
 * \brief   Internal types for the UBI secure backend.
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard -------------------------------------------------------------------------------- */

#ifndef UBI_SECURE_TYPES_H
#define UBI_SECURE_TYPES_H

/* Include files -------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_plain_io.h"

/* Zephyr headers: */
#include <zephyr/sys/__assert.h>

/* Third-party headers: */
#include <mbedtls/platform_util.h>

/* Standard library headers: */
#include <stddef.h>
#include <stdint.h>

/* Defines -------------------------------------------------------------------------------------- */

/** Magic for secure wrapper prefix. */
#define UBI_SECURE_PREFIX_MAGIC (0x55424953U) /* 'UBIS' */

/** Current secure on-flash format version. */
#define UBI_SECURE_WRAPPER_VERSION (1)

/** Size of the common prefix in bytes. */
#define UBI_SECURE_PREFIX_SIZE (32)

/** Size of the AEAD authentication tag in bytes. */
#define UBI_SECURE_TAG_SIZE (16)

/** Size of the salt field in the prefix. */
#define UBI_SECURE_SALT_SIZE (6)

/** Size of the counter field in the prefix. */
#define UBI_SECURE_COUNTER_SIZE (6)

/** Maximum value representable in a 48-bit counter. */
#define UBI_SECURE_COUNTER_MAX (0xFFFFFFFFFFFFULL)

/** Maximum authenticated payload (bytes) for a single AES-128-CCM
 *  invocation with q = 2 (length-field width = 2 bytes): the encoded
 *  payload length must fit in 2 bytes, so payload_len < 2^16 = 65536,
 *  i.e. up to 65535 inclusive.  Used as a runtime guard on the secure
 *  LEB single-tag IO path to reject payloads that would overflow the
 *  CCM length field. */
#define UBI_SECURE_LEB_SINGLE_TAG_MAX_PAYLOAD (65535U)

/** Size of the reserved field in the prefix. */
#define UBI_SECURE_PREFIX_RESERVED_SIZE (12)

/** Byte offset of domain field within serialized prefix32. */
#define UBI_SECURE_PREFIX_OFF_DOMAIN (5)

/** Byte offset of key_version field within serialized prefix32. */
#define UBI_SECURE_PREFIX_OFF_KEY_VERSION (6)

/* Internal error codes (outside POSIX errno range) --------------------------------------------- */

/** Salt generation (RNG) failure — distinct from generic -EIO. */
#define UBI_SECURE_ENORAND 201

/** Key-ID lookup failure — key version not available from get_key_id callback. */
#define UBI_SECURE_ENOKEY 202

/** Post-AEAD format violation — authentic data with invalid structure. */
#define UBI_SECURE_EFORMAT 203

/* Utility helpers ------------------------------------------------------------------------------ */

/**
 * \brief Compiler-safe zeroization of a buffer (will not be optimized away).
 *
 * Delegates to mbedtls_platform_zeroize which is specifically designed to
 * resist compiler dead-store elimination.
 *
 * \param[in,out] buf  Buffer to clear.
 * \param[in]     len  Number of bytes.
 */
static inline void ubi_secure_zeroize(void *buf, size_t len)
{
	__ASSERT_NO_MSG(buf != NULL);

	mbedtls_platform_zeroize(buf, len);
}

/** CCM nonce size = domain(1) + salt(6) + counter(6). */
#define UBI_SECURE_NONCE_SIZE (13)

/** HKDF-SHA-256 child key output length. */
#define UBI_SECURE_KEY_SIZE (16)

/** Size of secure device header on flash: prefix(32) + ciphertext(32+16) + tag(16). */
#define UBI_SECURE_DEV_HDR_SIZE (96)

/** Size of secure volume header on flash: prefix(32) + ciphertext(48) + tag(16). */
#define UBI_SECURE_VOL_HDR_SIZE (96)

/** Size of struct ubi_dev_secure_meta. */
#define UBI_SECURE_DEV_META_SIZE (16)

/** Plaintext payload for secure device header: dev_hdr(32) + dev_secure_meta(16) = 48. */
#define UBI_SECURE_DEV_HDR_PLAINTEXT_SIZE (UBI_DEV_HDR_SIZE + UBI_SECURE_DEV_META_SIZE)

/** Ciphertext+tag for secure device header: 48 + 16 = 64. */
#define UBI_SECURE_DEV_HDR_CT_TAG_SIZE (UBI_SECURE_DEV_HDR_PLAINTEXT_SIZE + UBI_SECURE_TAG_SIZE)

/** Plaintext payload for secure volume header: vol_hdr(48). */
#define UBI_SECURE_VOL_HDR_PLAINTEXT_SIZE (UBI_VOL_HDR_SIZE)

/** Ciphertext+tag for secure volume header: 48 + 16 = 64. */
#define UBI_SECURE_VOL_HDR_CT_TAG_SIZE (UBI_SECURE_VOL_HDR_PLAINTEXT_SIZE + UBI_SECURE_TAG_SIZE)

/* Data-PEB secure record sizes ----------------------------------------------------------------- */

/** Size of the plain EC header payload in bytes. */
#define UBI_SECURE_PLAIN_EC_HDR_SIZE (16)

/** Size of the plain VID header payload in bytes. */
#define UBI_SECURE_PLAIN_VID_HDR_SIZE (32)

/** Size of secure EC header on flash: prefix(32) + ciphertext(ec_hdr=16) + tag(16). */
#define UBI_SECURE_EC_HDR_SIZE (64)

/** Plaintext payload for EC header: ec_hdr(16). */
#define UBI_SECURE_EC_PLAINTEXT_SIZE (UBI_SECURE_PLAIN_EC_HDR_SIZE)

/** Ciphertext+tag for EC header: 16 + 16 = 32. */
#define UBI_SECURE_EC_CT_TAG_SIZE (UBI_SECURE_EC_PLAINTEXT_SIZE + UBI_SECURE_TAG_SIZE)

/** Size of struct ubi_vid_secure_meta. */
#define UBI_SECURE_VID_META_SIZE (16)

/** Size of secure data-PEB VID on flash: prefix(32) + ciphertext(vid_hdr=32 + vid_meta=16) + tag(16). */
#define UBI_SECURE_DATA_VID_SIZE (96)

/** Plaintext payload for data-PEB VID: vid_hdr(32) + vid_secure_meta(16) = 48. */
#define UBI_SECURE_DATA_VID_PLAINTEXT_SIZE \
	(UBI_SECURE_PLAIN_VID_HDR_SIZE + UBI_SECURE_VID_META_SIZE)

/** Ciphertext+tag for data-PEB VID: 48 + 16 = 64. */
#define UBI_SECURE_DATA_VID_CT_TAG_SIZE (UBI_SECURE_DATA_VID_PLAINTEXT_SIZE + UBI_SECURE_TAG_SIZE)

/** Offset of secure LEB data region within a data PEB: EC(64) + VID(96) = 160. */
#define UBI_SECURE_LEB_OFFSET (UBI_SECURE_EC_HDR_SIZE + UBI_SECURE_DATA_VID_SIZE)

/** Fixed overhead per secure LEB record: prefix(32) + tag(16) = 48. */
#define UBI_SECURE_LEB_OVERHEAD (UBI_SECURE_PREFIX_SIZE + UBI_SECURE_TAG_SIZE)

/* Crypto domains ------------------------------------------------------------------------------- */

/**
 * \brief Domain byte values for the prefix and nonce.
 *
 * Each on-flash record belongs to exactly one domain. The domain byte
 * is embedded in the 32-byte prefix and in the 13-byte AEAD nonce.
 * It controls which HKDF child key is derived for that record.
 */
enum ubi_secure_domain {
	UBI_SECURE_DOMAIN_DEVICE_HEADER = 0, /*!< Reserved-PEB device header. */
	UBI_SECURE_DOMAIN_VOLUME_HEADER = 1, /*!< Reserved-PEB volume header. */
	UBI_SECURE_DOMAIN_ERASE_COUNTER = 2, /*!< Data-PEB erase counter header. */
	UBI_SECURE_DOMAIN_VOLUME_IDENTIFIER = 3, /*!< Data-PEB volume identifier header. */
	UBI_SECURE_DOMAIN_LEB = 4, /*!< Data-PEB LEB payload. */
	UBI_SECURE_DOMAIN_COUNT, /*!< Number of defined secure domains. */
};

/* Types and type definitions ------------------------------------------------------------------- */

/**
 * \brief Common 32-byte prefix for all secure on-flash records.
 *
 * All multi-byte integers serialized in big-endian.
 */
struct ubi_crypto_prefix32 {
	uint32_t magic; /*!< UBI_SECURE_PREFIX_MAGIC */
	uint8_t wrapper_version; /*!< UBI_SECURE_WRAPPER_VERSION */
	uint8_t domain; /*!< enum ubi_secure_domain */
	uint8_t key_version; /*!< IKM[v] version */
	uint8_t flags; /*!< Record flags (zero in v1) */
	uint8_t salt[UBI_SECURE_SALT_SIZE]; /*!< Fresh RNG salt */
	uint8_t counter[UBI_SECURE_COUNTER_SIZE]; /*!< Monotonic AEAD counter */
	uint8_t reserved[UBI_SECURE_PREFIX_RESERVED_SIZE]; /*!< Zero in v1 */
};
BUILD_ASSERT(sizeof(struct ubi_crypto_prefix32) == UBI_SECURE_PREFIX_SIZE);

/**
 * \brief Secure device-header crypto metadata (encrypted alongside dev_hdr).
 */
struct ubi_dev_secure_meta {
	uint8_t write_active_key_version; /*!< Authenticated current write-active version */
	uint8_t reserved0[7]; /*!< Zero in v1 */
	uint64_t vid_next_counter_floor; /*!< Next unused VID counter for write_active */
};
BUILD_ASSERT(sizeof(struct ubi_dev_secure_meta) == UBI_SECURE_DEV_META_SIZE);

/**
 * \brief Secure VID-side LEB metadata (encrypted alongside vid_hdr in data PEBs).
 *
 * These fields are the authoritative write-usage recovery state for
 * a {key_version, volume_id} pair.
 */
struct ubi_vid_secure_meta {
	uint64_t leb_write_counter; /*!< Next unused AEAD counter for {kv, vol_id}. */
	uint64_t leb_total_auth_bytes; /*!< Cumulative authenticated bytes (AAD + payload). */
};
BUILD_ASSERT(sizeof(struct ubi_vid_secure_meta) == UBI_SECURE_VID_META_SIZE);

/* Parent authentication context structures ----------------------------------------------------- */

/**
 * \brief Authenticated EC-header context — passed as parent to VID/LEB operations.
 *
 * Populated by ubi_secure_ec_hdr_read() on success.
 */
struct ubi_secure_ec_auth_ctx {
	uint64_t ec; /*!< Authenticated erase counter value. */
	uint64_t aead_counter; /*!< AEAD nonce counter from EC prefix. */
	uint8_t key_version; /*!< EC-header prefix key_version. */
};

/**
 * \brief Authenticated VID-header context — passed as parent to LEB operations.
 *
 * Bundles the EC parent chain and VID-specific fields needed for LEB AAD.
 */
struct ubi_secure_vid_auth_ctx {
	struct ubi_secure_ec_auth_ctx ec_ctx; /*!< Parent EC auth context. */
	const struct ubi_vid_hdr *vid_hdr; /*!< Authenticated VID header. */
	uint8_t key_version; /*!< VID-header prefix key_version. */
	uint64_t vid_counter; /*!< VID-domain AEAD counter from prefix32. */
};

/* AAD sizes ------------------------------------------------------------------------------------ */

/* Per-field AAD component sizes (canonical byte widths bound into AAD). */

/** AAD component: physical erase block index (uint32_t big-endian). */
#define UBI_SECURE_AAD_PEB_IDX_SIZE (sizeof(uint32_t))

/** AAD component: byte offset of the record on flash (uint64_t big-endian). */
#define UBI_SECURE_AAD_FLASH_OFFSET_SIZE (sizeof(uint64_t))

/** AAD component: parent EC value bound into VID/LEB AAD (uint64_t big-endian). */
#define UBI_SECURE_AAD_EC_SIZE (sizeof(uint64_t))

/** AAD component: parent key_version byte. */
#define UBI_SECURE_AAD_KV_SIZE (sizeof(uint8_t))

/** AAD component: device_revision bound into VOL_HDR AAD (uint64_t big-endian). */
#define UBI_SECURE_AAD_DEV_REVISION_SIZE (sizeof(uint64_t))

/** AAD component: volume identifier bound into LEB AAD (uint32_t big-endian). */
#define UBI_SECURE_AAD_VOL_ID_SIZE (sizeof(uint32_t))

/** AAD component: logical erase block number bound into LEB AAD (uint32_t big-endian). */
#define UBI_SECURE_AAD_LNUM_SIZE (sizeof(uint32_t))

/** AAD component: VID sequence number bound into LEB AAD (uint64_t big-endian). */
#define UBI_SECURE_AAD_SQNUM_SIZE (sizeof(uint64_t))

/** AAD component: data_size bound into LEB AAD (uint32_t big-endian). */
#define UBI_SECURE_AAD_DATA_SIZE_SIZE (sizeof(uint32_t))

/** AAD component: chunk index bound into chunked-LEB AAD (uint32_t big-endian). */
#define UBI_SECURE_AAD_CHUNK_INDEX_SIZE (sizeof(uint32_t))

/** AAD size for secure device header: prefix + peb_idx + flash_offset. */
#define UBI_SECURE_DEV_HDR_AAD_SIZE \
	(UBI_SECURE_PREFIX_SIZE + UBI_SECURE_AAD_PEB_IDX_SIZE + UBI_SECURE_AAD_FLASH_OFFSET_SIZE)

/** AAD size for secure volume header: prefix + peb_idx + flash_offset + device_revision + parent_kv. */
#define UBI_SECURE_VOL_HDR_AAD_SIZE                                                                \
	(UBI_SECURE_PREFIX_SIZE + UBI_SECURE_AAD_PEB_IDX_SIZE + UBI_SECURE_AAD_FLASH_OFFSET_SIZE + \
	 UBI_SECURE_AAD_DEV_REVISION_SIZE + UBI_SECURE_AAD_KV_SIZE)

/** AAD size for secure EC header: prefix + peb_idx + flash_offset. */
#define UBI_SECURE_EC_HDR_AAD_SIZE \
	(UBI_SECURE_PREFIX_SIZE + UBI_SECURE_AAD_PEB_IDX_SIZE + UBI_SECURE_AAD_FLASH_OFFSET_SIZE)

/** AAD size for secure data-VID header: prefix + peb_idx + flash_offset + parent_ec + parent_ec_kv. */
#define UBI_SECURE_DATA_VID_AAD_SIZE                                                               \
	(UBI_SECURE_PREFIX_SIZE + UBI_SECURE_AAD_PEB_IDX_SIZE + UBI_SECURE_AAD_FLASH_OFFSET_SIZE + \
	 UBI_SECURE_AAD_EC_SIZE + UBI_SECURE_AAD_KV_SIZE)

/** AAD size for secure LEB record (single-tag): prefix + peb_idx + flash_offset + parent_ec
 *  + parent_ec_kv + vol_id + lnum + sqnum + data_size + parent_vid_kv. */
#define UBI_SECURE_LEB_AAD_SIZE                                                                    \
	(UBI_SECURE_PREFIX_SIZE + UBI_SECURE_AAD_PEB_IDX_SIZE + UBI_SECURE_AAD_FLASH_OFFSET_SIZE + \
	 UBI_SECURE_AAD_EC_SIZE + UBI_SECURE_AAD_KV_SIZE + UBI_SECURE_AAD_VOL_ID_SIZE +            \
	 UBI_SECURE_AAD_LNUM_SIZE + UBI_SECURE_AAD_SQNUM_SIZE + UBI_SECURE_AAD_DATA_SIZE_SIZE +    \
	 UBI_SECURE_AAD_KV_SIZE)

/** AAD size for secure LEB record (chunked mode, per chunk): single-tag AAD + chunk_index. */
#define UBI_SECURE_LEB_CHUNK_AAD_SIZE (UBI_SECURE_LEB_AAD_SIZE + UBI_SECURE_AAD_CHUNK_INDEX_SIZE)

BUILD_ASSERT(UBI_SECURE_DEV_HDR_AAD_SIZE == 44, "DEV_HDR AAD size composition drift");
BUILD_ASSERT(UBI_SECURE_VOL_HDR_AAD_SIZE == 53, "VOL_HDR AAD size composition drift");
BUILD_ASSERT(UBI_SECURE_EC_HDR_AAD_SIZE == 44, "EC_HDR AAD size composition drift");
BUILD_ASSERT(UBI_SECURE_DATA_VID_AAD_SIZE == 53, "DATA_VID AAD size composition drift");
BUILD_ASSERT(UBI_SECURE_LEB_AAD_SIZE == 74, "LEB AAD size composition drift");
BUILD_ASSERT(UBI_SECURE_LEB_CHUNK_AAD_SIZE == 78, "LEB chunked AAD size composition drift");

/**
 * Sentinel LEB number for hidden per-volume anchor PEBs.
 * This value must never collide with user-visible lnum range [0, leb_count).
 */
#define UBI_SECURE_INTERNAL_ANCHOR_LNUM (UINT32_MAX)

/* Spec pinning: secure record sizes are bound here to the real on-flash
 * struct definitions plus the wrapper prefix and AEAD tag.  If a future
 * change to ubi_*_hdr or ubi_*_secure_meta layout (or to the wrapper)
 * shifts the byte count of any committed record, one of these asserts
 * fires at compile time -- spec drift can no longer go silent.
 *
 * Ordered by domain (matches enum ubi_secure_domain): DEVICE_HEADER,
 * VOLUME_HEADER, ERASE_COUNTER, VOLUME_IDENTIFIER. */

/* DEVICE_HEADER domain. */
BUILD_ASSERT(UBI_SECURE_DEV_HDR_SIZE == UBI_SECURE_PREFIX_SIZE + sizeof(struct ubi_dev_hdr) +
						sizeof(struct ubi_dev_secure_meta) +
						UBI_SECURE_TAG_SIZE,
	     "UBI_SECURE_DEV_HDR_SIZE drifted from struct ubi_dev_hdr + ubi_dev_secure_meta");
BUILD_ASSERT(
	UBI_SECURE_DEV_HDR_PLAINTEXT_SIZE ==
		sizeof(struct ubi_dev_hdr) + sizeof(struct ubi_dev_secure_meta),
	"UBI_SECURE_DEV_HDR_PLAINTEXT_SIZE drifted from struct ubi_dev_hdr + ubi_dev_secure_meta");

/* VOLUME_HEADER domain. */
BUILD_ASSERT(UBI_SECURE_VOL_HDR_SIZE ==
		     UBI_SECURE_PREFIX_SIZE + sizeof(struct ubi_vol_hdr) + UBI_SECURE_TAG_SIZE,
	     "UBI_SECURE_VOL_HDR_SIZE drifted from struct ubi_vol_hdr layout");
BUILD_ASSERT(UBI_SECURE_VOL_HDR_PLAINTEXT_SIZE == sizeof(struct ubi_vol_hdr),
	     "UBI_SECURE_VOL_HDR_PLAINTEXT_SIZE drifted from struct ubi_vol_hdr layout");

/* ERASE_COUNTER domain. */
BUILD_ASSERT(UBI_SECURE_EC_HDR_SIZE ==
		     UBI_SECURE_PREFIX_SIZE + sizeof(struct ubi_ec_hdr) + UBI_SECURE_TAG_SIZE,
	     "UBI_SECURE_EC_HDR_SIZE drifted from struct ubi_ec_hdr layout");
BUILD_ASSERT(UBI_SECURE_PLAIN_EC_HDR_SIZE == sizeof(struct ubi_ec_hdr),
	     "UBI_SECURE_PLAIN_EC_HDR_SIZE drifted from struct ubi_ec_hdr layout");

/* VOLUME_IDENTIFIER domain (data-PEB VID). */
BUILD_ASSERT(UBI_SECURE_DATA_VID_SIZE == UBI_SECURE_PREFIX_SIZE + sizeof(struct ubi_vid_hdr) +
						 sizeof(struct ubi_vid_secure_meta) +
						 UBI_SECURE_TAG_SIZE,
	     "UBI_SECURE_DATA_VID_SIZE drifted from struct ubi_vid_hdr + ubi_vid_secure_meta");
BUILD_ASSERT(
	UBI_SECURE_DATA_VID_PLAINTEXT_SIZE ==
		sizeof(struct ubi_vid_hdr) + sizeof(struct ubi_vid_secure_meta),
	"UBI_SECURE_DATA_VID_PLAINTEXT_SIZE drifted from struct ubi_vid_hdr + ubi_vid_secure_meta");
BUILD_ASSERT(UBI_SECURE_PLAIN_VID_HDR_SIZE == sizeof(struct ubi_vid_hdr),
	     "UBI_SECURE_PLAIN_VID_HDR_SIZE drifted from struct ubi_vid_hdr layout");

#endif /* UBI_SECURE_TYPES_H */
