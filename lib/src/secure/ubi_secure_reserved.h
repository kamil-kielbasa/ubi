/**
 * \file    ubi_secure_reserved.h
 * \author  Kamil Kielbasa
 * \brief   Secure reserved-PEB management: scan, authenticate, write.
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard ------------------------------------------------------------------------------- */
#ifndef UBI_SECURE_RESERVED_H
#define UBI_SECURE_RESERVED_H

/* Include files ------------------------------------------------------------------------------- */
#include "ubi_secure_types.h"
#include "ubi_io.h"

#include <ubi_crypto.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Defines ------------------------------------------------------------------------------------- */

/** Number of reserved PEBs that must be kept active. */
#define UBI_SECURE_RES_PEB_NR_ACTIVE (2)

/* Types and type definitions ------------------------------------------------------------------ */

/** Classification of a secure reserved PEB after scanning. */
enum ubi_secure_res_peb_state {
	UBI_SECURE_RES_PEB_CORRUPT = 0, /**< Cannot authenticate or bad flash. */
	UBI_SECURE_RES_PEB_SPARE, /**< Erased/blank. */
	UBI_SECURE_RES_PEB_AUTHENTICATED, /**< Secure device header authenticated. */
};

/**
 * \brief Result of scanning and authenticating all reserved PEBs.
 */
struct ubi_secure_res_peb_scan {
	size_t auth_count; /**< PEBs successfully authenticated. */
	size_t spare_count; /**< Blank PEBs. */
	size_t corrupt_count; /**< PEBs that failed auth or read. */

	enum ubi_secure_res_peb_state state[UBI_DEV_HDR_NR_OF_RES_PEBS];

	/** Canonical (highest-revision) authenticated device header. */
	struct ubi_dev_hdr dev_hdr;

	/** Canonical secure metadata from authenticated device header. */
	struct ubi_dev_secure_meta dev_meta;

	/** Canonical prefix from the authenticated device header. */
	struct ubi_crypto_prefix32 dev_prefix;

	/** PEB index of the canonical copy. */
	size_t canonical_peb_idx;

	/** Per-PEB authenticated revision (0 if not authenticated). */
	uint32_t revision[UBI_DEV_HDR_NR_OF_RES_PEBS];
};

/* Function declarations ----------------------------------------------------------------------- */

/**
 * \brief Scan and authenticate all reserved PEBs.
 *
 * Reads the secure device header from each reserved PEB, attempts AEAD
 * authentication. Selects the PEB with the highest authenticated revision
 * as canonical.
 *
 * \param[in]  mtd         UBI MTD descriptor.
 * \param[in]  crypto_cfg  Crypto configuration (for get_key_id callback).
 * \param[out] scan        Scan result.
 *
 * \retval 0    Success.
 * \retval -EIO Flash read failure.
 */
int ubi_secure_res_peb_scan(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			    struct ubi_secure_res_peb_scan *scan);

/**
 * \brief Authenticate and read volume headers from a reserved PEB.
 *
 * After scanning, reads and authenticates the secure volume headers tied
 * to the canonical device header generation.
 *
 * \param[in]  mtd         UBI MTD descriptor.
 * \param[in]  crypto_cfg  Crypto configuration.
 * \param[in]  scan        Completed scan result (canonical PEB selected).
 * \param[out] vol_hdrs    Array of vol_count volume headers.
 * \param      max_vols    Capacity of vol_hdrs array.
 *
 * \retval 0           Success, vol_hdrs populated.
 * \retval -EOVERFLOW  vol_count exceeds max_vols.
 * \retval -EIO        Auth failure on a volume header.
 */
int ubi_secure_res_peb_read_vol_hdrs(const struct ubi_mtd *mtd,
				     const struct ubi_crypto_config *crypto_cfg,
				     const struct ubi_secure_res_peb_scan *scan,
				     struct ubi_vol_hdr *vol_hdrs, size_t max_vols);

/**
 * \brief Write a full secure reserved-PEB content (format or overwrite).
 *
 * Encrypts the device header + dev_secure_meta and all volume headers,
 * then writes them to all active + spare reserved PEBs.
 *
 * \param[in] mtd         UBI MTD descriptor.
 * \param[in] crypto_cfg  Crypto configuration.
 * \param[in] dev_hdr     Device header to commit.
 * \param[in] dev_meta    Secure metadata to commit.
 * \param[in] vol_hdrs    Volume headers (NULL if vol_count == 0).
 * \param     vol_count   Number of volume headers.
 * \param     key_version Key version for encryption.
 * \param     counter     Next metadata counter to use.
 *
 * \retval 0       Success.
 * \retval -EIO    Flash or crypto failure.
 * \retval -EROFS  Committed but degraded (fewer active PEBs than required).
 */
int ubi_secure_res_peb_commit(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			      const struct ubi_dev_hdr *dev_hdr,
			      const struct ubi_dev_secure_meta *dev_meta,
			      const struct ubi_vol_hdr *vol_hdrs, size_t vol_count,
			      uint8_t key_version, uint64_t counter);

/**
 * \brief Check if a reserved PEB contains a secure wrapper (vs plain or blank).
 *
 * Reads the first 4 bytes (magic) from the PEB to determine mode.
 *
 * \param[in] mtd       UBI MTD descriptor.
 * \param     peb_idx   Reserved PEB index.
 * \param[out] is_secure   True if secure magic found.
 * \param[out] is_blank    True if all bytes are erased.
 *
 * \retval 0    Success.
 * \retval -EIO Flash read error.
 */
int ubi_secure_res_peb_detect_mode(const struct ubi_mtd *mtd, size_t peb_idx, bool *is_secure,
				   bool *is_blank);

#endif /* UBI_SECURE_RESERVED_H */
