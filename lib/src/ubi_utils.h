/**
 * \file    ubi_utils.h
 *
 * \brief   Unsorted Block Images (UBI) Utilities
 *
 * \author  Kamil Kielbasa
 * \version 0.4
 * \date    2025-09-24
 *
 * \copyright Copyright (c) 2025
 */

/* Include guard ------------------------------------------------------------------------------- */
#ifndef UBI_UTILS_H
#define UBI_UTILS_H

/* Include files ------------------------------------------------------------------------------- */
#include "ubi.h"

/* Zephyr header */
#include <zephyr/sys/util.h>

/* Standard library headers */
#include <stddef.h>
#include <stdint.h>

/* Defines ------------------------------------------------------------------------------------- */

/**
 * \def WRITE_BLOCK_SIZE_ALIGNMENT
 * \brief Required alignment for UBI header structures.
 */
#define WRITE_BLOCK_SIZE_ALIGNMENT (16)

/* UBI device header constants */
#define UBI_DEV_HDR_MAGIC (0x55424925)
#define UBI_DEV_HDR_SIZE (32)
#define UBI_DEV_HDR_VERSION (1)
#define UBI_DEV_HDR_NR_OF_RES_PEBS (2)
#define UBI_DEV_HDR_RES_PEB_0 (0)
#define UBI_DEV_HDR_RES_PEB_1 (1)

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

/* Types and type definitions ------------------------------------------------------------------ */

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
	uint32_t padding_2; /*!< Reserved */
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
	uint32_t lebs_count; /*!< Number of logical erase blocks */
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

/* Module interface function declarations ------------------------------------------------------ */

/**
 * \defgroup ubi_utils_device Device Utilities
 * \brief Functions for mounting and reading UBI device headers.
 * \{
 */

/**
 * \brief Check if a UBI device is mounted.
 *
 * \param[in] mtd        	Pointer to memory technology device.
 * \param[out] is_mounted 	Set to true if device is mounted.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_dev_is_mounted(const struct ubi_mtd *mtd, bool *is_mounted);

/**
 * \brief Mount a UBI device.
 *
 * \param[in] mtd 		Pointer to memory technology device.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_dev_mount(const struct ubi_mtd *mtd);

/**
 * \brief Read UBI device header.
 *
 * \param[in] mtd     		Pointer to memory technology device.
 * \param[out] dev_hdr 		Pointer to device header structure.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_dev_hdr_read(const struct ubi_mtd *mtd, struct ubi_dev_hdr *dev_hdr);

/** \} name ubi_utils_device */

/**
 * \defgroup ubi_utils_volume Volume Utilities
 * \brief Functions for reading, writing, and updating UBI volume headers.
 * \{
 */

/**
 * \brief Read a UBI volume header.
 *
 * \param[in] mtd     		Pointer to memory technology device.
 * \param[in] index   		Volume index.
 * \param[out] vol_hdr 		Pointer to volume header structure.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_vol_hdr_read(const struct ubi_mtd *mtd, const size_t index, struct ubi_vol_hdr *vol_hdr);

/**
 * \brief Append a new UBI volume header.
 *
 * \param[in] mtd     		Pointer to memory technology device.
 * \param[in] dev_hdr 		Pointer to device header.
 * \param[in] vol_hdr 		Pointer to volume header to append.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_vol_hdr_append(const struct ubi_mtd *mtd, const struct ubi_dev_hdr *dev_hdr,
		       const struct ubi_vol_hdr *vol_hdr);

/**
 * \brief Remove an existing UBI volume header.
 *
 * \param[in] mtd     		Pointer to memory technology device.
 * \param[in] dev_hdr 		Pointer to device header.
 * \param index   		Volume index to remove.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_vol_hdr_remove(const struct ubi_mtd *mtd, const struct ubi_dev_hdr *dev_hdr,
		       const size_t index);

/**
 * \brief Update an existing UBI volume header.
 *
 * \param[in] mtd     		Pointer to memory technology device.
 * \param[in] dev_hdr 		Pointer to device header.
 * \param index   		Volume index to update.
 * \param[in] vol_hdr 		Pointer to new volume header values.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_vol_hdr_update(const struct ubi_mtd *mtd, const struct ubi_dev_hdr *dev_hdr,
		       const size_t index, const struct ubi_vol_hdr *vol_hdr);

/** \} name ubi_utils_volume */

/**
 * \defgroup ubi_utils_ec Erase Counter Utilities
 * \brief Functions for reading and writing UBI erase counter headers.
 * \{
 */

/**
 * \brief Read an erase counter (EC) header.
 *
 * \param[in] mtd     		Pointer to memory technology device.
 * \param pnum    		Physical eraseblock number.
 * \param[out] ec_hdr 		Pointer to EC header.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_ec_hdr_read(const struct ubi_mtd *mtd, const size_t pnum, struct ubi_ec_hdr *ec_hdr);

/**
 * \brief Write an erase counter (EC) header.
 *
 * \param[in] mtd     		Pointer to memory technology device.
 * \param pnum    		Physical eraseblock number.
 * \param[in] ec_hdr  		Pointer to EC header.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_ec_hdr_write(const struct ubi_mtd *mtd, const size_t pnum, const struct ubi_ec_hdr *ec_hdr);

/** \} name ubi_utils_ec */

/**
 * \defgroup ubi_utils_vid Volume Identifier Utilities
 * \brief Functions for reading and writing UBI volume identifier headers.
 * \{
 */

/**
 * \brief Read a volume identifier (VID) header.
 *
 * \param[in] mtd     		Pointer to memory technology device.
 * \param pnum    		Physical eraseblock number.
 * \param[out] vid_hdr 		Pointer to VID header.
 * \param check   		Validate header CRC if true.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_vid_hdr_read(const struct ubi_mtd *mtd, const size_t pnum, struct ubi_vid_hdr *vid_hdr,
		     bool check);

/**
 * \brief Write a volume identifier (VID) header.
 *
 * \param[in] mtd     		Pointer to memory technology device.
 * \param pnum    		Physical eraseblock number.
 * \param[in] vid_hdr 		Pointer to VID header.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_vid_hdr_write(const struct ubi_mtd *mtd, const size_t pnum, struct ubi_vid_hdr *vid_hdr);

/** \} name ubi_utils_vid */

/**
 * \defgroup ubi_utils_data LEB Data Utilities
 * \brief Functions for reading and writing logical erase block data.
 * \{
 */

/**
 * \brief Write data to a logical erase block (LEB).
 *
 * \param[in] mtd  		Pointer to memory technology device.
 * \param pnum 			Physical eraseblock number.
 * \param[in] buf  		Data buffer.
 * \param len  			Length of data in bytes.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_leb_data_write(const struct ubi_mtd *mtd, const size_t pnum, const uint8_t *buf,
		       size_t len);

/**
 * \brief Read data from a logical erase block (LEB).
 *
 * \param[in] mtd  		Pointer to memory technology device.
 * \param pnum 			Physical eraseblock number.
 * \param offset 		Offset in bytes within the block.
 * \param[out] buf 		Output buffer.
 * \param len  			Number of bytes to read.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_leb_data_read(const struct ubi_mtd *mtd, const size_t pnum, size_t offset, uint8_t *buf,
		      size_t len);

/** \} name ubi_utils_data */

#endif /* UBI_UTILS_H */
