/**
 * \file    ubi_io.h
 *
 * \brief   Unsorted Block Images (UBI) flash I/O operations.
 *
 * \author  Kamil Kielbasa
 *
 * \copyright Copyright (c) 2025
 */

/* Include guard -------------------------------------------------------------------------------- */

#ifndef UBI_IO_H
#define UBI_IO_H

/* Include files -------------------------------------------------------------------------------- */

/* Public headers: */
#include "ubi.h"

/* Zephyr headers: */
#include <zephyr/sys/util.h>

/* Standard library headers: */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Defines -------------------------------------------------------------------------------------- */

/**
 * \def WRITE_BLOCK_SIZE_ALIGNMENT
 * \brief Required alignment for UBI header structures.
 */
#define WRITE_BLOCK_SIZE_ALIGNMENT (16)

/* UBI device header constants */
#define UBI_DEV_HDR_MAGIC (0x55424925)
#define UBI_DEV_HDR_SIZE (32)
#define UBI_DEV_HDR_VERSION (1)
#define UBI_DEV_HDR_NR_OF_RES_PEBS (CONFIG_UBI_DEV_HDR_NR_OF_RES_PEBS)

/* UBI volume header constants */
#define UBI_VOL_HDR_MAGIC (0x55424926)
#define UBI_VOL_HDR_SIZE (48)
#define UBI_VOL_HDR_VERSION (1)

/* UBI erase counter header constants */
#define UBI_EC_HDR_MAGIC (0x55424923)
#define UBI_EC_HDR_SIZE (16)
#define UBI_EC_HDR_VERSION (1)

/* UBI volume identifier header constants */
#define UBI_VID_HDR_MAGIC (0x55424921)
#define UBI_VID_HDR_SIZE (32)
#define UBI_VID_HDR_VERSION (1)

/* Types and type definitions ------------------------------------------------------------------- */

/**
 * \brief UBI device header structure.
 */
struct ubi_dev_hdr {
	uint32_t magic; /*!< Magic number */
	uint8_t version; /*!< Header version */
	uint8_t padding_1[3]; /*!< Reserved */
	uint32_t offset; /*!< Offset of first volume header */
	uint32_t size; /*!< Device size */
	uint32_t revision; /*!< Revision number */
	uint32_t vol_count; /*!< Number of volumes */
	uint32_t vol_id_watermark; /*!< Monotonic volume ID counter (never reused) */
	uint32_t hdr_crc; /*!< CRC32 of header */
};
BUILD_ASSERT(sizeof(struct ubi_dev_hdr) == UBI_DEV_HDR_SIZE);
BUILD_ASSERT(sizeof(struct ubi_dev_hdr) % WRITE_BLOCK_SIZE_ALIGNMENT == 0);

/**
 * \brief UBI volume header structure.
 */
struct ubi_vol_hdr {
	uint32_t magic; /*!< Magic number */
	uint8_t version; /*!< Header version */
	uint8_t vol_type; /*!< Volume type */
	uint8_t padding_1[2]; /*!< Reserved */
	uint32_t vol_id; /*!< Volume ID */
	uint32_t leb_count; /*!< Number of logical erase blocks */
	uint32_t padding_2[3]; /*!< Reserved */
	uint8_t name[UBI_VOLUME_NAME_MAX_LEN]; /*!< Volume name */
	uint32_t hdr_crc; /*!< CRC32 of header */
};
BUILD_ASSERT(sizeof(struct ubi_vol_hdr) == UBI_VOL_HDR_SIZE);
BUILD_ASSERT(sizeof(struct ubi_vol_hdr) % WRITE_BLOCK_SIZE_ALIGNMENT == 0);

/**
 * \brief UBI erase counter (EC) header structure.
 */
struct ubi_ec_hdr {
	uint32_t magic; /*!< Magic number */
	uint8_t version; /*!< Header version */
	uint8_t padding[3]; /*!< Reserved */
	uint32_t ec; /*!< Erase counter */
	uint32_t hdr_crc; /*!< CRC32 of header */
};
BUILD_ASSERT(sizeof(struct ubi_ec_hdr) == UBI_EC_HDR_SIZE);
BUILD_ASSERT(sizeof(struct ubi_ec_hdr) % WRITE_BLOCK_SIZE_ALIGNMENT == 0);

/**
 * \brief UBI volume identifier (VID) header structure.
 */
struct ubi_vid_hdr {
	uint32_t magic; /*!< Magic number */
	uint8_t version; /*!< Header version */
	uint8_t padding[3]; /*!< Reserved */
	uint32_t lnum; /*!< Logical block number */
	uint32_t vol_id; /*!< Volume ID */
	uint64_t sqnum; /*!< Sequence number */
	uint32_t data_size; /*!< Data size in bytes */
	uint32_t hdr_crc; /*!< CRC32 of header */
};
BUILD_ASSERT(sizeof(struct ubi_vid_hdr) == UBI_VID_HDR_SIZE);
BUILD_ASSERT(sizeof(struct ubi_vid_hdr) % WRITE_BLOCK_SIZE_ALIGNMENT == 0);

/* Module interface function declarations ------------------------------------------------------- */

/**
 * \defgroup ubi_io_device Device Utilities
 * \brief Functions for mounting and reading UBI device headers.
 * \{
 */

/**
 * \brief Check if a UBI device is mounted.
 *
 * \param[in] flash        	Flash partition descriptor.
 * \param[out] is_mounted 	Set to true if device is mounted.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_dev_is_mounted(const struct ubi_flash_desc *flash, bool *is_mounted);

/**
 * \brief Mount a UBI device.
 *
 * \param[in] flash 		Flash partition descriptor.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_dev_mount(const struct ubi_flash_desc *flash);

/**
 * \brief Read UBI device header.
 *
 * \param[in] flash     		Flash partition descriptor.
 * \param[out] dev_hdr 		Pointer to device header structure.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_dev_hdr_read(const struct ubi_flash_desc *flash, struct ubi_dev_hdr *dev_hdr);

/** \} name ubi_io_device */

/**
 * \defgroup ubi_io_volume Volume Utilities
 * \brief Functions for reading, writing, and updating UBI volume headers.
 * \{
 */

/**
 * \brief Read a UBI volume header.
 *
 * \param[in] flash     		Flash partition descriptor.
 * \param[in] index   		Volume index.
 * \param[out] vol_hdr 		Pointer to volume header structure.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_vol_hdr_read(const struct ubi_flash_desc *flash, const size_t index,
		     struct ubi_vol_hdr *vol_hdr);

/**
 * \brief Append a new UBI volume header.
 *
 * \param[in] flash     		Flash partition descriptor.
 * \param[in] dev_hdr 		Pointer to device header.
 * \param[in] vol_hdr 		Pointer to volume header to append.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_vol_hdr_append(const struct ubi_flash_desc *flash, const struct ubi_dev_hdr *dev_hdr,
		       const struct ubi_vol_hdr *vol_hdr);

/**
 * \brief Remove an existing UBI volume header.
 *
 * \param[in] flash     		Flash partition descriptor.
 * \param[in] dev_hdr 		Pointer to device header.
 * \param vol_id  		Volume identifier to remove.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_vol_hdr_remove(const struct ubi_flash_desc *flash, const struct ubi_dev_hdr *dev_hdr,
		       const uint32_t vol_id);

/**
 * \brief Update an existing UBI volume header.
 *
 * Reads the existing header for \p vol_id from flash, sets its
 * \c leb_count to \p new_leb_count, recomputes the CRC, and commits.
 *
 * \param[in] flash     		Flash partition descriptor.
 * \param[in] dev_hdr 		Pointer to device header.
 * \param vol_id  		Volume identifier to update.
 * \param new_leb_count		New LEB count value.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_vol_hdr_update(const struct ubi_flash_desc *flash, const struct ubi_dev_hdr *dev_hdr,
		       uint32_t vol_id, size_t new_leb_count);

/** \} name ubi_io_volume */

/**
 * \defgroup ubi_io_ec Erase Counter Utilities
 * \brief Functions for reading and writing UBI erase counter headers.
 * \{
 */

/**
 * \brief Read an erase counter (EC) header.
 *
 * \param[in] flash     		Flash partition descriptor.
 * \param pnum    		Physical eraseblock number.
 * \param[out] ec_hdr 		Pointer to EC header.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_ec_hdr_read(const struct ubi_flash_desc *flash, const size_t pnum,
		    struct ubi_ec_hdr *ec_hdr);

/**
 * \brief Write an erase counter (EC) header.
 *
 * \param[in] flash     		Flash partition descriptor.
 * \param pnum    		Physical eraseblock number.
 * \param[in] ec_hdr  		Pointer to EC header.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_ec_hdr_write(const struct ubi_flash_desc *flash, const size_t pnum,
		     const struct ubi_ec_hdr *ec_hdr);

/** \} name ubi_io_ec */

/**
 * \defgroup ubi_io_vid Volume Identifier Utilities
 * \brief Functions for reading and writing UBI volume identifier headers.
 * \{
 */

/**
 * \brief Read a volume identifier (VID) header.
 *
 * \param[in] flash     		Flash partition descriptor.
 * \param pnum    		Physical eraseblock number.
 * \param[out] vid_hdr 		Pointer to VID header.
 * \param check   		Validate header CRC if true.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_vid_hdr_read(const struct ubi_flash_desc *flash, const size_t pnum,
		     struct ubi_vid_hdr *vid_hdr, bool check);

/**
 * \brief Write a volume identifier (VID) header.
 *
 * \param[in] flash     		Flash partition descriptor.
 * \param pnum    		Physical eraseblock number.
 * \param[in] vid_hdr 		Pointer to VID header.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_vid_hdr_write(const struct ubi_flash_desc *flash, const size_t pnum,
		      struct ubi_vid_hdr *vid_hdr);

/** \} name ubi_io_vid */

/**
 * \defgroup ubi_io_data LEB Data Utilities
 * \brief Functions for reading and writing logical erase block data.
 * \{
 */

/**
 * \brief Write data to a logical erase block (LEB).
 *
 * \param[in] flash  		Flash partition descriptor.
 * \param pnum 			Physical eraseblock number.
 * \param[in] buf  		Data buffer.
 * \param len  			Length of data in bytes.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_leb_data_write(const struct ubi_flash_desc *flash, const size_t pnum, const uint8_t *buf,
		       size_t len);

/**
 * \brief Read data from a logical erase block (LEB).
 *
 * \param[in] flash  		Flash partition descriptor.
 * \param pnum 			Physical eraseblock number.
 * \param offset 		Offset in bytes within the block.
 * \param[out] buf 		Output buffer.
 * \param len  			Number of bytes to read.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_leb_data_read(const struct ubi_flash_desc *flash, const size_t pnum, size_t offset,
		      uint8_t *buf, size_t len);

/** \} name ubi_io_data */

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
/**
 * \brief Check and consume flash erase fault injection counter.
 *
 * \retval true  Erase should be faulted.
 * \retval false Erase proceeds normally.
 */
bool ubi_test_flash_erase_check_fail(void);

/**
 * \brief Check and consume flash write fault injection counter.
 *
 * \retval true  Write should be faulted.
 * \retval false Write proceeds normally.
 */
bool ubi_test_flash_write_check_fail(void);
#endif /* CONFIG_UBI_TEST_FAULT_INJECTION */

/* Inline helpers for on-flash header validation ------------------------------------------------ */

/**
 * \brief Validate a volume header beyond CRC -- check field semantics.
 */
static inline bool ubi_vol_hdr_semantically_valid(const struct ubi_vol_hdr *hdr)
{
	if (hdr->magic != UBI_VOL_HDR_MAGIC)
		return false;
	if (hdr->version != UBI_VOL_HDR_VERSION)
		return false;
	if (hdr->vol_type != UBI_VOLUME_TYPE_STATIC && hdr->vol_type != UBI_VOLUME_TYPE_DYNAMIC)
		return false;
	if (hdr->leb_count == 0)
		return false;
	if (strnlen((const char *)hdr->name, UBI_VOLUME_NAME_MAX_LEN) == 0)
		return false;
	return true;
}

/**
 * \brief Safely copy a volume name from a RAM source into an on-flash header field.
 */
static inline void ubi_copy_name_to_hdr(uint8_t *dst, const char *src)
{
	memset(dst, 0, UBI_VOLUME_NAME_MAX_LEN);
	const size_t len = strnlen(src, UBI_VOLUME_NAME_MAX_LEN - 1);
	memcpy(dst, src, len);
}

/**
 * \brief Safely copy a volume name from an on-flash header field into a RAM config.
 */
static inline void ubi_copy_name_from_hdr(char *dst, const uint8_t *src)
{
	memset(dst, 0, UBI_VOLUME_NAME_MAX_LEN);
	memcpy(dst, src, UBI_VOLUME_NAME_MAX_LEN - 1);
	dst[UBI_VOLUME_NAME_MAX_LEN - 1] = '\0';
}

#endif /* UBI_IO_H */
