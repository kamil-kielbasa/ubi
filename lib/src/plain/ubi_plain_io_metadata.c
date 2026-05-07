/**
 * \file    ubi_plain_io_metadata.c
 * \author  Kamil Kielbasa
 * \brief   UBI metadata I/O: device and volume header operations.
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files -------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_plain_io.h"
#include "ubi_mem.h"
#include "ubi_plain_flash_res_peb.h"

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/storage/flash_map.h>

/* Standard library headers: */
#include <stdbool.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Module interface function definitions -------------------------------------------------------- */

int ubi_dev_is_mounted(const struct ubi_flash_desc *flash, bool *is_mounted)
{
	if (!flash || !is_mounted) {
		LOG_ERR("NULL argument: flash=%p is_mounted=%p", (const void *)flash,
			(const void *)is_mounted);
		return -EINVAL;
	}

	struct ubi_flash_res_peb_scan scan = { 0 };
	const int ret = ubi_flash_res_peb_scan(flash, &scan);

	if (ret != 0) {
		LOG_ERR("Reserved PEB scan failure");
		return ret;
	}

	*is_mounted = (scan.active_count >= 1 || scan.corrupt_count >= 1);
	return 0;
}

int ubi_dev_mount(const struct ubi_flash_desc *flash)
{
	if (!flash) {
		LOG_ERR("flash is NULL");
		return -EINVAL;
	}

	const struct flash_area *fa = NULL;
	int ret = flash_area_open(flash->partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failure");
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

	return ubi_flash_res_peb_overwrite(flash, (const uint8_t *)&dev_hdr, sizeof(dev_hdr));
}

int ubi_dev_hdr_read(const struct ubi_flash_desc *flash, struct ubi_dev_hdr *hdr)
{
	if (!flash || !hdr) {
		LOG_ERR("NULL argument: flash=%p hdr=%p", (const void *)flash, (const void *)hdr);
		return -EINVAL;
	}

	struct ubi_dev_hdr dev_hdr = { 0 };
	const int ret = ubi_flash_res_peb_validate(flash, &dev_hdr);

	if (ret != 0 && ret != -EROFS) {
		LOG_ERR("Reserved PEB validation failure");
		return ret;
	}

	memcpy(hdr, &dev_hdr, sizeof(dev_hdr));

	/* Propagate -EROFS so callers can detect degraded mode. */
	return ret;
}

int ubi_vol_hdr_read(const struct ubi_flash_desc *flash, const size_t index,
		     struct ubi_vol_hdr *hdr)
{
	if (!flash || index >= CONFIG_UBI_MAX_NR_OF_VOLUMES || !hdr) {
		LOG_ERR("Invalid argument: flash=%p index=%zu hdr=%p", (const void *)flash, index,
			(const void *)hdr);
		return -EINVAL;
	}

	/* Validate and recover reserved PEBs if needed */
	struct ubi_dev_hdr dev_hdr = { 0 };
	int ret = ubi_flash_res_peb_validate(flash, &dev_hdr);

	/* Allow reads in read-only degraded mode */
	if (ret != 0 && ret != -EROFS) {
		LOG_ERR("Reserved PEB validation failure");
		return ret;
	}

	/* Scan to find active PEBs for reading vol headers */
	struct ubi_flash_res_peb_scan scan = { 0 };
	ret = ubi_flash_res_peb_scan(flash, &scan);

	if (ret != 0) {
		LOG_ERR("Reserved PEB scan failure");
		return ret;
	}

	if (scan.active_count == 0) {
		LOG_ERR("No active reserved PEBs found");
		return -EIO;
	}

	const struct flash_area *fa = NULL;
	ret = flash_area_open(flash->partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		return ret;
	}

	/* Read volume header from the canonical (highest-revision) PEB. */
	struct ubi_vol_hdr vol_hdr = { 0 };
	const size_t offset = (scan.canonical_peb_idx * flash->erase_block_size) +
			      UBI_DEV_HDR_SIZE + (UBI_VOL_HDR_SIZE * index);

	ret = flash_area_read(fa, offset, &vol_hdr, sizeof(vol_hdr));

	if (ret != 0) {
		LOG_ERR("Volume header flash read failure");
		flash_area_close(fa);
		return ret;
	}

	if (vol_hdr.magic != UBI_VOL_HDR_MAGIC) {
		LOG_ERR("Volume header bad magic");
		flash_area_close(fa);
		return -EBADMSG;
	}

	const uint32_t crc =
		crc32_ieee((const uint8_t *)&vol_hdr, sizeof(vol_hdr) - sizeof(vol_hdr.hdr_crc));

	if (crc != vol_hdr.hdr_crc) {
		LOG_ERR("Volume header CRC mismatch");
		flash_area_close(fa);
		return -EBADMSG;
	}

	memcpy(hdr, &vol_hdr, sizeof(vol_hdr));

	flash_area_close(fa);
	return 0;
}

int ubi_vol_hdr_append(const struct ubi_flash_desc *flash, const struct ubi_dev_hdr *dev_hdr,
		       const struct ubi_vol_hdr *vol_hdr)
{
	if (!flash || !dev_hdr || !vol_hdr) {
		LOG_ERR("NULL argument: flash=%p dev_hdr=%p vol_hdr=%p", (const void *)flash,
			(const void *)dev_hdr, (const void *)vol_hdr);
		return -EINVAL;
	}

	int ret = -EIO;

	struct ubi_dev_hdr cur_hdr = { 0 };
	ret = ubi_flash_res_peb_validate(flash, &cur_hdr);

	if (ret != 0) {
		LOG_ERR("Reserved PEB validation failed");
		goto exit;
	}

	if (cur_hdr.vol_count >= CONFIG_UBI_MAX_NR_OF_VOLUMES) {
		LOG_ERR("Volume count exceeds maximum");
		ret = -ENOSPC;
		goto exit;
	}

	if (cur_hdr.vol_count + 1 != dev_hdr->vol_count) {
		LOG_ERR("Volume count mismatch in append");
		ret = -EACCES;
		goto exit;
	}

	const size_t content_len = UBI_DEV_HDR_SIZE + ((cur_hdr.vol_count + 1) * UBI_VOL_HDR_SIZE);

	uint8_t *content = NULL;
	ret = ubi_mem_scratch_alloc(content_len, &content);

	if (ret != 0) {
		LOG_ERR("Scratch allocation failed");
		goto exit;
	}

	/* Read existing content from canonical (highest-revision) PEB */
	struct ubi_flash_res_peb_scan scan = { 0 };
	ret = ubi_flash_res_peb_scan(flash, &scan);

	if (ret != 0) {
		LOG_ERR("Reserved PEB scan failed");
		goto exit;
	}

	if (scan.active_count == 0) {
		LOG_ERR("No active reserved PEBs found");
		ret = -EIO;
		goto exit;
	}

	ret = ubi_flash_res_peb_read_content(flash, scan.canonical_peb_idx, content,
					     content_len - UBI_VOL_HDR_SIZE);

	if (ret != 0) {
		LOG_ERR("Reserved PEB content read failed");
		goto exit;
	}

	memcpy(&content[0], dev_hdr, sizeof(*dev_hdr));
	memcpy(&content[content_len - UBI_VOL_HDR_SIZE], vol_hdr, sizeof(*vol_hdr));

	ret = ubi_flash_res_peb_commit(flash, content, content_len);

exit:
	ubi_mem_scratch_free(content);

	return ret;
}

int ubi_vol_hdr_remove(const struct ubi_flash_desc *flash, const struct ubi_dev_hdr *dev_hdr,
		       const uint32_t vol_id)
{
	if (!flash || !dev_hdr) {
		LOG_ERR("NULL argument: flash=%p dev_hdr=%p", (const void *)flash,
			(const void *)dev_hdr);
		return -EINVAL;
	}

	int ret = -EIO;
	uint8_t *content = NULL;

	struct ubi_dev_hdr cur_hdr = { 0 };
	ret = ubi_flash_res_peb_validate(flash, &cur_hdr);

	if (ret != 0) {
		LOG_ERR("Reserved PEB validation failed");
		goto exit;
	}

	if (cur_hdr.vol_count == 0) {
		LOG_ERR("No volumes to remove");
		ret = -EINVAL;
		goto exit;
	}

	if (cur_hdr.revision + 1 != dev_hdr->revision) {
		LOG_ERR("Revision mismatch in remove");
		ret = -EACCES;
		goto exit;
	}

	if (cur_hdr.vol_count - 1 != dev_hdr->vol_count) {
		LOG_ERR("Volume count mismatch in remove");
		ret = -EACCES;
		goto exit;
	}

	const size_t content_len = UBI_DEV_HDR_SIZE + (dev_hdr->vol_count * UBI_VOL_HDR_SIZE);
	size_t content_off = 0;

	ret = ubi_mem_scratch_alloc(content_len, &content);

	if (ret != 0) {
		LOG_ERR("Scratch allocation failed");
		goto exit;
	}

	memcpy(&content[content_off], dev_hdr, UBI_DEV_HDR_SIZE);
	content_off += UBI_DEV_HDR_SIZE;

	for (size_t i = 0; i < cur_hdr.vol_count; ++i) {
		struct ubi_vol_hdr exist_vol_hdr = { 0 };
		ret = ubi_vol_hdr_read(flash, i, &exist_vol_hdr);

		if (ret != 0) {
			LOG_ERR("Volume header read failed during remove");
			goto exit;
		}

		if (exist_vol_hdr.vol_id != vol_id) {
			memcpy(&content[content_off], &exist_vol_hdr, UBI_VOL_HDR_SIZE);
			content_off += UBI_VOL_HDR_SIZE;
		}
	}

	ret = ubi_flash_res_peb_commit(flash, content, content_len);

exit:
	ubi_mem_scratch_free(content);

	return ret;
}

int ubi_vol_hdr_update(const struct ubi_flash_desc *flash, const struct ubi_dev_hdr *dev_hdr,
		       uint32_t vol_id, size_t new_leb_count)
{
	if (!flash || !dev_hdr) {
		LOG_ERR("NULL argument: flash=%p dev_hdr=%p", (const void *)flash,
			(const void *)dev_hdr);
		return -EINVAL;
	}

	int ret = -EIO;
	uint8_t *content = NULL;

	struct ubi_dev_hdr cur_hdr = { 0 };
	ret = ubi_flash_res_peb_validate(flash, &cur_hdr);

	if (ret != 0) {
		LOG_ERR("Reserved PEB validation failed");
		goto exit;
	}

	if (cur_hdr.vol_count == 0) {
		LOG_ERR("No volumes to update");
		ret = -EINVAL;
		goto exit;
	}

	if (cur_hdr.revision + 1 != dev_hdr->revision) {
		LOG_ERR("Revision mismatch in update");
		ret = -EINVAL;
		goto exit;
	}

	const size_t content_len = UBI_DEV_HDR_SIZE + (cur_hdr.vol_count * UBI_VOL_HDR_SIZE);
	size_t content_off = 0;

	ret = ubi_mem_scratch_alloc(content_len, &content);

	if (ret != 0) {
		LOG_ERR("Scratch allocation failed");
		goto exit;
	}

	memcpy(&content[content_off], dev_hdr, UBI_DEV_HDR_SIZE);
	content_off += UBI_DEV_HDR_SIZE;

	for (size_t i = 0; i < cur_hdr.vol_count; ++i) {
		struct ubi_vol_hdr exist_vol_hdr = { 0 };
		ret = ubi_vol_hdr_read(flash, i, &exist_vol_hdr);

		if (ret != 0) {
			LOG_ERR("Volume header read failed during update");
			goto exit;
		}

		if (exist_vol_hdr.vol_id == vol_id) {
			exist_vol_hdr.leb_count = new_leb_count;
			exist_vol_hdr.hdr_crc =
				crc32_ieee((const uint8_t *)&exist_vol_hdr,
					   sizeof(exist_vol_hdr) - sizeof(exist_vol_hdr.hdr_crc));
		}

		memcpy(&content[content_off], &exist_vol_hdr, UBI_VOL_HDR_SIZE);
		content_off += UBI_VOL_HDR_SIZE;
	}

	if (content_off != content_len) {
		LOG_ERR("Content size mismatch after update assembly");
		ret = -EINVAL;
		goto exit;
	}

	ret = ubi_flash_res_peb_commit(flash, content, content_len);

exit:
	ubi_mem_scratch_free(content);

	return ret;
}
