/**
 * \file    ubi.h
 *
 * \brief   Unsorted Block Images (UBI) interface.
 *
 * \author  Kamil Kielbasa
 * \version 0.3
 * \date    2025-09-21
 *
 * \copyright Copyright (c) 2025
 */

/* Include guard ------------------------------------------------------------------------------- */
#ifndef UBI_H
#define UBI_H

/* Include files ------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Defines ------------------------------------------------------------------------------------- */

/**
 * \def UBI_VOLUME_NAME_MAX_LEN
 * \brief Maximum length of a UBI volume name including the null terminator.
 */
#define UBI_VOLUME_NAME_MAX_LEN (16)

/* Forward declarations ------------------------------------------------------------------------ */

/**
 * \brief Forward declaration of the UBI device structure.
 *
 * This opaque structure represents an instance of a UBI device.
 */
struct ubi_device;

/* Types and type definitions ------------------------------------------------------------------ */

/**
 * \defgroup ubi_structs UBI Data Structures
 * \{
 */

/**
 * \brief Memory technology device (MTD) for UBI.
 */
struct ubi_mtd {
	uint8_t partition_id; /*!< Partition identifier from FIXED_PARTITION_ID macro. */

	size_t write_block_size; /*!< Write block size in bytes. */
	size_t erase_block_size; /*!< Erase block size in bytes. */
};

/**
 * \brief Device informations.
 */
struct ubi_device_info {
	size_t allocated_leb_count; /*!< Number of allocated by volumes physical erase blocks. */

	size_t free_leb_count; /*!< Number of free physical erase blocks. */
	size_t dirty_leb_count; /*!< Number of dirty physical erase blocks. */
	size_t bad_leb_count; /*!< Number of bad physical erase blocks. */

	size_t leb_total_count; /*!< Total number of logical erase blocks. */
	size_t leb_size; /*!< Size of each logical erase block in bytes. */

	size_t volumes_count; /*!< Number of created volumes. */
};

/**
 * \brief Types of UBI volumes.
 */
enum ubi_volume_type {
	UBI_VOLUME_TYPE_STATIC = 0, /*!< Static volume type, contents fixed. */
	UBI_VOLUME_TYPE_DYNAMIC = 1, /*!< Dynamic volume type, contents can change. */
};

/**
 * \brief Volume configuration.
 */
struct ubi_volume_config {
	unsigned char name[UBI_VOLUME_NAME_MAX_LEN]; /*!< Volume name. */
	enum ubi_volume_type type; /*!< Volume type. */
	size_t leb_count; /*!< Number of logical erase blocks. */
};

/** \} name ubi_structs */

/* Module interface variables and constants ---------------------------------------------------- */
/* Extern variables and constant declarations -------------------------------------------------- */
/* Module interface function declarations ------------------------------------------------------ */

/**
 * \defgroup ubi_device UBI Device Management
 * \brief Functions to initialize, get statistics, erases and shut down the UBI
 * device.
 * \{
 */

/**
 * \brief Initialize the UBI subsystem with a given memory device.
 *
 * \param[in] mtd 		Pointer to memory technology device.
 * \param[out] ubi		Pointer to UBI device instance.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_device_init(const struct ubi_mtd *mtd, struct ubi_device **ubi);

/**
 * \brief Deinitialize the UBI subsystem and release resources.
 *
 * \param[in] ubi 		Pointer to UBI device instance.
 * \param[out] info 		Pointer to UBI device statistics.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_device_get_info(struct ubi_device *ubi, struct ubi_device_info *info);

/**
 * \brief Trigger erase operation on a physical erase block.
 *
 * \param[in] ubi 		Pointer to UBI device instance.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_device_erase_peb(struct ubi_device *ubi);

/**
 * \brief Deinitialize the UBI subsystem and release resources.
 *
 * \param[in] ubi 		Pointer to UBI device instance.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_device_deinit(struct ubi_device *ubi);

#if defined(CONFIG_UBI_TEST_API_ENABLE)

/**
 * \brief Get UBI erase counter header values.
 *
 * \param[in] ubi 		Pointer to UBI device instance.
 * \param[out] peb_ec		Address pointer for PEBs erase counters.
 * \param[out] len		Number of PEBs.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_device_get_peb_ec(struct ubi_device *ubi, size_t **peb_ec, size_t *len);

#endif /* CONFIG_UBI_TEST_API_ENABLE */

/** \} name ubi_device */

/**
 * \defgroup ubi_volumes UBI Volume Management
 * \brief Functions to create, resize, remove and get statistics for volumes.
 * \{
 */

/**
 * \brief Create a new UBI volume.
 *
 * \param[in] ubi 		Pointer to UBI device.
 * \param[in] vol_cfg 		Pointer to volume instance.
 * \param[out] vol_id 		Assigned volume ID (output).
 *
 * \return 0 on success, or negative error code.
 */
int ubi_volume_create(struct ubi_device *ubi, const struct ubi_volume_config *vol_cfg, int *vol_id);

/**
 * \brief Resize an existing UBI volume.
 *
 * \param[in] ubi 		Pointer to UBI device.
 * \param vol_id 		Volume ID to resize.
 * \param[in] vol_cfg 		Pointer to new volume parameters.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_volume_resize(struct ubi_device *ubi, int vol_id, const struct ubi_volume_config *vol_cfg);

/**
 * \brief Remove an existing UBI volume.
 *
 * \param[in] ubi 		Pointer to UBI device.
 * \param vol_id 		Volume ID to remove.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_volume_remove(struct ubi_device *ubi, int vol_id);

/**
 * \brief Get information about a UBI volume.
 *
 * \param[in] ubi 		Pointer to UBI device.
 * \param vol_id 		Volume ID.
 * \param[in,out] vol_cfg 	Output volume information.
 * \param[in,out] alloc_lebs	Number of allocated LEBs.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_volume_get_info(struct ubi_device *ubi, int vol_id, struct ubi_volume_config *vol_cfg,
			size_t *alloc_lebs);

/** \} name ubi_volumes */

/**
 * \defgroup ubi_io UBI LEBs Management
 * \brief Functions to map/unmap, check if mapped, write/read and get size of
 * logical erase blocks.
 * \{
 */

/**
 * \brief Write data to a logical erase block (LEB).
 *
 * \param[in] ubi 		Pointer to UBI device.
 * \param vol_id 		Volume ID.
 * \param lnum 			Logical block number.
 * \param[in] buf 		Buffer containing data to write.
 * \param len 			Size of the \p buf in bytes.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_leb_write(struct ubi_device *ubi, int vol_id, size_t lnum, const void *buf, size_t len);

/**
 * \brief Read data from a logical erase block (LEB).
 *
 * \param[in] ubi 		Pointer to UBI device.
 * \param vol_id 		Volume ID.
 * \param lnum 			Logical block number.
 * \param offset 		Offset in the block to read from.
 * \param[out] buf 		Output buffer.
 * \param size			Size of the \p buf in bytes.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_leb_read(struct ubi_device *ubi, int vol_id, size_t lnum, size_t offset, void *buf,
		 size_t size);

/**
 * \brief Map a logical erase block (LEB) to a physical block.
 *
 * \param[in] ubi 		Pointer to UBI device.
 * \param vol_id 		Volume ID.
 * \param lnum 			Logical block number.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_leb_map(struct ubi_device *ubi, int vol_id, size_t lnum);

/**
 * \brief Unmap a logical erase block (LEB).
 *
 * \param[in] ubi 		Pointer to UBI device.
 * \param vol_id 		Volume ID.
 * \param lnum 			Logical block number.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_leb_unmap(struct ubi_device *ubi, int vol_id, size_t lnum);

/**
 * \brief Check if a logical erase block (LEB) is mapped.
 *
 * \param[in] ubi 		Pointer to UBI device.
 * \param vol_id 		Volume ID.
 * \param lnum 			Logical block number.
 * \param[out] is_mapped 	Output flag set to true if mapped.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_leb_is_mapped(struct ubi_device *ubi, int vol_id, size_t lnum, bool *is_mapped);

/**
 * \brief Get size of mapped LEB.
 *
 * \param[in] ubi 		Pointer to UBI device.
 * \param vol_id 		Volume ID.
 * \param lnum 			Logical block number.
 * \param[out] size		Size of mapped LEB.
 *
 * \return 0 on success, or negative error code.
 */
int ubi_leb_get_size(struct ubi_device *ubi, int vol_id, size_t lnum, size_t *size);

/** \} name ubi_io */

#endif /* UBI_H */
