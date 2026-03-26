/**
 * \file    ubi.h
 *
 * \brief   Unsorted Block Images (UBI) interface.
 *
 * \author  Kamil Kielbasa
 * \version 0.9
 * \date    2026-03-26
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
 * \brief Memory technology device (MTD) descriptor.
 *
 * Describes the underlying flash partition used by UBI, including its
 * geometry (write and erase block sizes).
 */
struct ubi_mtd {
	uint8_t partition_id; /*!< Flash partition identifier (from FIXED_PARTITION_ID). */

	size_t write_block_size; /*!< Minimum write block size in bytes. */
	size_t erase_block_size; /*!< Erase block (sector) size in bytes. */
};

/**
 * \brief UBI device information.
 *
 * Snapshot of the current device state returned by ubi_device_get_info().
 */
struct ubi_device_info {
	size_t allocated_peb_count; /*!< PEBs allocated to volumes. */

	size_t free_peb_count; /*!< Free PEBs available for allocation. */
	size_t dirty_peb_count; /*!< Dirty PEBs awaiting erasure. */
	size_t bad_peb_count; /*!< Bad PEBs detected and retired. */

	size_t total_peb_count; /*!< Total usable data PEBs on the device. */
	size_t leb_size; /*!< Usable data size per LEB in bytes. */

	size_t volume_count; /*!< Number of volumes on the device. */
};

/**
 * \brief Types of UBI volumes.
 */
enum ubi_volume_type {
	UBI_VOLUME_TYPE_STATIC = 0, /*!< Static volume — LEB count fixed after creation. */
	UBI_VOLUME_TYPE_DYNAMIC = 1, /*!< Dynamic volume — can be resized at runtime. */
};

/**
 * \brief Volume configuration.
 *
 * Describes the parameters of a UBI volume, used during volume creation
 * and returned by ubi_volume_get_info().
 */
struct ubi_volume_config {
	char name[UBI_VOLUME_NAME_MAX_LEN]; /*!< Volume name. */
	enum ubi_volume_type type; /*!< Volume type. */
	size_t leb_count; /*!< Number of logical erase blocks. */
};

/** \} name ubi_structs */

/**
 * \defgroup ubi_device UBI Device Management
 * \brief Functions for device initialization, shutdown, statistics, and PEB reclamation.
 * \{
 */

/**
 * \brief Initialize a UBI device on the given flash partition.
 *
 * Scans the flash partition, formats it on first use, and builds the
 * in-memory PEB and volume tables. On success, *ubi points to the
 * allocated device handle; on failure, *ubi is set to NULL.
 *
 * \param[in] mtd 		Flash partition descriptor.
 * \param[out] ubi		Pointer to receive the UBI device handle.
 *
 * \retval 0       Success.
 * \retval -EINVAL  Invalid parameters.
 * \retval -ENOMEM  Allocation failure.
 * \retval -ENODEV  Flash device not ready.
 */
int ubi_device_init(const struct ubi_mtd *mtd, struct ubi_device **ubi);

/**
 * \brief Query the current state of a UBI device.
 *
 * Populates \p info with PEB counts, LEB geometry, and volume statistics.
 *
 * \param[in] ubi 		UBI device handle.
 * \param[out] info 		Device information output.
 *
 * \retval 0       Success.
 * \retval -EINVAL  Invalid parameters.
 */
int ubi_device_get_info(struct ubi_device *ubi, struct ubi_device_info *info);

/**
 * \brief Reclaim one dirty PEB by erasing it.
 *
 * Erases the highest-priority dirty PEB and moves it to the free pool.
 * Call this repeatedly to reclaim all dirty PEBs.
 *
 * \param[in] ubi 		UBI device handle.
 *
 * \retval 0       Success (or no dirty PEBs to reclaim).
 * \retval -EINVAL  Invalid parameter.
 * \retval -ENOMEM  Allocation failure during bad-block handling.
 */
int ubi_device_erase_peb(struct ubi_device *ubi);

/**
 * \brief Shut down a UBI device and release all resources.
 *
 * Frees all in-memory structures. The handle must not be used after this call.
 *
 * \param[in] ubi 		UBI device handle.
 *
 * \retval 0       Success.
 * \retval -EINVAL  Invalid parameter.
 */
int ubi_device_deinit(struct ubi_device *ubi);

#if defined(CONFIG_UBI_TEST_API_ENABLE)

/**
 * \brief Retrieve per-PEB erase counters (test API).
 *
 * Allocates an array of erase counters, one per data PEB. The caller
 * must free the array with k_free() when done.
 *
 * \param[in] ubi 		UBI device handle.
 * \param[out] peb_ec		Pointer to receive the erase-counter array.
 * \param[out] len		Number of entries in the array.
 *
 * \retval 0       Success.
 * \retval -EINVAL  Invalid parameters.
 * \retval -ENOMEM  Allocation failure.
 */
int ubi_device_get_peb_ec(struct ubi_device *ubi, size_t **peb_ec, size_t *len);

#endif /* CONFIG_UBI_TEST_API_ENABLE */

/** \} name ubi_device */

/**
 * \defgroup ubi_volumes UBI Volume Management
 * \brief Functions to create, resize, remove, and query volumes.
 * \{
 */

/**
 * \brief Create a new UBI volume.
 *
 * \param[in] ubi 		UBI device handle.
 * \param[in] vol_cfg 		Volume configuration (name, type, LEB count).
 * \param[out] vol_id 		Assigned volume identifier.
 *
 * \retval 0       Success.
 * \retval -EINVAL  Invalid parameters.
 * \retval -ENOMEM  No free PEBs for the requested LEB count.
 * \retval -EEXIST  A volume with the same name already exists.
 */
int ubi_volume_create(struct ubi_device *ubi, const struct ubi_volume_config *vol_cfg, int *vol_id);

/**
 * \brief Resize an existing UBI volume.
 *
 * \param[in] ubi 		UBI device handle.
 * \param[in] vol_id 		Volume identifier.
 * \param[in] vol_cfg 		New volume configuration (only leb_count is used).
 *
 * \retval 0       Success.
 * \retval -EINVAL  Invalid parameters or volume not found.
 * \retval -ENOMEM  Not enough free PEBs to grow.
 */
int ubi_volume_resize(struct ubi_device *ubi, int vol_id, const struct ubi_volume_config *vol_cfg);

/**
 * \brief Remove an existing UBI volume.
 *
 * \param[in] ubi 		UBI device handle.
 * \param[in] vol_id 		Volume identifier.
 *
 * \retval 0       Success.
 * \retval -EINVAL  Invalid parameters or volume not found.
 */
int ubi_volume_remove(struct ubi_device *ubi, int vol_id);

/**
 * \brief Query volume configuration and allocation.
 *
 * \param[in] ubi 		UBI device handle.
 * \param[in] vol_id 		Volume identifier.
 * \param[out] vol_cfg 		Returned volume configuration.
 * \param[out] alloc_lebs	Number of LEBs currently allocated.
 *
 * \retval 0       Success.
 * \retval -EINVAL  Invalid parameters or volume not found.
 */
int ubi_volume_get_info(struct ubi_device *ubi, int vol_id, struct ubi_volume_config *vol_cfg,
			size_t *alloc_lebs);

/** \} name ubi_volumes */

/**
 * \defgroup ubi_io UBI LEB Operations
 * \brief Functions to map, unmap, read, write, and query logical erase blocks.
 * \{
 */

/**
 * \brief Write data to a logical erase block (LEB).
 *
 * \param[in] ubi 		UBI device handle.
 * \param[in] vol_id 		Volume identifier.
 * \param[in] lnum 		Logical block number.
 * \param[in] buf 		Data buffer to write.
 * \param[in] len 		Number of bytes to write from \p buf.
 *
 * \retval 0       Success.
 * \retval -EINVAL  Invalid parameters.
 */
int ubi_leb_write(struct ubi_device *ubi, int vol_id, size_t lnum, const void *buf, size_t len);

/**
 * \brief Read data from a logical erase block (LEB).
 *
 * \param[in] ubi 		UBI device handle.
 * \param[in] vol_id 		Volume identifier.
 * \param[in] lnum 		Logical block number.
 * \param[in] offset 		Byte offset within the LEB to start reading.
 * \param[out] buf 		Buffer to receive the data.
 * \param[in] len		Number of bytes to read.
 *
 * \retval 0       Success.
 * \retval -EINVAL  Invalid parameters.
 */
int ubi_leb_read(struct ubi_device *ubi, int vol_id, size_t lnum, size_t offset, void *buf,
		 size_t len);

/**
 * \brief Map a logical erase block (LEB) to a physical block.
 *
 * \param[in] ubi 		UBI device handle.
 * \param[in] vol_id 		Volume identifier.
 * \param[in] lnum 		Logical block number.
 *
 * \retval 0       Success.
 * \retval -EINVAL  Invalid parameters.
 */
int ubi_leb_map(struct ubi_device *ubi, int vol_id, size_t lnum);

/**
 * \brief Unmap a logical erase block (LEB).
 *
 * \param[in] ubi 		UBI device handle.
 * \param[in] vol_id 		Volume identifier.
 * \param[in] lnum 		Logical block number.
 *
 * \retval 0       Success.
 * \retval -EINVAL  Invalid parameters.
 */
int ubi_leb_unmap(struct ubi_device *ubi, int vol_id, size_t lnum);

/**
 * \brief Check whether a logical erase block is mapped.
 *
 * \param[in] ubi 		UBI device handle.
 * \param[in] vol_id 		Volume identifier.
 * \param[in] lnum 		Logical block number.
 * \param[out] is_mapped 	Set to true if the LEB is mapped.
 *
 * \retval 0       Success.
 * \retval -EINVAL  Invalid parameters.
 */
int ubi_leb_is_mapped(struct ubi_device *ubi, int vol_id, size_t lnum, bool *is_mapped);

/**
 * \brief Get the data size of a mapped LEB.
 *
 * \param[in] ubi 		UBI device handle.
 * \param[in] vol_id 		Volume identifier.
 * \param[in] lnum 		Logical block number.
 * \param[out] size		Data size in bytes stored in the LEB.
 *
 * \retval 0       Success.
 * \retval -EINVAL  Invalid parameters.
 */
int ubi_leb_get_size(struct ubi_device *ubi, int vol_id, size_t lnum, size_t *size);

/** \} name ubi_io */

#endif /* UBI_H */
