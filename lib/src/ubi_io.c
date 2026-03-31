/**
 * \file    ubi_io.c
 * \author  Kamil Kielbasa
 * \brief   UBI flash I/O operations.
 * \version 0.10
 * \date    2026-03-27
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files ------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_io.h"
#include "ubi_flash_res_peb.h"

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <zephyr/storage/flash_map.h>

/* Standard library headers: */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------ */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Module types and type definitions ----------------------------------------------------------- */
/* Module interface variables and constants ---------------------------------------------------- */
/* Static variables and constants -------------------------------------------------------------- */
/* Static function declarations ---------------------------------------------------------------- */

static int flash_write_with_retry(const struct flash_area *fa, off_t offset, const void *data,
				  size_t len);

/* Static function definitions ----------------------------------------------------------------- */

static int flash_write_with_retry(const struct flash_area *fa, off_t offset, const void *data,
				  size_t len)
{
	int ret = -EIO;

	for (size_t attempt = 1; attempt <= CONFIG_UBI_PEB_WRITE_RETRY_COUNT; attempt++) {
		ret = flash_area_write(fa, offset, data, len);

		if (ret == 0) {
			return 0;
		}

		if (attempt < CONFIG_UBI_PEB_WRITE_RETRY_COUNT) {
			LOG_WRN("Flash write retry %zu/%d at offset 0x%lx (err %d)", attempt,
				CONFIG_UBI_PEB_WRITE_RETRY_COUNT, (unsigned long)offset, ret);
		}
	}

	return ret;
}

/* Module interface function definitions ------------------------------------------------------- */

int ubi_dev_is_mounted(const struct ubi_mtd *mtd, bool *is_mounted)
{
	if (!mtd || !is_mounted) {
		return -EINVAL;
	}

	struct ubi_flash_res_peb_scan scan = { 0 };
	const int ret = ubi_flash_res_peb_scan(mtd, &scan);

	if (ret != 0) {
		return ret;
	}

	*is_mounted = (scan.active_count >= 1 || scan.corrupt_count >= 1);
	return 0;
}

int ubi_dev_mount(const struct ubi_mtd *mtd)
{
	if (!mtd) {
		return -EINVAL;
	}

	const struct flash_area *fa = NULL;
	int ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0) {
		return ret;
	}

	struct ubi_dev_hdr dev_hdr = { 0 };
	dev_hdr.magic = UBI_DEV_HDR_MAGIC;
	dev_hdr.version = UBI_DEV_HDR_VERSION;
	dev_hdr.offset = fa->fa_off;
	dev_hdr.size = fa->fa_size;
	dev_hdr.revision = 0;
	dev_hdr.vol_count = 0;
	dev_hdr.hdr_crc =
		crc32_ieee((const uint8_t *)&dev_hdr, sizeof(dev_hdr) - sizeof(dev_hdr.hdr_crc));

	flash_area_close(fa);

	return ubi_flash_res_peb_overwrite(mtd, (const uint8_t *)&dev_hdr, sizeof(dev_hdr));
}

int ubi_dev_hdr_read(const struct ubi_mtd *mtd, struct ubi_dev_hdr *hdr)
{
	if (!mtd || !hdr) {
		return -EINVAL;
	}

	struct ubi_dev_hdr dev_hdr = { 0 };
	const int ret = ubi_flash_res_peb_validate(mtd, &dev_hdr);

	if (ret != 0 && ret != -EROFS) {
		return ret;
	}

	/* Both healthy and read-only degraded mode — header is valid for reads */
	memcpy(hdr, &dev_hdr, sizeof(dev_hdr));
	return 0;
}

int ubi_vol_hdr_read(const struct ubi_mtd *mtd, const size_t index, struct ubi_vol_hdr *hdr)
{
	if (!mtd || index >= CONFIG_UBI_MAX_NR_OF_VOLUMES || !hdr) {
		return -EINVAL;
	}

	/* Validate and recover reserved PEBs if needed */
	struct ubi_dev_hdr dev_hdr = { 0 };
	int ret = ubi_flash_res_peb_validate(mtd, &dev_hdr);

	/* Allow reads in read-only degraded mode */
	if (ret != 0 && ret != -EROFS) {
		return ret;
	}

	/* Scan to find active PEBs for reading vol headers */
	struct ubi_flash_res_peb_scan scan = { 0 };
	ret = ubi_flash_res_peb_scan(mtd, &scan);

	if (ret != 0) {
		return ret;
	}

	if (scan.active_count == 0) {
		return -EIO;
	}

	const struct flash_area *fa = NULL;
	ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0) {
		return ret;
	}

	/* Read volume header from the canonical (highest-revision) PEB. */
	struct ubi_vol_hdr vol_hdr = { 0 };
	const size_t offset = (scan.canonical_peb_idx * mtd->erase_block_size) + UBI_DEV_HDR_SIZE +
			      (UBI_VOL_HDR_SIZE * index);

	ret = flash_area_read(fa, offset, &vol_hdr, sizeof(vol_hdr));

	if (ret != 0) {
		flash_area_close(fa);
		return ret;
	}

	if (vol_hdr.magic != UBI_VOL_HDR_MAGIC) {
		flash_area_close(fa);
		return -EBADMSG;
	}

	const uint32_t crc =
		crc32_ieee((const uint8_t *)&vol_hdr, sizeof(vol_hdr) - sizeof(vol_hdr.hdr_crc));

	if (crc != vol_hdr.hdr_crc) {
		flash_area_close(fa);
		return -EBADMSG;
	}

	memcpy(hdr, &vol_hdr, sizeof(vol_hdr));

	flash_area_close(fa);
	return 0;
}

int ubi_vol_hdr_append(const struct ubi_mtd *mtd, const struct ubi_dev_hdr *dev_hdr,
		       const struct ubi_vol_hdr *vol_hdr)
{
	if (!mtd || !dev_hdr || !vol_hdr)
		return -EINVAL;

	int ret = -EIO;
	uint8_t *content = NULL;

	struct ubi_dev_hdr cur_hdr = { 0 };
	ret = ubi_flash_res_peb_validate(mtd, &cur_hdr);

	if (ret != 0) {
		goto exit;
	}

	if (cur_hdr.vol_count >= CONFIG_UBI_MAX_NR_OF_VOLUMES) {
		ret = -ENOSPC;
		goto exit;
	}

	if (cur_hdr.vol_count + 1 != dev_hdr->vol_count) {
		ret = -EACCES;
		goto exit;
	}

	const size_t content_len = UBI_DEV_HDR_SIZE + ((cur_hdr.vol_count + 1) * UBI_VOL_HDR_SIZE);

	content = k_malloc(content_len);

	if (!content) {
		ret = -ENOMEM;
		goto exit;
	}

	/* Read existing content from canonical (highest-revision) PEB */
	struct ubi_flash_res_peb_scan scan = { 0 };
	ret = ubi_flash_res_peb_scan(mtd, &scan);

	if (ret != 0) {
		goto exit;
	}

	if (scan.active_count == 0) {
		ret = -EIO;
		goto exit;
	}

	ret = ubi_flash_res_peb_read_content(mtd, scan.canonical_peb_idx, content,
					     content_len - UBI_VOL_HDR_SIZE);

	if (ret != 0) {
		goto exit;
	}

	memcpy(&content[0], dev_hdr, sizeof(*dev_hdr));
	memcpy(&content[content_len - UBI_VOL_HDR_SIZE], vol_hdr, sizeof(*vol_hdr));

	ret = ubi_flash_res_peb_commit(mtd, content, content_len);

exit:
	if (content) {
		k_free(content);
	}

	return ret;
}

int ubi_vol_hdr_remove(const struct ubi_mtd *mtd, const struct ubi_dev_hdr *dev_hdr,
		       const size_t index)
{
	if (!mtd || !dev_hdr)
		return -EINVAL;

	int ret = -EIO;
	uint8_t *content = NULL;

	struct ubi_dev_hdr cur_hdr = { 0 };
	ret = ubi_flash_res_peb_validate(mtd, &cur_hdr);

	if (ret != 0) {
		goto exit;
	}

	if (cur_hdr.vol_count == 0) {
		ret = -EINVAL;
		goto exit;
	}

	if (index > (cur_hdr.vol_count - 1)) {
		ret = -EACCES;
		goto exit;
	}

	if (cur_hdr.revision + 1 != dev_hdr->revision) {
		ret = -EACCES;
		goto exit;
	}

	if (cur_hdr.vol_count - 1 != dev_hdr->vol_count) {
		ret = -EACCES;
		goto exit;
	}

	const size_t content_len = UBI_DEV_HDR_SIZE + (dev_hdr->vol_count * UBI_VOL_HDR_SIZE);
	size_t content_off = 0;

	content = k_malloc(content_len);

	if (!content) {
		ret = -ENOMEM;
		goto exit;
	}

	memcpy(&content[content_off], dev_hdr, UBI_DEV_HDR_SIZE);
	content_off += UBI_DEV_HDR_SIZE;

	for (size_t vol_idx = 0; vol_idx < cur_hdr.vol_count; ++vol_idx) {
		if (vol_idx != index) {
			struct ubi_vol_hdr exist_vol_hdr = { 0 };
			ret = ubi_vol_hdr_read(mtd, vol_idx, &exist_vol_hdr);

			if (ret != 0) {
				goto exit;
			}

			memcpy(&content[content_off], &exist_vol_hdr, UBI_VOL_HDR_SIZE);
			content_off += UBI_VOL_HDR_SIZE;
		}
	}

	ret = ubi_flash_res_peb_commit(mtd, content, content_len);

exit:
	if (content) {
		k_free(content);
	}

	return ret;
}

int ubi_vol_hdr_update(const struct ubi_mtd *mtd, const struct ubi_dev_hdr *dev_hdr,
		       const size_t index, const struct ubi_vol_hdr *vol_hdr)
{
	if (!mtd || !dev_hdr) {
		return -EINVAL;
	}

	int ret = -EIO;
	uint8_t *content = NULL;

	struct ubi_dev_hdr cur_hdr = { 0 };
	ret = ubi_flash_res_peb_validate(mtd, &cur_hdr);

	if (ret != 0) {
		goto exit;
	}

	if (cur_hdr.vol_count == 0) {
		ret = -EINVAL;
		goto exit;
	}

	if (index > (cur_hdr.vol_count - 1)) {
		ret = -EINVAL;
		goto exit;
	}

	if (cur_hdr.revision + 1 != dev_hdr->revision) {
		ret = -EINVAL;
		goto exit;
	}

	const size_t content_len = UBI_DEV_HDR_SIZE + (cur_hdr.vol_count * UBI_VOL_HDR_SIZE);
	size_t content_off = 0;

	content = k_malloc(content_len);

	if (!content) {
		ret = -ENOMEM;
		goto exit;
	}

	memcpy(&content[content_off], dev_hdr, UBI_DEV_HDR_SIZE);
	content_off += UBI_DEV_HDR_SIZE;

	for (size_t vol_idx = 0; vol_idx < cur_hdr.vol_count; ++vol_idx) {
		if (vol_idx != index) {
			struct ubi_vol_hdr exist_vol_hdr = { 0 };
			ret = ubi_vol_hdr_read(mtd, vol_idx, &exist_vol_hdr);

			if (ret != 0) {
				goto exit;
			}

			memcpy(&content[content_off], &exist_vol_hdr, UBI_VOL_HDR_SIZE);
			content_off += UBI_VOL_HDR_SIZE;
		} else {
			memcpy(&content[content_off], vol_hdr, sizeof(*vol_hdr));
			content_off += UBI_VOL_HDR_SIZE;
		}
	}

	if (content_off != content_len) {
		ret = -EINVAL;
		goto exit;
	}

	ret = ubi_flash_res_peb_commit(mtd, content, content_len);

exit:
	if (content) {
		k_free(content);
	}

	return ret;
}

int ubi_ec_hdr_read(const struct ubi_mtd *mtd, const size_t pnum, struct ubi_ec_hdr *hdr)
{
	int ret = -EIO;

	if (!mtd)
		return -EINVAL;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0)
		return ret;

	const size_t nr_of_pebs = fa->fa_size / mtd->erase_block_size;

	if (pnum >= nr_of_pebs || pnum < UBI_DEV_HDR_NR_OF_RES_PEBS) {
		ret = -EINVAL;
		goto exit;
	}

	struct ubi_ec_hdr ec_hdr = { 0 };
	ret = flash_area_read(fa, pnum * mtd->erase_block_size, &ec_hdr, sizeof(ec_hdr));

	if (ret != 0)
		goto exit;

	if (UBI_EC_HDR_MAGIC != ec_hdr.magic ||
	    ec_hdr.hdr_crc !=
		    crc32_ieee((const uint8_t *)&ec_hdr, sizeof(ec_hdr) - sizeof(ec_hdr.hdr_crc))) {
		ret = -EBADMSG;
		goto exit;
	}

	if (hdr)
		*hdr = ec_hdr;

exit:
	if (fa)
		flash_area_close(fa);

	return ret;
}

int ubi_ec_hdr_write(const struct ubi_mtd *mtd, const size_t pnum, const struct ubi_ec_hdr *hdr)
{
	int ret = -EIO;

	if (!mtd || !hdr)
		return -EINVAL;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0)
		goto exit;

	const size_t nr_of_pebs = fa->fa_size / mtd->erase_block_size;

	if (pnum >= nr_of_pebs || pnum < UBI_DEV_HDR_NR_OF_RES_PEBS) {
		ret = -EINVAL;
		goto exit;
	}

	ret = flash_write_with_retry(fa, pnum * mtd->erase_block_size, hdr, sizeof(*hdr));

	if (ret != 0)
		goto exit;

exit:
	if (fa)
		flash_area_close(fa);

	return ret;
}

int ubi_vid_hdr_read(const struct ubi_mtd *mtd, const size_t pnum, struct ubi_vid_hdr *vid_hdr,
		     bool check)
{
	int ret = -EIO;

	if (!mtd)
		return -EINVAL;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0)
		return ret;

	const size_t nr_of_pebs = fa->fa_size / mtd->erase_block_size;

	if (pnum >= nr_of_pebs || pnum < UBI_DEV_HDR_NR_OF_RES_PEBS) {
		ret = -EINVAL;
		goto exit;
	}

	struct ubi_vid_hdr hdr = { 0 };
	ret = flash_area_read(fa, (pnum * mtd->erase_block_size) + UBI_EC_HDR_SIZE, &hdr,
			      sizeof(hdr));

	if (ret != 0)
		goto exit;

	if (vid_hdr)
		*vid_hdr = hdr;

	if (check) {
		if (UBI_VID_HDR_MAGIC != hdr.magic ||
		    hdr.hdr_crc !=
			    crc32_ieee((const uint8_t *)&hdr, sizeof(hdr) - sizeof(hdr.hdr_crc))) {
			ret = -EBADMSG;
			goto exit;
		}
	}

exit:
	if (fa)
		flash_area_close(fa);

	return ret;
}

int ubi_vid_hdr_write(const struct ubi_mtd *mtd, const size_t pnum, struct ubi_vid_hdr *vid_hdr)
{
	int ret = -EIO;

	if (!mtd || !vid_hdr)
		return -EINVAL;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0)
		goto exit;

	const size_t nr_of_pebs = fa->fa_size / mtd->erase_block_size;

	if (pnum >= nr_of_pebs || pnum < UBI_DEV_HDR_NR_OF_RES_PEBS) {
		ret = -EINVAL;
		goto exit;
	}

	ret = flash_write_with_retry(fa, (pnum * mtd->erase_block_size) + UBI_EC_HDR_SIZE, vid_hdr,
				     sizeof(*vid_hdr));

	if (ret != 0)
		goto exit;

exit:
	if (fa)
		flash_area_close(fa);

	return ret;
}

int ubi_leb_data_write(const struct ubi_mtd *mtd, const size_t pnum, const uint8_t *buf, size_t len)
{
	int ret = -EIO;

	if (!mtd || !buf || 0 == len)
		return -EINVAL;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0)
		goto exit;

	const size_t nr_of_pebs = fa->fa_size / mtd->erase_block_size;

	if (pnum >= nr_of_pebs || pnum < UBI_DEV_HDR_NR_OF_RES_PEBS) {
		ret = -EINVAL;
		goto exit;
	}

	if (len > (mtd->erase_block_size - UBI_EC_HDR_SIZE - UBI_VID_HDR_SIZE)) {
		ret = -ENOSPC;
		goto exit;
	}

	size_t offset = (pnum * mtd->erase_block_size) + UBI_EC_HDR_SIZE + UBI_VID_HDR_SIZE;
	const size_t wbs = mtd->write_block_size;

	if (len % wbs == 0) {
		ret = flash_write_with_retry(fa, offset, buf, len);

		if (ret != 0)
			goto exit;
	} else {
		if (len < wbs) {
			uint8_t align_buf[WRITE_BLOCK_SIZE_ALIGNMENT] = { 0 };
			memcpy(align_buf, buf, len);

			ret = flash_write_with_retry(fa, offset, align_buf, wbs);

			if (ret != 0)
				goto exit;
		} else {
			const size_t left_size = len % wbs;

			uint8_t align_buf[WRITE_BLOCK_SIZE_ALIGNMENT] = { 0 };
			memcpy(align_buf, &buf[len - left_size], left_size);

			ret = flash_write_with_retry(fa, offset, buf, len - left_size);

			if (ret != 0)
				goto exit;

			ret = flash_write_with_retry(fa, offset + len - left_size, align_buf, wbs);

			if (ret != 0)
				goto exit;
		}
	}

exit:
	if (fa)
		flash_area_close(fa);

	return ret;
}

int ubi_leb_data_read(const struct ubi_mtd *mtd, const size_t pnum, size_t offset, uint8_t *buf,
		      size_t len)
{
	int ret = -EIO;

	if (!mtd || !buf || 0 == len)
		return -EINVAL;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0)
		goto exit;

	const size_t nr_of_pebs = fa->fa_size / mtd->erase_block_size;

	if (pnum >= nr_of_pebs || pnum < UBI_DEV_HDR_NR_OF_RES_PEBS) {
		ret = -EINVAL;
		goto exit;
	}

	if ((offset + len) > (mtd->erase_block_size - UBI_EC_HDR_SIZE - UBI_VID_HDR_SIZE)) {
		ret = -ENOSPC;
		goto exit;
	}

	const size_t _offset =
		(pnum * mtd->erase_block_size) + UBI_EC_HDR_SIZE + UBI_VID_HDR_SIZE + offset;

	ret = flash_area_read(fa, _offset, buf, len);

	if (ret != 0)
		goto exit;

exit:
	if (fa)
		flash_area_close(fa);

	return ret;
}
