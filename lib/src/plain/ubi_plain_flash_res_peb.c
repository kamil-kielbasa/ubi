/**
 * \file    ubi_flash_res_peb.c
 * \author  Kamil Kielbasa
 * \brief   UBI reserved PEB management: scanning, recovery, and commit.
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files ------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_plain_flash_res_peb.h"
#include "ubi_internal.h"
#include "ubi_plain_io.h"
#include "ubi_mem.h"

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/crc.h>
#include <zephyr/storage/flash_map.h>

/* Standard library headers: */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------ */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Static function declarations ---------------------------------------------------------------- */

/**
 * \brief Recover corrupt/spare reserved PEBs from a valid canonical copy.
 *
 * For each PEB not classified as active, attempts to erase and rewrite it
 * with the canonical content. If the erase or write fails, the PEB is left
 * as corrupt (physically dead).
 *
 * \pre scan->active_count < UBI_FLASH_RES_PEB_NR_ACTIVE (caller must verify).
 *
 * \param[in] mtd		UBI MTD device structure.
 * \param[in,out] scan		Scan result (updated on successful recovery).
 * \param[in] content		Full reserved-PEB content (dev hdr + vol hdrs).
 * \param content_len		Size of \p content in bytes.
 *
 * \return 0 on success (at least 2 active PEBs after recovery), -EIO on failure.
 */
static int flash_res_peb_recover(const struct ubi_mtd *mtd, struct ubi_flash_res_peb_scan *scan,
				 const uint8_t *content, size_t content_len);

/**
 * \brief Semantically validate a device header beyond magic/CRC.
 *
 * Checks that fields are within expected ranges given the flash geometry.
 *
 * \param[in] hdr              Device header to validate.
 * \param erase_block_size     Size of one erase block in bytes.
 *
 * \retval true  Header is semantically valid.
 * \retval false One or more fields are out of range.
 */
static bool flash_res_peb_hdr_semantically_valid(const struct ubi_dev_hdr *hdr,
						 size_t erase_block_size);

/* Static function definitions ----------------------------------------------------------------- */

static bool flash_res_peb_hdr_semantically_valid(const struct ubi_dev_hdr *hdr,
						 size_t erase_block_size)
{
	if (hdr->version != UBI_DEV_HDR_VERSION) {
		LOG_WRN("Unexpected device header version: %u", hdr->version);
		return false;
	}

	if (hdr->vol_count > CONFIG_UBI_MAX_NR_OF_VOLUMES) {
		LOG_WRN("vol_count %u exceeds max %d", hdr->vol_count,
			CONFIG_UBI_MAX_NR_OF_VOLUMES);
		return false;
	}

	const size_t required = UBI_DEV_HDR_SIZE + ((size_t)hdr->vol_count * UBI_VOL_HDR_SIZE);

	if (required > erase_block_size) {
		LOG_WRN("Headers (%zu bytes) exceed erase block (%zu bytes)", required,
			erase_block_size);
		return false;
	}

	return true;
}

static int flash_res_peb_recover(const struct ubi_mtd *mtd, struct ubi_flash_res_peb_scan *scan,
				 const uint8_t *content, size_t content_len)
{
	__ASSERT_NO_MSG(mtd);
	__ASSERT_NO_MSG(scan);
	__ASSERT_NO_MSG(content);
	__ASSERT_NO_MSG(scan->active_count < UBI_FLASH_RES_PEB_NR_ACTIVE);

	const struct flash_area *fa = NULL;
	int ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failed: %d", ret);
		return ret;
	}

	for (size_t i = 0; i < UBI_DEV_HDR_NR_OF_RES_PEBS; ++i) {
		if (scan->state[i] == UBI_FLASH_RES_PEB_STATE_ACTIVE) {
			continue;
		}

		if (scan->active_count >= UBI_FLASH_RES_PEB_NR_ACTIVE) {
			break;
		}

		const size_t offset = i * mtd->erase_block_size;

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
		if (ubi_test_flash_erase_check_fail()) {
			LOG_WRN("Reserved PEB %zu erase faulted (injected)", i);
			continue;
		}
#endif

		ret = flash_area_erase(fa, offset, mtd->erase_block_size);

		if (ret != 0) {
			LOG_WRN("Reserved PEB %zu erase failed (dead?), skipping", i);
			if (scan->state[i] == UBI_FLASH_RES_PEB_STATE_SPARE) {
				scan->spare_count--;
				scan->corrupt_count++;
				scan->state[i] = UBI_FLASH_RES_PEB_STATE_CORRUPT;
			}
			continue;
		}

		ret = flash_area_write(fa, offset, content, content_len);

		if (ret != 0) {
			LOG_WRN("Reserved PEB %zu write failed (dead?), skipping", i);
			if (scan->state[i] == UBI_FLASH_RES_PEB_STATE_SPARE) {
				scan->spare_count--;
				scan->corrupt_count++;
				scan->state[i] = UBI_FLASH_RES_PEB_STATE_CORRUPT;
			}
			continue;
		}

		/* Successfully recovered */
		if (scan->state[i] == UBI_FLASH_RES_PEB_STATE_CORRUPT) {
			scan->corrupt_count--;
		} else if (scan->state[i] == UBI_FLASH_RES_PEB_STATE_SPARE) {
			scan->spare_count--;
		}

		scan->state[i] = UBI_FLASH_RES_PEB_STATE_ACTIVE;
		scan->active_count++;
		LOG_INF("Reserved PEB %zu recovered", i);
	}

	flash_area_close(fa);

	if (scan->active_count < UBI_FLASH_RES_PEB_NR_ACTIVE) {
		LOG_ERR("Recovery failed: only %zu active PEBs (need %d)", scan->active_count,
			UBI_FLASH_RES_PEB_NR_ACTIVE);
		return -EIO;
	}

	return 0;
}

/* Module interface function definitions ------------------------------------------------------- */

size_t ubi_flash_res_peb_find_first_active(const struct ubi_flash_res_peb_scan *scan)
{
	__ASSERT_NO_MSG(scan);

	for (size_t i = 0; i < UBI_DEV_HDR_NR_OF_RES_PEBS; ++i) {
		if (scan->state[i] == UBI_FLASH_RES_PEB_STATE_ACTIVE) {
			return i;
		}
	}

	return UBI_DEV_HDR_NR_OF_RES_PEBS;
}

int ubi_flash_res_peb_scan(const struct ubi_mtd *mtd, struct ubi_flash_res_peb_scan *scan)
{
	__ASSERT_NO_MSG(mtd);
	__ASSERT_NO_MSG(scan);

	memset(scan, 0, sizeof(*scan));

	const struct flash_area *fa = NULL;
	int ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failed: %d", ret);
		return ret;
	}

	/* Build the erased magic pattern from the hardware-reported erased byte. */
	const uint8_t ev = flash_area_erased_val(fa);
	uint32_t erased_magic;
	memset(&erased_magic, ev, sizeof(erased_magic));

	uint32_t highest_revision = 0;
	bool has_active = false;

	for (size_t i = 0; i < UBI_DEV_HDR_NR_OF_RES_PEBS; ++i) {
		struct ubi_dev_hdr hdr = { 0 };
		const size_t offset = i * mtd->erase_block_size;

		ret = flash_area_read(fa, offset, &hdr, sizeof(hdr));

		if (ret != 0) {
			scan->state[i] = UBI_FLASH_RES_PEB_STATE_CORRUPT;
			scan->corrupt_count++;
			continue;
		}

		/* Check if PEB is erased by comparing magic against erased pattern. */
		if (hdr.magic == erased_magic) {
			scan->state[i] = UBI_FLASH_RES_PEB_STATE_SPARE;
			scan->spare_count++;
			continue;
		}

		/* Validate magic and CRC */
		if (hdr.magic != UBI_DEV_HDR_MAGIC) {
			scan->state[i] = UBI_FLASH_RES_PEB_STATE_CORRUPT;
			scan->corrupt_count++;
			continue;
		}

		const uint32_t crc =
			crc32_ieee((const uint8_t *)&hdr, sizeof(hdr) - sizeof(hdr.hdr_crc));

		if (crc != hdr.hdr_crc) {
			scan->state[i] = UBI_FLASH_RES_PEB_STATE_CORRUPT;
			scan->corrupt_count++;
			continue;
		}

		/* Semantic validation beyond magic/CRC */
		if (!flash_res_peb_hdr_semantically_valid(&hdr, mtd->erase_block_size)) {
			scan->state[i] = UBI_FLASH_RES_PEB_STATE_CORRUPT;
			scan->corrupt_count++;
			continue;
		}

		/* Also validate volume headers if any exist */
		bool vol_hdrs_valid = true;

		for (uint32_t v = 0; v < hdr.vol_count; ++v) {
			struct ubi_vol_hdr vhdr = { 0 };
			const size_t vol_off = offset + UBI_DEV_HDR_SIZE + (v * UBI_VOL_HDR_SIZE);

			ret = flash_area_read(fa, vol_off, &vhdr, sizeof(vhdr));

			if (ret != 0) {
				vol_hdrs_valid = false;
				break;
			}

			if (vhdr.magic != UBI_VOL_HDR_MAGIC) {
				vol_hdrs_valid = false;
				break;
			}

			const uint32_t vol_crc = crc32_ieee((const uint8_t *)&vhdr,
							    sizeof(vhdr) - sizeof(vhdr.hdr_crc));

			if (vol_crc != vhdr.hdr_crc) {
				vol_hdrs_valid = false;
				break;
			}

			if (!ubi_vol_hdr_semantically_valid(&vhdr)) {
				LOG_WRN("Vol header %u in PEB %zu semantically invalid", v, i);
				vol_hdrs_valid = false;
				break;
			}
		}

		if (!vol_hdrs_valid) {
			scan->state[i] = UBI_FLASH_RES_PEB_STATE_CORRUPT;
			scan->corrupt_count++;
			continue;
		}

		/* Valid active PEB */
		scan->state[i] = UBI_FLASH_RES_PEB_STATE_ACTIVE;
		scan->active_count++;

		if (!has_active || hdr.revision > highest_revision) {
			highest_revision = hdr.revision;
			scan->hdr = hdr;
			scan->canonical_peb_idx = i;
			has_active = true;
		}
	}

	flash_area_close(fa);
	return 0;
}

int ubi_flash_res_peb_read_content(const struct ubi_mtd *mtd, const size_t peb_idx,
				   uint8_t *content, const size_t content_len)
{
	__ASSERT_NO_MSG(mtd);
	__ASSERT_NO_MSG(content);
	__ASSERT_NO_MSG(peb_idx < UBI_DEV_HDR_NR_OF_RES_PEBS);

	const struct flash_area *fa = NULL;
	int ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failed: %d", ret);
		return ret;
	}

	const size_t offset = peb_idx * mtd->erase_block_size;
	ret = flash_area_read(fa, offset, content, content_len);

	if (ret != 0) {
		LOG_ERR("Reserved PEB %zu read failed: %d", peb_idx, ret);
	}

	flash_area_close(fa);
	return ret;
}

int ubi_flash_res_peb_overwrite(const struct ubi_mtd *mtd, const uint8_t *content,
				const size_t content_len)
{
	__ASSERT_NO_MSG(mtd);
	__ASSERT_NO_MSG(content);
	__ASSERT_NO_MSG(content_len > 0);

	if (content_len > mtd->erase_block_size) {
		LOG_ERR("Content length %zu exceeds erase block size %zu", content_len,
			mtd->erase_block_size);
		return -EINVAL;
	}

	struct ubi_flash_res_peb_scan scan = { 0 };
	int ret = ubi_flash_res_peb_scan(mtd, &scan);

	if (ret != 0) {
		LOG_ERR("Reserved PEB scan failed: %d", ret);
		return ret;
	}

	const struct flash_area *fa = NULL;
	ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failed: %d", ret);
		return ret;
	}

	size_t written = 0;

	/*
	 * For each active PEB: try erase+write. On failure, immediately seek
	 * a replacement from corrupt/spare PEBs before touching the next active
	 * PEB.  This prevents losing all copies of the old data when multiple
	 * active PEBs fail in sequence.
	 */
	for (size_t i = 0; i < UBI_DEV_HDR_NR_OF_RES_PEBS; ++i) {
		if (scan.state[i] != UBI_FLASH_RES_PEB_STATE_ACTIVE) {
			continue;
		}

		const size_t offset = i * mtd->erase_block_size;

		ret = flash_area_erase(fa, offset, mtd->erase_block_size);

		if (ret == 0) {
			ret = flash_area_write(fa, offset, content, content_len);
		}

		if (ret == 0) {
			written++;
			continue;
		}

		/* Active PEB failed — mark dead and seek immediate replacement */
		LOG_WRN("Active PEB %zu failed during commit, seeking replacement", i);
		scan.state[i] = UBI_FLASH_RES_PEB_STATE_CORRUPT;

		for (size_t j = 0; j < UBI_DEV_HDR_NR_OF_RES_PEBS; ++j) {
			if (j == i) {
				continue;
			}

			if (scan.state[j] != UBI_FLASH_RES_PEB_STATE_CORRUPT &&
			    scan.state[j] != UBI_FLASH_RES_PEB_STATE_SPARE) {
				continue;
			}

			const size_t repl_offset = j * mtd->erase_block_size;

			ret = flash_area_erase(fa, repl_offset, mtd->erase_block_size);

			if (ret != 0) {
				scan.state[j] = UBI_FLASH_RES_PEB_STATE_CORRUPT;
				continue;
			}

			ret = flash_area_write(fa, repl_offset, content, content_len);

			if (ret != 0) {
				scan.state[j] = UBI_FLASH_RES_PEB_STATE_CORRUPT;
				continue;
			}

			scan.state[j] = UBI_FLASH_RES_PEB_STATE_ACTIVE;
			written++;
			break;
		}
	}

	/* Fill remaining slots from spare/corrupt PEBs (e.g. initial format) */
	for (size_t i = 0; i < UBI_DEV_HDR_NR_OF_RES_PEBS && written < UBI_FLASH_RES_PEB_NR_ACTIVE;
	     ++i) {
		if (scan.state[i] != UBI_FLASH_RES_PEB_STATE_SPARE &&
		    scan.state[i] != UBI_FLASH_RES_PEB_STATE_CORRUPT) {
			continue;
		}

		const size_t offset = i * mtd->erase_block_size;

		ret = flash_area_erase(fa, offset, mtd->erase_block_size);

		if (ret != 0) {
			continue;
		}

		ret = flash_area_write(fa, offset, content, content_len);

		if (ret != 0) {
			continue;
		}

		written++;
	}

	flash_area_close(fa);

	if (written == 0) {
		LOG_ERR("Overwrite failed: no PEB could be written");
		return -EIO;
	}

	return 0;
}

int ubi_flash_res_peb_validate(const struct ubi_mtd *mtd, struct ubi_dev_hdr *dev_hdr)
{
	__ASSERT_NO_MSG(mtd);
	__ASSERT_NO_MSG(dev_hdr);

	struct ubi_flash_res_peb_scan scan = { 0 };
	int ret = ubi_flash_res_peb_scan(mtd, &scan);

	if (ret != 0) {
		LOG_ERR("Reserved PEB scan failed: %d", ret);
		return ret;
	}

	if (scan.active_count == 0) {
		LOG_ERR("Validate failed: no active reserved PEBs");
		return -EIO;
	}

	/* Healthy: all required active PEBs present */
	if (scan.active_count >= UBI_FLASH_RES_PEB_NR_ACTIVE) {
		*dev_hdr = scan.hdr;
		return 0;
	}

	/* Degraded: attempt recovery from canonical PEB */
	const size_t canonical = scan.canonical_peb_idx;

	if (canonical >= UBI_DEV_HDR_NR_OF_RES_PEBS) {
		LOG_ERR("Validate failed: no active PEB found for recovery");
		return -EIO;
	}

	const size_t content_len = UBI_DEV_HDR_SIZE + (scan.hdr.vol_count * UBI_VOL_HDR_SIZE);
	uint8_t *content = NULL;
	ret = ubi_mem_scratch_alloc(content_len, &content);

	if (ret != 0) {
		LOG_ERR("Scratch allocation failed for recovery content (%zu bytes)", content_len);
		return ret;
	}

	ret = ubi_flash_res_peb_read_content(mtd, canonical, content, content_len);

	if (ret != 0) {
		LOG_ERR("Reserved PEB %zu content read failed: %d", canonical, ret);
		ubi_mem_scratch_free(content);
		return ret;
	}

	ret = flash_res_peb_recover(mtd, &scan, content, content_len);
	ubi_mem_scratch_free(content);

	if (ret != 0) {
		/* Recovery failed but we still have 1 active PEB — read-only degraded mode */
		if (scan.active_count >= 1) {
			LOG_ERR("Recovery failed, degraded read-only mode (%zu active PEBs)",
				scan.active_count);
			*dev_hdr = scan.hdr;
			return -EROFS;
		}

		LOG_ERR("Recovery failed: no active reserved PEBs remaining");
		return -EIO;
	}

	*dev_hdr = scan.hdr;
	return 0;
}

int ubi_flash_res_peb_commit(const struct ubi_mtd *mtd, const uint8_t *content,
			     const size_t content_len)
{
	__ASSERT_NO_MSG(mtd);
	__ASSERT_NO_MSG(content);

	int ret = ubi_flash_res_peb_overwrite(mtd, content, content_len);

	if (ret != 0) {
		LOG_ERR("Commit overwrite failed: %d", ret);
		return ret;
	}

	/* Verify the write succeeded */
	struct ubi_flash_res_peb_scan verify = { 0 };
	ret = ubi_flash_res_peb_scan(mtd, &verify);

	if (ret != 0) {
		LOG_ERR("Commit verification scan failed: %d", ret);
		return ret;
	}

	if (verify.active_count == 0) {
		LOG_ERR("Commit verification failed: no active PEBs after write");
		return -EIO;
	}

	if (verify.active_count < UBI_FLASH_RES_PEB_NR_ACTIVE) {
		LOG_WRN("Commit succeeded but bank is degraded: %zu/%d active PEBs",
			verify.active_count, UBI_FLASH_RES_PEB_NR_ACTIVE);
		return -EROFS;
	}

	return 0;
}
