/**
 * \file    ubi_test_raw_flash.h
 * \brief   Shared helpers for direct flash manipulation in tests.
 *
 * \copyright Copyright (c) 2026
 */

#ifndef UBI_TEST_RAW_FLASH_H
#define UBI_TEST_RAW_FLASH_H

#include <zephyr/ztest.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

struct raw_ec_hdr {
	uint32_t magic;
	uint8_t version;
	uint8_t padding[3];
	uint32_t ec;
	uint32_t hdr_crc;
};

struct raw_vid_hdr {
	uint32_t magic;
	uint8_t version;
	uint8_t padding[3];
	uint32_t lnum;
	uint32_t vol_id;
	uint64_t sqnum;
	uint32_t data_size;
	uint32_t hdr_crc;
};

#define RAW_EC_HDR_MAGIC  (0x55424923U)
#define RAW_VID_HDR_MAGIC (0x55424921U)
#define RAW_EC_HDR_SIZE   (16U)
#define RAW_VID_HDR_SIZE  (32U)

static inline void ubi_test_raw_write_ec_hdr(const struct flash_area *fa, size_t peb_offset,
					     uint32_t ec)
{
	struct raw_ec_hdr hdr = { 0 };
	hdr.magic = RAW_EC_HDR_MAGIC;
	hdr.version = 1;
	hdr.ec = ec;
	hdr.hdr_crc = crc32_ieee((const uint8_t *)&hdr, sizeof(hdr) - sizeof(hdr.hdr_crc));

	zassert_ok(flash_area_write(fa, peb_offset, &hdr, sizeof(hdr)));
}

static inline void ubi_test_raw_write_vid_hdr(const struct flash_area *fa, size_t peb_offset,
					      size_t ec_hdr_size, uint32_t lnum, uint32_t vol_id,
					      uint64_t sqnum, uint32_t data_size)
{
	struct raw_vid_hdr hdr = { 0 };
	hdr.magic = RAW_VID_HDR_MAGIC;
	hdr.version = 1;
	hdr.lnum = lnum;
	hdr.vol_id = vol_id;
	hdr.sqnum = sqnum;
	hdr.data_size = data_size;
	hdr.hdr_crc = crc32_ieee((const uint8_t *)&hdr, sizeof(hdr) - sizeof(hdr.hdr_crc));

	zassert_ok(flash_area_write(fa, peb_offset + ec_hdr_size, &hdr, sizeof(hdr)));
}

#endif /* UBI_TEST_RAW_FLASH_H */
