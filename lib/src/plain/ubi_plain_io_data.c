/**
 * \file    ubi_io_data.c
 * \author  Kamil Kielbasa
 * \brief   UBI data I/O: EC/VID header and LEB data read/write.
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files -------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_plain_io.h"

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

/* Module defines ------------------------------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Flash I/O fault injection -------------------------------------------------------------------- */

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)

static int flash_write_remaining = -1;
static int flash_erase_remaining = -1;

void ubi_test_fault_set_flash_write_fail_after(int n)
{
	flash_write_remaining = n;
}

void ubi_test_fault_set_flash_erase_fail_after(int n)
{
	flash_erase_remaining = n;
}

static inline bool flash_write_should_fail(void)
{
	if (flash_write_remaining == 0) {
		return true;
	}
	if (flash_write_remaining > 0) {
		flash_write_remaining--;
	}
	return false;
}

static inline bool flash_erase_should_fail(void)
{
	if (flash_erase_remaining == 0) {
		return true;
	}
	if (flash_erase_remaining > 0) {
		flash_erase_remaining--;
	}
	return false;
}

bool ubi_test_flash_erase_check_fail(void)
{
	return flash_erase_should_fail();
}

bool ubi_test_flash_write_check_fail(void)
{
	return flash_write_should_fail();
}

#endif /* CONFIG_UBI_TEST_FAULT_INJECTION */

/* Static function declarations ----------------------------------------------------------------- */

static int flash_write_with_retry(const struct flash_area *fa, off_t offset, const void *data,
				  size_t len);

/* Static function definitions ------------------------------------------------------------------ */

static int flash_write_with_retry(const struct flash_area *fa, off_t offset, const void *data,
				  size_t len)
{
	int ret = -EIO;

	for (size_t attempt = 1; attempt <= CONFIG_UBI_PEB_WRITE_RETRY_COUNT; attempt++) {
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
		if (flash_write_should_fail()) {
			LOG_WRN("Flash write fault injected at offset 0x%lx",
				(unsigned long)offset);
			return -EIO;
		}
#endif /* CONFIG_UBI_TEST_FAULT_INJECTION */
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

/* Module interface function definitions -------------------------------------------------------- */

int ubi_ec_hdr_read(const struct ubi_flash_desc *flash, const size_t pnum, struct ubi_ec_hdr *hdr)
{
	int ret = -EIO;

	if (!flash)
		return -EINVAL;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(flash->partition_id, &fa);

	if (ret != 0)
		return ret;

	const size_t nr_of_pebs = fa->fa_size / flash->erase_block_size;

	if (pnum >= nr_of_pebs || pnum < UBI_DEV_HDR_NR_OF_RES_PEBS) {
		LOG_ERR("PEB index %zu out of range", pnum);
		ret = -EINVAL;
		goto exit;
	}

	struct ubi_ec_hdr ec_hdr = { 0 };
	ret = flash_area_read(fa, pnum * flash->erase_block_size, &ec_hdr, sizeof(ec_hdr));

	if (ret != 0) {
		LOG_ERR("EC header flash read failure for PEB %zu", pnum);
		goto exit;
	}

	if (UBI_EC_HDR_MAGIC != ec_hdr.magic ||
	    ec_hdr.hdr_crc !=
		    crc32_ieee((const uint8_t *)&ec_hdr, sizeof(ec_hdr) - sizeof(ec_hdr.hdr_crc))) {
		LOG_ERR("EC header corrupt on PEB %zu", pnum);
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

int ubi_ec_hdr_write(const struct ubi_flash_desc *flash, const size_t pnum,
		     const struct ubi_ec_hdr *hdr)
{
	int ret = -EIO;

	if (!flash || !hdr)
		return -EINVAL;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(flash->partition_id, &fa);

	if (ret != 0)
		goto exit;

	const size_t nr_of_pebs = fa->fa_size / flash->erase_block_size;

	if (pnum >= nr_of_pebs || pnum < UBI_DEV_HDR_NR_OF_RES_PEBS) {
		LOG_ERR("PEB index %zu out of range", pnum);
		ret = -EINVAL;
		goto exit;
	}

	ret = flash_write_with_retry(fa, pnum * flash->erase_block_size, hdr, sizeof(*hdr));

	if (ret != 0) {
		LOG_ERR("EC header write failure for PEB %zu", pnum);
		goto exit;
	}

exit:
	if (fa)
		flash_area_close(fa);

	return ret;
}

int ubi_vid_hdr_read(const struct ubi_flash_desc *flash, const size_t pnum,
		     struct ubi_vid_hdr *vid_hdr, bool check)
{
	int ret = -EIO;

	if (!flash)
		return -EINVAL;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(flash->partition_id, &fa);

	if (ret != 0)
		return ret;

	const size_t nr_of_pebs = fa->fa_size / flash->erase_block_size;

	if (pnum >= nr_of_pebs || pnum < UBI_DEV_HDR_NR_OF_RES_PEBS) {
		LOG_ERR("PEB index %zu out of range", pnum);
		ret = -EINVAL;
		goto exit;
	}

	struct ubi_vid_hdr hdr = { 0 };
	ret = flash_area_read(fa, (pnum * flash->erase_block_size) + UBI_EC_HDR_SIZE, &hdr,
			      sizeof(hdr));

	if (ret != 0) {
		LOG_ERR("VID header flash read failure for PEB %zu", pnum);
		goto exit;
	}

	if (vid_hdr)
		*vid_hdr = hdr;

	if (check) {
		if (UBI_VID_HDR_MAGIC != hdr.magic ||
		    hdr.hdr_crc !=
			    crc32_ieee((const uint8_t *)&hdr, sizeof(hdr) - sizeof(hdr.hdr_crc))) {
			LOG_ERR("VID header corrupt on PEB %zu", pnum);
			ret = -EBADMSG;
			goto exit;
		}
	}

exit:
	if (fa)
		flash_area_close(fa);

	return ret;
}

int ubi_vid_hdr_write(const struct ubi_flash_desc *flash, const size_t pnum,
		      struct ubi_vid_hdr *vid_hdr)
{
	int ret = -EIO;

	if (!flash || !vid_hdr)
		return -EINVAL;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(flash->partition_id, &fa);

	if (ret != 0)
		goto exit;

	const size_t nr_of_pebs = fa->fa_size / flash->erase_block_size;

	if (pnum >= nr_of_pebs || pnum < UBI_DEV_HDR_NR_OF_RES_PEBS) {
		LOG_ERR("PEB index %zu out of range", pnum);
		ret = -EINVAL;
		goto exit;
	}

	ret = flash_write_with_retry(fa, (pnum * flash->erase_block_size) + UBI_EC_HDR_SIZE,
				     vid_hdr, sizeof(*vid_hdr));

	if (ret != 0) {
		LOG_ERR("VID header write failure for PEB %zu", pnum);
		goto exit;
	}

exit:
	if (fa)
		flash_area_close(fa);

	return ret;
}

int ubi_leb_data_write(const struct ubi_flash_desc *flash, const size_t pnum, const uint8_t *buf,
		       size_t len)
{
	int ret = -EIO;

	if (!flash || !buf || len == 0)
		return -EINVAL;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(flash->partition_id, &fa);

	if (ret != 0)
		goto exit;

	const size_t nr_of_pebs = fa->fa_size / flash->erase_block_size;

	if (pnum >= nr_of_pebs || pnum < UBI_DEV_HDR_NR_OF_RES_PEBS) {
		LOG_ERR("PEB index %zu out of range", pnum);
		ret = -EINVAL;
		goto exit;
	}

	if (len > (flash->erase_block_size - UBI_EC_HDR_SIZE - UBI_VID_HDR_SIZE)) {
		LOG_ERR("LEB data write length %zu exceeds capacity", len);
		ret = -ENOSPC;
		goto exit;
	}

	size_t offset = (pnum * flash->erase_block_size) + UBI_EC_HDR_SIZE + UBI_VID_HDR_SIZE;
	const size_t wbs = flash->write_block_size;

	if (len % wbs == 0) {
		ret = flash_write_with_retry(fa, offset, buf, len);

		if (ret != 0) {
			LOG_ERR("LEB data write failure for PEB %zu", pnum);
			goto exit;
		}
	} else {
		if (len < wbs) {
			uint8_t align_buf[WRITE_BLOCK_SIZE_ALIGNMENT] = { 0 };
			memcpy(align_buf, buf, len);

			ret = flash_write_with_retry(fa, offset, align_buf, wbs);

			if (ret != 0) {
				LOG_ERR("LEB data write failure for PEB %zu", pnum);
				goto exit;
			}
		} else {
			const size_t left_size = len % wbs;

			uint8_t align_buf[WRITE_BLOCK_SIZE_ALIGNMENT] = { 0 };
			memcpy(align_buf, &buf[len - left_size], left_size);

			ret = flash_write_with_retry(fa, offset, buf, len - left_size);

			if (ret != 0) {
				LOG_ERR("LEB data write failure for PEB %zu", pnum);
				goto exit;
			}

			ret = flash_write_with_retry(fa, offset + len - left_size, align_buf, wbs);

			if (ret != 0) {
				LOG_ERR("LEB data write tail failure for PEB %zu", pnum);
				goto exit;
			}
		}
	}

exit:
	if (fa)
		flash_area_close(fa);

	return ret;
}

int ubi_leb_data_read(const struct ubi_flash_desc *flash, const size_t pnum, size_t offset,
		      uint8_t *buf, size_t len)
{
	int ret = -EIO;

	if (!flash || !buf || len == 0)
		return -EINVAL;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(flash->partition_id, &fa);

	if (ret != 0)
		goto exit;

	const size_t nr_of_pebs = fa->fa_size / flash->erase_block_size;

	if (pnum >= nr_of_pebs || pnum < UBI_DEV_HDR_NR_OF_RES_PEBS) {
		LOG_ERR("PEB index %zu out of range", pnum);
		ret = -EINVAL;
		goto exit;
	}

	if ((offset + len) > (flash->erase_block_size - UBI_EC_HDR_SIZE - UBI_VID_HDR_SIZE)) {
		LOG_ERR("LEB data read offset+len exceeds capacity");
		ret = -ENOSPC;
		goto exit;
	}

	const size_t _offset =
		(pnum * flash->erase_block_size) + UBI_EC_HDR_SIZE + UBI_VID_HDR_SIZE + offset;

	ret = flash_area_read(fa, _offset, buf, len);

	if (ret != 0) {
		LOG_ERR("LEB data read failure for PEB %zu", pnum);
		goto exit;
	}

exit:
	if (fa)
		flash_area_close(fa);

	return ret;
}
