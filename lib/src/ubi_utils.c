/**
 * \file    ubi.c
 * \author  Kamil Kielbasa
 * \brief   Unsorted Block Images (UBI) implementation.
 * \version 0.3
 * \date    2025-09-21
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files ------------------------------------------------------------------------------- */

/* Internal header: */
#include "ubi_utils.h"

/* Zephyr headers: */
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <zephyr/storage/flash_map.h>

/* Standard library headers: */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------ */
/* Module types and type definitions ----------------------------------------------------------- */
/* Module interface variables and constants ---------------------------------------------------- */
/* Static variables and constants -------------------------------------------------------------- */
/* Static function declarations ---------------------------------------------------------------- */
/* Static function definitions ----------------------------------------------------------------- */
/* Module interface function definitions ------------------------------------------------------- */

/** TODO: verify magic, version and crc32 for every write */

int ubi_dev_is_mounted(const struct ubi_mtd *mtd, bool *is_mounted)
{
	if (!mtd || !is_mounted)
		return -EINVAL;

	int ret = -EIO;
	size_t offset = 0;

	bool valid_1 = false;
	struct ubi_dev_hdr dev_hdr_1 = { 0 };

	bool valid_2 = false;
	struct ubi_dev_hdr dev_hdr_2 = { 0 };

	/* 1. Read first device header */
	const struct flash_area *fa = NULL;
	ret = flash_area_open(mtd->partition_id, &fa);

	if (0 != ret)
		goto exit;

	offset = UBI_DEV_HDR_RES_PEB_0 * mtd->erase_block_size;
	ret = flash_area_read(fa, offset, &dev_hdr_1, sizeof(dev_hdr_1));

	valid_1 = (0 == ret);

	if (valid_1) {
		valid_1 &= (UBI_DEV_HDR_MAGIC == dev_hdr_1.magic);

		const uint32_t crc = crc32_ieee((const uint8_t *)&dev_hdr_1,
						sizeof(dev_hdr_1) - sizeof(dev_hdr_1.hdr_crc));
		valid_1 &= (crc == dev_hdr_1.hdr_crc);
	}

	/* 2. Read second device header */
	offset = UBI_DEV_HDR_RES_PEB_1 * mtd->erase_block_size;
	ret = flash_area_read(fa, offset, &dev_hdr_2, sizeof(dev_hdr_2));

	valid_2 = (0 == ret);

	if (valid_2) {
		valid_2 &= (UBI_DEV_HDR_MAGIC == dev_hdr_2.magic);

		const uint32_t crc = crc32_ieee((const uint8_t *)&dev_hdr_2,
						sizeof(dev_hdr_2) - sizeof(dev_hdr_2.hdr_crc));
		valid_2 &= (crc == dev_hdr_2.hdr_crc);
	}

	*is_mounted = (valid_1 && valid_2);

exit:
	if (fa)
		flash_area_close(fa);

	return ret;
}

int ubi_dev_mount(const struct ubi_mtd *mtd)
{
	int ret = -EIO;
	size_t offset = 0;

	if (!mtd)
		return -EINVAL;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(mtd->partition_id, &fa);

	if (0 != ret)
		goto exit;

	struct ubi_dev_hdr dev_hdr = { 0 };
	dev_hdr.magic = UBI_DEV_HDR_MAGIC;
	dev_hdr.version = UBI_DEV_HDR_VERSION;
	dev_hdr.offset = fa->fa_off;
	dev_hdr.size = fa->fa_size;
	dev_hdr.revision = 0;
	dev_hdr.vol_count = 0;
	dev_hdr.hdr_crc =
		crc32_ieee((const uint8_t *)&dev_hdr, sizeof(dev_hdr) - sizeof(dev_hdr.hdr_crc));

	/* Save device header in first bank */
	offset = UBI_DEV_HDR_RES_PEB_0 * mtd->erase_block_size;
	ret = flash_area_erase(fa, offset, mtd->erase_block_size);

	if (0 != ret)
		goto exit;

	ret = flash_area_write(fa, offset, &dev_hdr, sizeof(dev_hdr));

	if (0 != ret)
		goto exit;

	/* Save device header in second bank */
	offset = UBI_DEV_HDR_RES_PEB_1 * mtd->erase_block_size;
	ret = flash_area_erase(fa, offset, mtd->erase_block_size);

	if (0 != ret)
		goto exit;

	ret = flash_area_write(fa, offset, &dev_hdr, sizeof(dev_hdr));

	if (0 != ret)
		goto exit;

exit:
	if (fa)
		flash_area_close(fa);

	return ret;
}

int ubi_dev_hdr_read(const struct ubi_mtd *mtd, struct ubi_dev_hdr *hdr)
{
	if (!mtd || !hdr)
		return -EINVAL;

	int ret = -EIO;
	size_t offset = 0;

	bool valid_1 = false;
	struct ubi_dev_hdr dev_hdr_1 = { 0 };

	bool valid_2 = false;
	struct ubi_dev_hdr dev_hdr_2 = { 0 };

	const struct flash_area *fa = NULL;
	ret = flash_area_open(mtd->partition_id, &fa);

	if (0 != ret)
		goto exit;

	/* 1. Read first device header */
	offset = UBI_DEV_HDR_RES_PEB_0 * mtd->erase_block_size;
	ret = flash_area_read(fa, offset, &dev_hdr_1, sizeof(dev_hdr_1));

	valid_1 = (0 == ret);

	if (valid_1) {
		valid_1 &= (UBI_DEV_HDR_MAGIC == dev_hdr_1.magic);

		const uint32_t crc = crc32_ieee((const uint8_t *)&dev_hdr_1,
						sizeof(dev_hdr_1) - sizeof(dev_hdr_1.hdr_crc));
		valid_1 &= (crc == dev_hdr_1.hdr_crc);
	}

	/* 2. Read second device header */
	offset = UBI_DEV_HDR_RES_PEB_1 * mtd->erase_block_size;
	ret = flash_area_read(fa, offset, &dev_hdr_2, sizeof(dev_hdr_2));

	valid_2 = (0 == ret);

	if (valid_2) {
		valid_2 &= (UBI_DEV_HDR_MAGIC == dev_hdr_2.magic);

		const uint32_t crc = crc32_ieee((const uint8_t *)&dev_hdr_2,
						sizeof(dev_hdr_2) - sizeof(dev_hdr_2.hdr_crc));
		valid_2 &= (crc == dev_hdr_2.hdr_crc);
	}

	flash_area_close(fa);

	/* 3. Use correct and validated device header */
	ret = -ENOENT;

	if (valid_1 && valid_2) {
		if (dev_hdr_1.revision != dev_hdr_2.revision) {
			ret = -EBADMSG;
			goto exit;
		}

		if (dev_hdr_1.hdr_crc != dev_hdr_2.hdr_crc) {
			ret = -EBADMSG;
			goto exit;
		}

		memcpy(hdr, &dev_hdr_1, sizeof(dev_hdr_1));
		ret = 0;
	}

	if (valid_1 && !valid_2) {
		/** TODO: dual-bank implementation */
		__ASSERT_NO_MSG(false);
	}

	if (valid_2 && !valid_1) {
		/** TODO: dual-bank implementation */
		__ASSERT_NO_MSG(false);
	}

exit:
	if (fa)
		flash_area_close(fa);

	return ret;
}

int ubi_vol_hdr_read(const struct ubi_mtd *mtd, const size_t index, struct ubi_vol_hdr *hdr)
{
	if (!mtd || index > CONFIG_UBI_MAX_NR_OF_VOLUMES || !hdr)
		return -EINVAL;

	int ret = -EIO;
	size_t offset = 0;

	bool valid_1 = false;
	struct ubi_dev_hdr dev_hdr_1 = { 0 };

	bool valid_2 = false;
	struct ubi_dev_hdr dev_hdr_2 = { 0 };

	const struct flash_area *fa = NULL;
	ret = flash_area_open(mtd->partition_id, &fa);

	if (0 != ret)
		goto exit;

	/* 1. Read first device header */
	offset = UBI_DEV_HDR_RES_PEB_0 * mtd->erase_block_size;
	ret = flash_area_read(fa, offset, &dev_hdr_1, sizeof(dev_hdr_1));

	valid_1 = (0 == ret);

	if (valid_1) {
		valid_1 &= (UBI_DEV_HDR_MAGIC == dev_hdr_1.magic);

		const uint32_t crc = crc32_ieee((const uint8_t *)&dev_hdr_1,
						sizeof(dev_hdr_1) - sizeof(dev_hdr_1.hdr_crc));
		valid_1 &= (crc == dev_hdr_1.hdr_crc);
	}

	/* 2. Read second device header */
	offset = UBI_DEV_HDR_RES_PEB_1 * mtd->erase_block_size;
	ret = flash_area_read(fa, offset, &dev_hdr_2, sizeof(dev_hdr_2));

	valid_2 = (0 == ret);

	if (valid_2) {
		valid_2 &= (UBI_DEV_HDR_MAGIC == dev_hdr_2.magic);

		const uint32_t crc = crc32_ieee((const uint8_t *)&dev_hdr_2,
						sizeof(dev_hdr_2) - sizeof(dev_hdr_2.hdr_crc));
		valid_2 &= (crc == dev_hdr_2.hdr_crc);
	}

	/* 3. Use correct and validated device header */
	ret = -EACCES;

	if (valid_1 && valid_2) {
		if (dev_hdr_1.revision != dev_hdr_2.revision) {
			ret = -EBADMSG;
			goto exit;
		}

		if (dev_hdr_1.hdr_crc != dev_hdr_2.hdr_crc) {
			ret = -EBADMSG;
			goto exit;
		}

		struct ubi_vol_hdr vol_hdr_1 = { 0 };
		struct ubi_vol_hdr vol_hdr_2 = { 0 };

		/* 3.1 Read VID header from first bank */
		offset = (UBI_DEV_HDR_RES_PEB_0 * mtd->erase_block_size) + UBI_DEV_HDR_SIZE +
			 (UBI_VOL_HDR_SIZE * index);
		ret = flash_area_read(fa, offset, &vol_hdr_1, sizeof(vol_hdr_1));

		valid_1 = (0 == ret);

		if (valid_1) {
			valid_1 &= (UBI_VOL_HDR_MAGIC == vol_hdr_1.magic);

			const uint32_t crc =
				crc32_ieee((const uint8_t *)&vol_hdr_1,
					   sizeof(vol_hdr_1) - sizeof(vol_hdr_1.hdr_crc));
			valid_1 &= (crc == vol_hdr_1.hdr_crc);
		}

		/* 3.2 Read VID header from second bank */
		offset = (UBI_DEV_HDR_RES_PEB_1 * mtd->erase_block_size) + UBI_DEV_HDR_SIZE +
			 (UBI_VOL_HDR_SIZE * index);
		ret = flash_area_read(fa, offset, &vol_hdr_2, sizeof(vol_hdr_2));

		valid_2 = (0 == ret);

		if (valid_2) {
			valid_2 &= (UBI_VOL_HDR_MAGIC == vol_hdr_2.magic);

			const uint32_t crc =
				crc32_ieee((const uint8_t *)&vol_hdr_2,
					   sizeof(vol_hdr_2) - sizeof(vol_hdr_2.hdr_crc));
			valid_2 &= (crc == vol_hdr_2.hdr_crc);
		}

		/* 3.3 VID headers from both banks are correct and validated */
		if (valid_1 && valid_2) {
			memcpy(hdr, &vol_hdr_1, sizeof(vol_hdr_1));
		}

		if (valid_1 && !valid_2) {
			/** TODO: dual-bank implementation */
			__ASSERT_NO_MSG(false);
		}

		if (valid_2 && !valid_1) {
			/** TODO: dual-bank implementation */
			__ASSERT_NO_MSG(false);
		}
	}

	if (valid_1 && !valid_2) {
		/** TODO: dual-bank implementation */
		__ASSERT_NO_MSG(false);
	}

	if (valid_2 && !valid_1) {
		/** TODO: dual-bank implementation */
		__ASSERT_NO_MSG(false);
	}

exit:
	if (fa)
		flash_area_close(fa);

	return ret;
}

int ubi_vol_hdr_append(const struct ubi_mtd *mtd, const struct ubi_dev_hdr *dev_hdr,
		       const struct ubi_vol_hdr *vol_hdr)
{
	if (!mtd || !dev_hdr || !vol_hdr)
		return -EINVAL;

	int ret = -EIO;
	size_t offset = 0;

	bool valid_1 = false;
	struct ubi_dev_hdr dev_hdr_1 = { 0 };

	bool valid_2 = false;
	struct ubi_dev_hdr dev_hdr_2 = { 0 };

	uint8_t *buf = NULL;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(mtd->partition_id, &fa);

	if (0 != ret)
		goto exit;

	/* 1. Read first device header */
	offset = UBI_DEV_HDR_RES_PEB_0 * mtd->erase_block_size;
	ret = flash_area_read(fa, offset, &dev_hdr_1, sizeof(dev_hdr_1));

	valid_1 = (0 == ret);

	if (valid_1) {
		valid_1 &= (UBI_DEV_HDR_MAGIC == dev_hdr_1.magic);

		const uint32_t crc = crc32_ieee((const uint8_t *)&dev_hdr_1,
						sizeof(dev_hdr_1) - sizeof(dev_hdr_1.hdr_crc));
		valid_1 &= (crc == dev_hdr_1.hdr_crc);
	}

	/* 2. Read second device header */
	offset = UBI_DEV_HDR_RES_PEB_1 * mtd->erase_block_size;
	ret = flash_area_read(fa, offset, &dev_hdr_2, sizeof(dev_hdr_2));

	valid_2 = (0 == ret);

	if (valid_2) {
		valid_2 &= (UBI_DEV_HDR_MAGIC == dev_hdr_2.magic);

		const uint32_t crc = crc32_ieee((const uint8_t *)&dev_hdr_2,
						sizeof(dev_hdr_2) - sizeof(dev_hdr_2.hdr_crc));
		valid_2 &= (crc == dev_hdr_2.hdr_crc);
	}

	/* 3. Use correct and validated device header */
	ret = -EACCES;

	if (valid_1 && valid_2) {
		if (dev_hdr_1.revision != dev_hdr_2.revision) {
			ret = -EBADMSG;
			goto exit;
		}

		if (dev_hdr_1.hdr_crc != dev_hdr_2.hdr_crc) {
			ret = -EBADMSG;
			goto exit;
		}

		if (dev_hdr_1.vol_count >= CONFIG_UBI_MAX_NR_OF_VOLUMES) {
			ret = -ENOSPC;
			goto exit;
		}

		if (dev_hdr_1.vol_count + 1 != dev_hdr->vol_count) {
			ret = -EACCES;
			goto exit;
		}

		/* 3.1 Allocate buffer for device header and volumes headers and copy it */
		const size_t buf_size =
			UBI_DEV_HDR_SIZE + ((dev_hdr_1.vol_count + 1) * UBI_VOL_HDR_SIZE);

		buf = k_malloc(buf_size);

		if (!buf) {
			ret = -ENOMEM;
			goto exit;
		}

		offset = UBI_DEV_HDR_RES_PEB_0 * mtd->erase_block_size;
		ret = flash_area_read(fa, offset, buf, buf_size - UBI_VOL_HDR_SIZE);

		if (0 != ret)
			goto exit;

		memcpy(&buf[0], dev_hdr, sizeof(*dev_hdr));
		memcpy(&buf[buf_size - UBI_VOL_HDR_SIZE], vol_hdr, sizeof(*vol_hdr));

		/* 3.2 Overwrite first bank */
		offset = UBI_DEV_HDR_RES_PEB_0 * mtd->erase_block_size;
		ret = flash_area_erase(fa, offset, mtd->erase_block_size);

		if (0 != ret)
			goto exit;

		ret = flash_area_write(fa, offset, buf, buf_size);

		if (0 != ret)
			goto exit;

		/* 3.2 Overwrite second bank */
		offset = UBI_DEV_HDR_RES_PEB_1 * mtd->erase_block_size;
		ret = flash_area_erase(fa, offset, mtd->erase_block_size);

		if (0 != ret)
			goto exit;

		ret = flash_area_write(fa, offset, buf, buf_size);

		if (0 != ret)
			goto exit;
	}

	if (valid_1 && !valid_2) {
		/** TODO: dual-bank implementation */
		__ASSERT_NO_MSG(false);
	}

	if (valid_2 && !valid_1) {
		/** TODO: dual-bank implementation */
		__ASSERT_NO_MSG(false);
	}

exit:
	if (fa)
		flash_area_close(fa);

	if (buf)
		k_free(buf);

	return ret;
}

int ubi_vol_hdr_remove(const struct ubi_mtd *mtd, const struct ubi_dev_hdr *dev_hdr,
		       const size_t index)
{
	if (!mtd || !dev_hdr)
		return -EINVAL;

	int ret = -EIO;
	size_t offset = 0;

	bool valid_1 = false;
	struct ubi_dev_hdr dev_hdr_1 = { 0 };

	bool valid_2 = false;
	struct ubi_dev_hdr dev_hdr_2 = { 0 };

	uint8_t *buf = NULL;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(mtd->partition_id, &fa);

	if (0 != ret)
		goto exit;

	/* 1. Read first device header */
	offset = UBI_DEV_HDR_RES_PEB_0 * mtd->erase_block_size;
	ret = flash_area_read(fa, offset, &dev_hdr_1, sizeof(dev_hdr_1));

	valid_1 = (0 == ret);

	if (valid_1) {
		valid_1 &= (UBI_DEV_HDR_MAGIC == dev_hdr_1.magic);

		const uint32_t crc = crc32_ieee((const uint8_t *)&dev_hdr_1,
						sizeof(dev_hdr_1) - sizeof(dev_hdr_1.hdr_crc));
		valid_1 &= (crc == dev_hdr_1.hdr_crc);
	}

	/* 2. Read second device header */
	offset = UBI_DEV_HDR_RES_PEB_1 * mtd->erase_block_size;
	ret = flash_area_read(fa, offset, &dev_hdr_2, sizeof(dev_hdr_2));

	valid_2 = (0 == ret);

	if (valid_2) {
		valid_2 &= (UBI_DEV_HDR_MAGIC == dev_hdr_2.magic);

		const uint32_t crc = crc32_ieee((const uint8_t *)&dev_hdr_2,
						sizeof(dev_hdr_2) - sizeof(dev_hdr_2.hdr_crc));
		valid_2 &= (crc == dev_hdr_2.hdr_crc);
	}

	/* 3. Use correct and validated device header */
	ret = -EACCES;

	if (valid_1 && valid_2) {
		if (dev_hdr_1.revision != dev_hdr_2.revision) {
			ret = -EBADMSG;
			goto exit;
		}

		if (dev_hdr_1.hdr_crc != dev_hdr_2.hdr_crc) {
			ret = -EBADMSG;
			goto exit;
		}

		if (dev_hdr_1.vol_count >= CONFIG_UBI_MAX_NR_OF_VOLUMES) {
			ret = -ENOSPC;
			goto exit;
		}

		if (index > (dev_hdr_1.vol_count - 1)) {
			ret = -EACCES;
			goto exit;
		}

		if (dev_hdr_1.revision + 1 != dev_hdr->revision) {
			ret = -EACCES;
			goto exit;
		}

		if (dev_hdr_1.vol_count - 1 != dev_hdr->vol_count) {
			ret = -EACCES;
			goto exit;
		}

		const size_t buf_size = UBI_DEV_HDR_SIZE + (dev_hdr->vol_count * UBI_VOL_HDR_SIZE);

		size_t buf_off = 0;
		buf = k_malloc(buf_size);

		if (!buf) {
			ret = -ENOMEM;
			goto exit;
		}

		memcpy(&buf[buf_off], dev_hdr, UBI_DEV_HDR_SIZE);
		buf_off += UBI_DEV_HDR_SIZE;

		for (size_t vol_idx = 0; vol_idx < dev_hdr_1.vol_count; ++vol_idx) {
			if (vol_idx != index) {
				struct ubi_vol_hdr exist_vol_hdr = { 0 };
				ret = ubi_vol_hdr_read(mtd, vol_idx, &exist_vol_hdr);

				if (0 != ret)
					goto exit;

				memcpy(&buf[buf_off], &exist_vol_hdr, UBI_VOL_HDR_SIZE);
				buf_off += UBI_VOL_HDR_SIZE;
			}
		}

		/* 3.2 Overwrite first bank */
		offset = UBI_DEV_HDR_RES_PEB_0 * mtd->erase_block_size;
		ret = flash_area_erase(fa, offset, mtd->erase_block_size);

		if (0 != ret)
			goto exit;

		ret = flash_area_write(fa, offset, buf, buf_size);

		if (0 != ret)
			goto exit;

		/* 3.3 Overwrite second bank */
		offset = UBI_DEV_HDR_RES_PEB_1 * mtd->erase_block_size;
		ret = flash_area_erase(fa, offset, mtd->erase_block_size);

		if (0 != ret)
			goto exit;

		ret = flash_area_write(fa, offset, buf, buf_size);

		if (0 != ret)
			goto exit;
	}

	if (valid_1 && !valid_2) {
		/** TODO: dual-bank implementation */
		__ASSERT_NO_MSG(false);
	}

	if (valid_2 && !valid_1) {
		/** TODO: dual-bank implementation */
		__ASSERT_NO_MSG(false);
	}

exit:
	if (fa)
		flash_area_close(fa);

	if (buf)
		k_free(buf);

	return ret;
}

int ubi_vol_hdr_update(const struct ubi_mtd *mtd, const struct ubi_dev_hdr *dev_hdr,
		       const size_t index, const struct ubi_vol_hdr *vol_hdr)
{
	if (!mtd || !dev_hdr)
		return -EINVAL;

	int ret = -EIO;
	size_t offset = 0;

	bool valid_1 = false;
	struct ubi_dev_hdr dev_hdr_1 = { 0 };

	bool valid_2 = false;
	struct ubi_dev_hdr dev_hdr_2 = { 0 };

	uint8_t *buf = NULL;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(mtd->partition_id, &fa);

	if (0 != ret)
		goto exit;

	/* 1. Read first device header */
	offset = UBI_DEV_HDR_RES_PEB_0 * mtd->erase_block_size;
	ret = flash_area_read(fa, offset, &dev_hdr_1, sizeof(dev_hdr_1));

	valid_1 = (0 == ret);

	if (valid_1) {
		valid_1 &= (UBI_DEV_HDR_MAGIC == dev_hdr_1.magic);

		const uint32_t crc = crc32_ieee((const uint8_t *)&dev_hdr_1,
						sizeof(dev_hdr_1) - sizeof(dev_hdr_1.hdr_crc));
		valid_1 &= (crc == dev_hdr_1.hdr_crc);
	}

	/* 2. Read second device header */
	offset = UBI_DEV_HDR_RES_PEB_1 * mtd->erase_block_size;
	ret = flash_area_read(fa, offset, &dev_hdr_2, sizeof(dev_hdr_2));

	valid_2 = (0 == ret);

	if (valid_2) {
		valid_2 &= (UBI_DEV_HDR_MAGIC == dev_hdr_2.magic);

		const uint32_t crc = crc32_ieee((const uint8_t *)&dev_hdr_2,
						sizeof(dev_hdr_2) - sizeof(dev_hdr_2.hdr_crc));
		valid_2 &= (crc == dev_hdr_2.hdr_crc);
	}

	/* 3. Use correct and validated device header */
	ret = -EACCES;

	if (valid_1 && valid_2) {
		if (dev_hdr_1.revision != dev_hdr_2.revision) {
			ret = -EBADMSG;
			goto exit;
		}

		if (dev_hdr_1.hdr_crc != dev_hdr_2.hdr_crc) {
			ret = -EBADMSG;
			goto exit;
		}

		if (dev_hdr_1.vol_count >= CONFIG_UBI_MAX_NR_OF_VOLUMES) {
			ret = -ENOSPC;
			goto exit;
		}

		if (index > (dev_hdr_1.vol_count - 1)) {
			ret = -EACCES;
			goto exit;
		}

		if (dev_hdr_1.revision + 1 != dev_hdr->revision) {
			ret = -EACCES;
			goto exit;
		}

		const size_t buf_size = UBI_DEV_HDR_SIZE + (dev_hdr_1.vol_count * UBI_VOL_HDR_SIZE);

		size_t buf_off = 0;
		buf = k_malloc(buf_size);

		if (!buf) {
			ret = -ENOMEM;
			goto exit;
		}

		memcpy(&buf[buf_off], dev_hdr, UBI_DEV_HDR_SIZE);
		buf_off += UBI_DEV_HDR_SIZE;

		for (size_t vol_idx = 0; vol_idx < dev_hdr_1.vol_count; ++vol_idx) {
			if (vol_idx != index) {
				struct ubi_vol_hdr exist_vol_hdr = { 0 };
				ret = ubi_vol_hdr_read(mtd, vol_idx, &exist_vol_hdr);

				if (0 != ret)
					goto exit;

				memcpy(&buf[buf_off], &exist_vol_hdr, UBI_VOL_HDR_SIZE);
				buf_off += UBI_VOL_HDR_SIZE;
			} else {
				memcpy(&buf[buf_off], vol_hdr, sizeof(*vol_hdr));
				buf_off += UBI_VOL_HDR_SIZE;
			}
		}

		if (buf_off != buf_size) {
			ret = -EACCES;
			goto exit;
		}

		/* 3.2 Overwrite first bank */
		offset = UBI_DEV_HDR_RES_PEB_0 * mtd->erase_block_size;
		ret = flash_area_erase(fa, offset, mtd->erase_block_size);

		if (0 != ret)
			goto exit;

		ret = flash_area_write(fa, offset, buf, buf_size);

		if (0 != ret)
			goto exit;

		/* 3.3 Overwrite second bank */
		offset = UBI_DEV_HDR_RES_PEB_1 * mtd->erase_block_size;
		ret = flash_area_erase(fa, offset, mtd->erase_block_size);

		if (0 != ret)
			goto exit;

		ret = flash_area_write(fa, offset, buf, buf_size);

		if (0 != ret)
			goto exit;
	}

	if (valid_1 && !valid_2) {
		/** TODO: dual-bank implementation */
		__ASSERT_NO_MSG(false);
	}

	if (valid_2 && !valid_1) {
		/** TODO: dual-bank implementation */
		__ASSERT_NO_MSG(false);
	}

exit:
	if (fa)
		flash_area_close(fa);

	if (buf)
		k_free(buf);

	return ret;
}

int ubi_ec_hdr_read(const struct ubi_mtd *mtd, const size_t pnum, struct ubi_ec_hdr *hdr)
{
	int ret = -EIO;

	if (!mtd)
		return -EINVAL;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(mtd->partition_id, &fa);

	if (0 != ret)
		return ret;

	const size_t nr_of_pebs = fa->fa_size / mtd->erase_block_size;

	if (nr_of_pebs < pnum || UBI_DEV_HDR_RES_PEB_0 == pnum || UBI_DEV_HDR_RES_PEB_1 == pnum) {
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

	if (0 != ret)
		goto exit;

	const size_t nr_of_pebs = fa->fa_size / mtd->erase_block_size;

	if (nr_of_pebs < pnum || UBI_DEV_HDR_RES_PEB_0 == pnum || UBI_DEV_HDR_RES_PEB_1 == pnum) {
		ret = -EINVAL;
		goto exit;
	}

	ret = flash_area_write(fa, pnum * mtd->erase_block_size, hdr, sizeof(*hdr));

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

	if (0 != ret)
		return ret;

	const size_t total_nr_of_pebs = fa->fa_size / mtd->erase_block_size;

	if (pnum > total_nr_of_pebs || UBI_DEV_HDR_RES_PEB_0 == pnum ||
	    UBI_DEV_HDR_RES_PEB_1 == pnum) {
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
			return -EBADMSG;
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

	if (0 != ret)
		goto exit;

	const size_t total_nr_of_pebs = fa->fa_size / mtd->erase_block_size;

	if (pnum > total_nr_of_pebs || UBI_DEV_HDR_RES_PEB_0 == pnum ||
	    UBI_DEV_HDR_RES_PEB_1 == pnum) {
		ret = -EINVAL;
		goto exit;
	}

	ret = flash_area_write(fa, (pnum * mtd->erase_block_size) + UBI_EC_HDR_SIZE, vid_hdr,
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

	if (0 != ret)
		goto exit;

	const size_t total_nr_of_pebs = fa->fa_size / mtd->erase_block_size;

	if (pnum > total_nr_of_pebs || UBI_DEV_HDR_RES_PEB_0 == pnum ||
	    UBI_DEV_HDR_RES_PEB_1 == pnum) {
		ret = -EINVAL;
		goto exit;
	}

	if (len > (mtd->erase_block_size - UBI_EC_HDR_SIZE - UBI_VID_HDR_SIZE)) {
		ret = -ENOSPC;
		goto exit;
	}

	size_t offset = (pnum * mtd->erase_block_size) + UBI_EC_HDR_SIZE + UBI_VID_HDR_SIZE;

	if (0 == len % WRITE_BLOCK_SIZE_ALIGNMENT) {
		ret = flash_area_write(fa, offset, buf, len);

		if (0 != ret)
			goto exit;
	} else {
		if (len < WRITE_BLOCK_SIZE_ALIGNMENT) {
			uint8_t align_buf[WRITE_BLOCK_SIZE_ALIGNMENT] = { 0 };
			memcpy(align_buf, buf, len);

			ret = flash_area_write(fa, offset, align_buf, ARRAY_SIZE(align_buf));

			if (0 != ret)
				goto exit;
		} else {
			const size_t left_size = len % WRITE_BLOCK_SIZE_ALIGNMENT;

			uint8_t align_buf[WRITE_BLOCK_SIZE_ALIGNMENT] = { 0 };
			memcpy(align_buf, &buf[len - left_size], left_size);

			ret = flash_area_write(fa, offset, buf, len - left_size);

			if (0 != ret)
				goto exit;

			ret = flash_area_write(fa, offset + len - left_size, align_buf,
					       ARRAY_SIZE(align_buf));

			if (0 != ret)
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

	if (0 != ret)
		goto exit;

	const size_t total_nr_of_pebs = fa->fa_size / mtd->erase_block_size;

	if (pnum > total_nr_of_pebs || UBI_DEV_HDR_RES_PEB_0 == pnum ||
	    UBI_DEV_HDR_RES_PEB_1 == pnum) {
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

	if (0 != ret)
		goto exit;

exit:
	if (fa)
		flash_area_close(fa);

	return ret;
}
