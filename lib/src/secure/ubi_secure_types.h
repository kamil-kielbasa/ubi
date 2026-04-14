/**
 * \file    ubi_secure_types.h
 * \brief   Internal types for the UBI secure backend.
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard ------------------------------------------------------------------------------- */
#ifndef UBI_SECURE_TYPES_H
#define UBI_SECURE_TYPES_H

/* Include files ------------------------------------------------------------------------------- */
#include <zephyr/sys/__assert.h>

#include <stddef.h>
#include <stdint.h>

/* Defines ------------------------------------------------------------------------------------- */

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

/** Size of the reserved field in the prefix. */
#define UBI_SECURE_PREFIX_RESERVED_SIZE (12)

/** Byte offset of domain field within serialized prefix32. */
#define UBI_SECURE_PREFIX_OFF_DOMAIN (5)

/** Byte offset of key_version field within serialized prefix32. */
#define UBI_SECURE_PREFIX_OFF_KEY_VERSION (6)

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

/* Crypto domains -------------------------------------------------------------- */

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

/* Types and type definitions ------------------------------------------------------------------ */

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

/* AAD sizes ------------------------------------------------------------------- */

/** AAD size for secure device header: prefix(32) + peb_idx(4) + offset(8). */
#define UBI_SECURE_DEV_HDR_AAD_SIZE (44)

/** AAD size for secure volume header: prefix(32) + peb_idx(4) + offset(8) + revision(8) + parent_kv(1). */
#define UBI_SECURE_VOL_HDR_AAD_SIZE (53)

#endif /* UBI_SECURE_TYPES_H */
