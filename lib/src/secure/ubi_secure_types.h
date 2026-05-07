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

/** AAD size for secure device header: prefix(32) + peb_idx(4) + offset(8). */
#define UBI_SECURE_DEV_HDR_AAD_SIZE (44)

/** AAD size for secure volume header: prefix(32) + peb_idx(4) + offset(8) + revision(8) + parent_kv(1). */
#define UBI_SECURE_VOL_HDR_AAD_SIZE (53)

/** AAD size for secure EC header: prefix(32) + peb_idx(4) + offset(8) = 44. */
#define UBI_SECURE_EC_HDR_AAD_SIZE (44)

/** AAD size for secure data-VID header: prefix(32) + peb_idx(4) + offset(8) + ec(8) + parent_ec_kv(1) = 53. */
#define UBI_SECURE_DATA_VID_AAD_SIZE (53)

/** AAD size for secure LEB record (single-tag): prefix(32) + peb_idx(4) + offset(8) + ec(8)
 *  + parent_ec_kv(1) + vol_id(4) + lnum(4) + sqnum(8) + data_size(4) + parent_vid_kv(1) = 74. */
#define UBI_SECURE_LEB_AAD_SIZE (74)

/** AAD size for secure LEB record (chunked mode, per chunk):
 *  single-tag AAD(74) + be32(chunk_index)(4) = 78. */
#define UBI_SECURE_LEB_CHUNK_AAD_SIZE (78)

/**
 * Sentinel LEB number for hidden per-volume anchor PEBs.
 * This value must never collide with user-visible lnum range [0, leb_count).
 */
#define UBI_SECURE_INTERNAL_ANCHOR_LNUM (UINT32_MAX)

#endif /* UBI_SECURE_TYPES_H */
