/**
 * \file    ubi_flash_res_peb.h
 * \author  Kamil Kielbasa
 * \brief   UBI reserved PEB management: scanning, recovery, and commit.
 *
 * \copyright Copyright (c) 2025
 */

/* Include guard ------------------------------------------------------------------------------- */
#ifndef UBI_FLASH_RES_PEB_H
#define UBI_FLASH_RES_PEB_H

/* Include files ------------------------------------------------------------------------------- */
#include "ubi_io.h"

#include <stddef.h>
#include <stdint.h>

/* Defines ------------------------------------------------------------------------------------- */

/** Number of PEBs that must remain active at all times. */
#define UBI_FLASH_RES_PEB_NR_ACTIVE (2)

/* Types and type definitions ------------------------------------------------------------------ */

/** Classification of a reserved PEB after scanning. */
enum ubi_flash_res_peb_state {
	UBI_FLASH_RES_PEB_STATE_CORRUPT = 0, /**< Invalid data (bad magic or CRC). */
	UBI_FLASH_RES_PEB_STATE_SPARE, /**< Erased/empty (all 0xFF), cold spare. */
	UBI_FLASH_RES_PEB_STATE_ACTIVE, /**< Valid device header (correct magic + CRC). */
};

/**
 * \brief Result of scanning all reserved PEBs.
 *
 * Classifies each reserved PEB and stores the canonical header from the
 * active PEB with the highest revision number.
 */
struct ubi_flash_res_peb_scan {
	size_t active_count; /**< Number of PEBs with valid device headers. */
	size_t spare_count; /**< Number of empty/erased PEBs (cold spares). */
	size_t corrupt_count; /**< Number of PEBs with invalid headers. */

	/**
	 * Per-PEB classification. Index corresponds to PEB index:
	 * state[0] = PEB 0, state[1] = PEB 1, etc.
	 */
	enum ubi_flash_res_peb_state state[UBI_DEV_HDR_NR_OF_RES_PEBS];

	/** Canonical device header (from active PEB with highest revision). */
	struct ubi_dev_hdr hdr;

	/** Index of the canonical (highest-revision) active PEB. */
	size_t canonical_peb_idx;
};

/* Function declarations ----------------------------------------------------------------------- */

/**
 * \brief Scan all reserved PEBs and classify their state.
 *
 * Reads the device header from each reserved PEB (indices 0..N-1),
 * validates magic and CRC, and populates the scan result. PEBs with
 * all-0xFF content are classified as spares; PEBs with valid headers
 * as active; all others as corrupt.
 *
 * \param[in] mtd	UBI MTD device structure.
 * \param[out] scan	Scan result structure.
 *
 * \return 0 on success, negative error code on flash read failure.
 */
int ubi_flash_res_peb_scan(const struct ubi_mtd *mtd, struct ubi_flash_res_peb_scan *scan);

/**
 * \brief Validate reserved PEBs and recover if degraded.
 *
 * Scans all reserved PEBs. If at least 2 are active, returns the device
 * header. If fewer are active but recovery is possible, reads the full
 * content from a valid PEB and recovers the missing ones. Returns -EROFS
 * when only 1 active PEB remains and recovery fails (read-only degraded
 * mode).
 *
 * \param[in] mtd	UBI MTD device structure.
 * \param[out] dev_hdr	Current device header.
 *
 * \return 0 on success, -EROFS for read-only degraded mode, -EIO if unrecoverable.
 */
int ubi_flash_res_peb_validate(const struct ubi_mtd *mtd, struct ubi_dev_hdr *dev_hdr);

/**
 * \brief Write device and volume headers to reserved PEBs.
 *
 * Scans for current state, then writes active PEBs first, promoting
 * spares to replace any that fail. Corrupt PEBs are skipped.
 *
 * \param[in] mtd	UBI MTD device structure.
 * \param[in] content	Buffer containing the new device and volume data.
 * \param content_len	Size of \p content in bytes.
 *
 * \return 0 on success, negative error code on failure.
 */
int ubi_flash_res_peb_overwrite(const struct ubi_mtd *mtd, const uint8_t *content,
				size_t content_len);

/**
 * \brief Commit updated headers to reserved PEBs and verify.
 *
 * Calls ubi_flash_res_peb_overwrite() and then verifies the result via a
 * fresh scan.
 *
 * \param[in] mtd	UBI MTD device structure.
 * \param[in] content	Buffer with new headers.
 * \param content_len	Size of \p content in bytes.
 *
 * \return 0 on success, negative error code on failure.
 */
int ubi_flash_res_peb_commit(const struct ubi_mtd *mtd, const uint8_t *content, size_t content_len);

/**
 * \brief Find the first active PEB index in a scan result.
 *
 * \param[in] scan	Scan result.
 *
 * \return PEB index, or UBI_DEV_HDR_NR_OF_RES_PEBS if none found.
 */
size_t ubi_flash_res_peb_find_first_active(const struct ubi_flash_res_peb_scan *scan);

/**
 * \brief Read the full content (dev hdr + vol hdrs) from a specific reserved PEB.
 *
 * \param[in] mtd		UBI MTD device structure.
 * \param peb_idx		Reserved PEB index.
 * \param[out] content		Output buffer (must be at least dev_hdr.size bytes).
 * \param content_len		Size of \p content in bytes.
 *
 * \return 0 on success, negative error code on failure.
 */
int ubi_flash_res_peb_read_content(const struct ubi_mtd *mtd, size_t peb_idx, uint8_t *content,
				   size_t content_len);

#endif /* UBI_FLASH_RES_PEB_H */
