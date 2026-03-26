/**
 * \file    ubi_io.c
 * \author  Kamil Kielbasa
 * \brief   UBI flash I/O operations.
 * \version 0.9
 * \date    2026-03-26
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files ------------------------------------------------------------------------------- */

/* Internal header: */
#include "ubi_io.h"

/* Zephyr headers: */
#include <zephyr/kernel.h>
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
/* Module types and type definitions ----------------------------------------------------------- */

enum dual_bank_state { BANKS_INVALID, BANKS_VALID, BANK1_VALID, BANK2_VALID };

/* Module interface variables and constants ---------------------------------------------------- */
/* Static variables and constants -------------------------------------------------------------- */
/* Static function declarations ---------------------------------------------------------------- */

/**
 * \brief Read the device headers from a UBI device.
 *
 * \param[in] mtd		UBI MTD device structure.
 * \param[out] db_state   	Dual-bank state.
 * \param[out] dev_hdr_1  	First device header.
 * \param[out] dev_hdr_2  	Second device header.
 *
 * \return 0 on success, negative error code on failure.
 */
static int get_dev_hdr(const struct ubi_mtd *mtd, enum dual_bank_state *db_state,
		       struct ubi_dev_hdr *dev_hdr_1, struct ubi_dev_hdr *dev_hdr_2);

/**
 * \brief Overwrite the device and volume headers on a UBI device.
 *
 * \param[in] mtd       	UBI MTD device structure.
 * \param[out] db_state  	Dual-bank state.
 * \param[in] buf       	Buffer containing the new device and volumes data.
 * \param len       		Size of the \p buf in bytes.
 *
 * \return 0 on success, negative error code on failure.
 */
static int overwrite_dev_and_vol_hdrs(const struct ubi_mtd *mtd, enum dual_bank_state *db_state,
				      const uint8_t *buf, size_t len);

/**
 * \brief Validate that both device header banks are in sync.
 *
 * Reads dual-bank headers and checks that the state is BANKS_VALID.
 *
 * \param[in] mtd       	UBI MTD device structure.
 * \param[out] dev_hdr		Current device header (from bank 1).
 *
 * \return 0 on success (BANKS_VALID), negative error code otherwise.
 */
static int validate_dual_bank(const struct ubi_mtd *mtd, struct ubi_dev_hdr *dev_hdr);

/**
 * \brief Commit updated device and volume headers to both banks.
 *
 * Overwrites both banks and verifies the write succeeded (BANKS_VALID).
 *
 * \param[in] mtd       	UBI MTD device structure.
 * \param[in] buf       	Buffer with new headers.
 * \param len       		Size of \p buf in bytes.
 *
 * \return 0 on success, negative error code on failure.
 */
static int commit_dual_bank(const struct ubi_mtd *mtd, const uint8_t *buf, size_t len);

/* Static function definitions ----------------------------------------------------------------- */

static int get_dev_hdr(const struct ubi_mtd *mtd, enum dual_bank_state *db_state,
		       struct ubi_dev_hdr *dev_hdr_1, struct ubi_dev_hdr *dev_hdr_2)
{
	__ASSERT_NO_MSG(mtd);
	__ASSERT_NO_MSG(db_state);
	__ASSERT_NO_MSG(dev_hdr_1);
	__ASSERT_NO_MSG(dev_hdr_2);

	int ret = -EIO;

	size_t offset = 0;
	uint32_t crc = 0;

	bool valid_1 = false;
	bool valid_2 = false;

	struct ubi_dev_hdr hdr_1 = { 0 };
	struct ubi_dev_hdr hdr_2 = { 0 };

	const struct flash_area *fa = NULL;
	ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0)
		return ret;

	/* Read first device header */
	offset = UBI_DEV_HDR_RES_PEB_0 * mtd->erase_block_size;
	ret = flash_area_read(fa, offset, &hdr_1, sizeof(hdr_1));

	valid_1 = (0 == ret);

	if (valid_1) {
		valid_1 &= (UBI_DEV_HDR_MAGIC == hdr_1.magic);

		crc = crc32_ieee((const uint8_t *)&hdr_1, sizeof(hdr_1) - sizeof(hdr_1.hdr_crc));
		valid_1 &= (crc == hdr_1.hdr_crc);
	}

	/* Read second device header */
	offset = UBI_DEV_HDR_RES_PEB_1 * mtd->erase_block_size;
	ret = flash_area_read(fa, offset, &hdr_2, sizeof(hdr_2));

	valid_2 = (0 == ret);

	if (valid_2) {
		valid_2 &= (UBI_DEV_HDR_MAGIC == hdr_2.magic);

		crc = crc32_ieee((const uint8_t *)&hdr_2, sizeof(hdr_2) - sizeof(hdr_2.hdr_crc));
		valid_2 &= (crc == hdr_2.hdr_crc);
	}

	/* Check dual-bank device headers state */
	*db_state = BANKS_INVALID;

	if (valid_1 && valid_2) {
		if ((hdr_1.hdr_crc == hdr_2.hdr_crc) && (hdr_1.revision == hdr_2.revision)) {
			*db_state = BANKS_VALID;

			if (dev_hdr_1)
				*dev_hdr_1 = hdr_1;

			if (dev_hdr_2)
				*dev_hdr_2 = hdr_2;
		}
	}

	if (valid_1 && !valid_2) {
		*db_state = BANK1_VALID;

		if (dev_hdr_1)
			*dev_hdr_1 = hdr_1;
	}

	if (valid_2 && !valid_1) {
		*db_state = BANK2_VALID;

		if (dev_hdr_2)
			*dev_hdr_2 = hdr_2;
	}

	flash_area_close(fa);
	return 0;
}

static int overwrite_dev_and_vol_hdrs(const struct ubi_mtd *mtd, enum dual_bank_state *db_state,
				      const uint8_t *buf, size_t len)
{
	__ASSERT_NO_MSG(mtd);
	__ASSERT_NO_MSG(db_state);
	__ASSERT_NO_MSG(buf);
	__ASSERT_NO_MSG(0 != len);

	if (len > mtd->erase_block_size)
		return -EINVAL;

	int ret = -EIO;
	size_t offset = 0;

	*db_state = BANKS_INVALID;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0)
		goto exit;

	offset = UBI_DEV_HDR_RES_PEB_0 * mtd->erase_block_size;
	ret = flash_area_erase(fa, offset, mtd->erase_block_size);

	if (ret != 0)
		goto exit;

	ret = flash_area_write(fa, offset, buf, len);

	if (ret != 0)
		goto exit;

	*db_state = BANK1_VALID;

	offset = UBI_DEV_HDR_RES_PEB_1 * mtd->erase_block_size;
	ret = flash_area_erase(fa, offset, mtd->erase_block_size);

	if (ret != 0)
		goto exit;

	ret = flash_area_write(fa, offset, buf, len);

	if (ret != 0)
		goto exit;

	*db_state = BANKS_VALID;

exit:
	if (fa)
		flash_area_close(fa);

	return ret;
}

static int validate_dual_bank(const struct ubi_mtd *mtd, struct ubi_dev_hdr *dev_hdr)
{
	__ASSERT_NO_MSG(mtd);
	__ASSERT_NO_MSG(dev_hdr);

	enum dual_bank_state db_state = BANKS_INVALID;
	struct ubi_dev_hdr dev_hdr_1 = { 0 };
	struct ubi_dev_hdr dev_hdr_2 = { 0 };

	int ret = get_dev_hdr(mtd, &db_state, &dev_hdr_1, &dev_hdr_2);

	if (ret != 0)
		return ret;

	if (db_state != BANKS_VALID)
		return -ENOSYS;

	*dev_hdr = dev_hdr_1;
	return 0;
}

static int commit_dual_bank(const struct ubi_mtd *mtd, const uint8_t *buf, size_t len)
{
	__ASSERT_NO_MSG(mtd);
	__ASSERT_NO_MSG(buf);

	enum dual_bank_state write_db_state = BANKS_INVALID;
	int ret = overwrite_dev_and_vol_hdrs(mtd, &write_db_state, buf, len);

	if (write_db_state != BANKS_VALID)
		return (0 == ret) ? -ENOSYS : ret;

	return ret;
}

/* Module interface function definitions ------------------------------------------------------- */

int ubi_dev_is_mounted(const struct ubi_mtd *mtd, bool *is_mounted)
{
	if (!mtd || !is_mounted)
		return -EINVAL;

	int ret = -EIO;

	enum dual_bank_state db_state = BANKS_INVALID;
	struct ubi_dev_hdr dev_hdr_1 = { 0 };
	struct ubi_dev_hdr dev_hdr_2 = { 0 };

	/* 1. Read first device header */
	ret = get_dev_hdr(mtd, &db_state, &dev_hdr_1, &dev_hdr_2);

	if (ret != 0)
		return ret;

	*is_mounted = false;

	switch (db_state) {
	case BANKS_VALID:
		*is_mounted = true;
		break;

	case BANKS_INVALID:
	case BANK1_VALID:
	case BANK2_VALID:
		break;
	}

	return 0;
}

int ubi_dev_mount(const struct ubi_mtd *mtd)
{
	if (!mtd)
		return -EINVAL;

	int ret = -EIO;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0)
		return ret;

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

	enum dual_bank_state db_state = BANKS_INVALID;
	ret = overwrite_dev_and_vol_hdrs(mtd, &db_state, (const uint8_t *)&dev_hdr,
					 sizeof(dev_hdr));

	switch (db_state) {
	case BANKS_VALID:
		return ret;

	case BANKS_INVALID:
	case BANK1_VALID:
	case BANK2_VALID:
		/** TODO: dual-bank implementation */
		return -ENOSYS;
	}

	return -EACCES;
}

int ubi_dev_hdr_read(const struct ubi_mtd *mtd, struct ubi_dev_hdr *hdr)
{
	if (!mtd || !hdr)
		return -EINVAL;

	int ret = -EIO;

	enum dual_bank_state db_state = BANKS_INVALID;
	struct ubi_dev_hdr dev_hdr_1 = { 0 };
	struct ubi_dev_hdr dev_hdr_2 = { 0 };

	ret = get_dev_hdr(mtd, &db_state, &dev_hdr_1, &dev_hdr_2);

	if (ret != 0)
		return ret;

	switch (db_state) {
	case BANKS_VALID:
		memcpy(hdr, &dev_hdr_1, sizeof(dev_hdr_1));
		return 0;

	case BANKS_INVALID:
	case BANK1_VALID:
	case BANK2_VALID:
		/** TODO: dual-bank implementation */
		return -ENOSYS;
	}

	return -EACCES;
}

int ubi_vol_hdr_read(const struct ubi_mtd *mtd, const size_t index, struct ubi_vol_hdr *hdr)
{
	if (!mtd || index > CONFIG_UBI_MAX_NR_OF_VOLUMES || !hdr)
		return -EINVAL;

	int ret = -EIO;
	size_t offset = 0;
	uint32_t crc = 0;

	enum dual_bank_state db_state = BANKS_INVALID;
	struct ubi_dev_hdr dev_hdr_1 = { 0 };
	struct ubi_dev_hdr dev_hdr_2 = { 0 };

	ret = get_dev_hdr(mtd, &db_state, &dev_hdr_1, &dev_hdr_2);

	if (ret != 0)
		return ret;

	switch (db_state) {
	case BANKS_VALID: {
		bool valid_1 = false;
		bool valid_2 = false;

		struct ubi_vol_hdr vol_hdr_1 = { 0 };
		struct ubi_vol_hdr vol_hdr_2 = { 0 };

		const struct flash_area *fa = NULL;
		ret = flash_area_open(mtd->partition_id, &fa);

		if (ret != 0)
			return ret;

		/* 3.1 Read VID header from first bank */
		offset = (UBI_DEV_HDR_RES_PEB_0 * mtd->erase_block_size) + UBI_DEV_HDR_SIZE +
			 (UBI_VOL_HDR_SIZE * index);
		ret = flash_area_read(fa, offset, &vol_hdr_1, sizeof(vol_hdr_1));

		valid_1 = (0 == ret);

		if (valid_1) {
			valid_1 &= (UBI_VOL_HDR_MAGIC == vol_hdr_1.magic);

			crc = crc32_ieee((const uint8_t *)&vol_hdr_1,
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

			crc = crc32_ieee((const uint8_t *)&vol_hdr_2,
					 sizeof(vol_hdr_2) - sizeof(vol_hdr_2.hdr_crc));
			valid_2 &= (crc == vol_hdr_2.hdr_crc);
		}

		flash_area_close(fa);

		/* 3.3 VID headers from both banks are correct and validated */
		if (valid_1 && valid_2) {
			memcpy(hdr, &vol_hdr_1, sizeof(vol_hdr_1));
			return 0;
		}

		if (valid_1 && !valid_2) {
			/** TODO: dual-bank implementation */
			return -ENOSYS;
		}

		if (valid_2 && !valid_1) {
			/** TODO: dual-bank implementation */
			return -ENOSYS;
		}

		return -EBADMSG;
	}

	case BANKS_INVALID:
	case BANK1_VALID:
	case BANK2_VALID:
		/** TODO: dual-bank implementation */
		return -ENOSYS;
	}

	return -EACCES;
}

int ubi_vol_hdr_append(const struct ubi_mtd *mtd, const struct ubi_dev_hdr *dev_hdr,
		       const struct ubi_vol_hdr *vol_hdr)
{
	if (!mtd || !dev_hdr || !vol_hdr)
		return -EINVAL;

	int ret = -EIO;
	const struct flash_area *fa = NULL;
	uint8_t *buf = NULL;

	struct ubi_dev_hdr cur_hdr = { 0 };
	ret = validate_dual_bank(mtd, &cur_hdr);

	if (ret != 0)
		goto exit;

	if (cur_hdr.vol_count >= CONFIG_UBI_MAX_NR_OF_VOLUMES) {
		ret = -ENOSPC;
		goto exit;
	}

	if (cur_hdr.vol_count + 1 != dev_hdr->vol_count) {
		ret = -EACCES;
		goto exit;
	}

	const size_t buf_size = UBI_DEV_HDR_SIZE + ((cur_hdr.vol_count + 1) * UBI_VOL_HDR_SIZE);

	buf = k_malloc(buf_size);

	if (!buf) {
		ret = -ENOMEM;
		goto exit;
	}

	ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0)
		goto exit;

	const size_t offset = UBI_DEV_HDR_RES_PEB_0 * mtd->erase_block_size;
	ret = flash_area_read(fa, offset, buf, buf_size - UBI_VOL_HDR_SIZE);

	if (ret != 0)
		goto exit;

	memcpy(&buf[0], dev_hdr, sizeof(*dev_hdr));
	memcpy(&buf[buf_size - UBI_VOL_HDR_SIZE], vol_hdr, sizeof(*vol_hdr));

	ret = commit_dual_bank(mtd, buf, buf_size);

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
	uint8_t *buf = NULL;

	struct ubi_dev_hdr cur_hdr = { 0 };
	ret = validate_dual_bank(mtd, &cur_hdr);

	if (ret != 0)
		goto exit;

	if (cur_hdr.vol_count >= CONFIG_UBI_MAX_NR_OF_VOLUMES) {
		ret = -ENOSPC;
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

	const size_t buf_size = UBI_DEV_HDR_SIZE + (dev_hdr->vol_count * UBI_VOL_HDR_SIZE);
	size_t buf_off = 0;

	buf = k_malloc(buf_size);

	if (!buf) {
		ret = -ENOMEM;
		goto exit;
	}

	memcpy(&buf[buf_off], dev_hdr, UBI_DEV_HDR_SIZE);
	buf_off += UBI_DEV_HDR_SIZE;

	for (size_t vol_idx = 0; vol_idx < cur_hdr.vol_count; ++vol_idx) {
		if (vol_idx != index) {
			struct ubi_vol_hdr exist_vol_hdr = { 0 };
			ret = ubi_vol_hdr_read(mtd, vol_idx, &exist_vol_hdr);

			if (ret != 0)
				goto exit;

			memcpy(&buf[buf_off], &exist_vol_hdr, UBI_VOL_HDR_SIZE);
			buf_off += UBI_VOL_HDR_SIZE;
		}
	}

	ret = commit_dual_bank(mtd, buf, buf_size);

exit:
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
	uint8_t *buf = NULL;

	struct ubi_dev_hdr cur_hdr = { 0 };
	ret = validate_dual_bank(mtd, &cur_hdr);

	if (ret != 0)
		goto exit;

	if (cur_hdr.vol_count >= CONFIG_UBI_MAX_NR_OF_VOLUMES) {
		ret = -ENOSPC;
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

	const size_t buf_size = UBI_DEV_HDR_SIZE + (cur_hdr.vol_count * UBI_VOL_HDR_SIZE);
	size_t buf_off = 0;

	buf = k_malloc(buf_size);

	if (!buf) {
		ret = -ENOMEM;
		goto exit;
	}

	memcpy(&buf[buf_off], dev_hdr, UBI_DEV_HDR_SIZE);
	buf_off += UBI_DEV_HDR_SIZE;

	for (size_t vol_idx = 0; vol_idx < cur_hdr.vol_count; ++vol_idx) {
		if (vol_idx != index) {
			struct ubi_vol_hdr exist_vol_hdr = { 0 };
			ret = ubi_vol_hdr_read(mtd, vol_idx, &exist_vol_hdr);

			if (ret != 0)
				goto exit;

			memcpy(&buf[buf_off], &exist_vol_hdr, UBI_VOL_HDR_SIZE);
			buf_off += UBI_VOL_HDR_SIZE;
		} else {
			memcpy(&buf[buf_off], vol_hdr, sizeof(*vol_hdr));
			buf_off += UBI_VOL_HDR_SIZE;
		}
	}

	if (buf_off != buf_size) {
		ret = -EINVAL;
		goto exit;
	}

	ret = commit_dual_bank(mtd, buf, buf_size);

exit:
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

	if (ret != 0)
		return ret;

	const size_t nr_of_pebs = fa->fa_size / mtd->erase_block_size;

	if (pnum >= nr_of_pebs || UBI_DEV_HDR_RES_PEB_0 == pnum || UBI_DEV_HDR_RES_PEB_1 == pnum) {
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

	if (pnum >= nr_of_pebs || UBI_DEV_HDR_RES_PEB_0 == pnum || UBI_DEV_HDR_RES_PEB_1 == pnum) {
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

	if (ret != 0)
		return ret;

	const size_t nr_of_pebs = fa->fa_size / mtd->erase_block_size;

	if (pnum >= nr_of_pebs || UBI_DEV_HDR_RES_PEB_0 == pnum || UBI_DEV_HDR_RES_PEB_1 == pnum) {
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

	if (pnum >= nr_of_pebs || UBI_DEV_HDR_RES_PEB_0 == pnum || UBI_DEV_HDR_RES_PEB_1 == pnum) {
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

	if (ret != 0)
		goto exit;

	const size_t nr_of_pebs = fa->fa_size / mtd->erase_block_size;

	if (pnum >= nr_of_pebs || UBI_DEV_HDR_RES_PEB_0 == pnum || UBI_DEV_HDR_RES_PEB_1 == pnum) {
		ret = -EINVAL;
		goto exit;
	}

	if (len > (mtd->erase_block_size - UBI_EC_HDR_SIZE - UBI_VID_HDR_SIZE)) {
		ret = -ENOSPC;
		goto exit;
	}

	size_t offset = (pnum * mtd->erase_block_size) + UBI_EC_HDR_SIZE + UBI_VID_HDR_SIZE;

	if (len % WRITE_BLOCK_SIZE_ALIGNMENT == 0) {
		ret = flash_area_write(fa, offset, buf, len);

		if (ret != 0)
			goto exit;
	} else {
		if (len < WRITE_BLOCK_SIZE_ALIGNMENT) {
			uint8_t align_buf[WRITE_BLOCK_SIZE_ALIGNMENT] = { 0 };
			memcpy(align_buf, buf, len);

			ret = flash_area_write(fa, offset, align_buf, ARRAY_SIZE(align_buf));

			if (ret != 0)
				goto exit;
		} else {
			const size_t left_size = len % WRITE_BLOCK_SIZE_ALIGNMENT;

			uint8_t align_buf[WRITE_BLOCK_SIZE_ALIGNMENT] = { 0 };
			memcpy(align_buf, &buf[len - left_size], left_size);

			ret = flash_area_write(fa, offset, buf, len - left_size);

			if (ret != 0)
				goto exit;

			ret = flash_area_write(fa, offset + len - left_size, align_buf,
					       ARRAY_SIZE(align_buf));

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

	if (pnum >= nr_of_pebs || UBI_DEV_HDR_RES_PEB_0 == pnum || UBI_DEV_HDR_RES_PEB_1 == pnum) {
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
