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
	bool read_only_degraded; /*!< True when reserved PEB redundancy is lost
	                              (metadata is read-only; volume create/resize/remove
	                              will fail with -EROFS). */

	size_t reserved_peb_count; /*!< Sum of leb_count across all volumes
	                                (PEBs reserved by volume configuration). */

	size_t free_peb_count; /*!< Free PEBs available for allocation. */
	size_t dirty_peb_count; /*!< Dirty PEBs awaiting erasure. */
	size_t bad_peb_count; /*!< Bad PEBs detected and retired. */

	size_t ec_avg; /*!< Average erase counter across all data PEBs. */

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
 *
 * \note **Thread safety**: all functions in this group use a per-device mutex.
 *       They must NOT be called from ISR context.
 * \{
 */

/**
 * \brief Initialize a UBI device on the given flash partition.
 *
 * Scans the flash partition, formats it on first use, and builds the
 * in-memory PEB and volume tables. On success, *ubi points to the
 * allocated device handle; on failure, *ubi is set to NULL.
 *
 * If the reserved PEB area has lost redundancy (one copy damaged),
 * initialization still succeeds but the device enters degraded
 * read-only mode — check \c ubi_device_info.read_only_degraded.
 *
 * \pre \p mtd fields must be non-zero and geometrically consistent:
 *      write_block_size > 0, erase_block_size > 0,
 *      erase_block_size % write_block_size == 0,
 *      partition size % erase_block_size == 0,
 *      partition must hold at least UBI_DEV_HDR_NR_OF_RES_PEBS + 1 PEBs.
 *
 * \param[in] mtd 		Flash partition descriptor (caller retains ownership).
 * \param[out] ubi		Pointer to receive the UBI device handle.
 *                              Set to NULL on failure.
 *
 * \retval 0        Success (device may be degraded — query info to check).
 * \retval -EINVAL  NULL pointer or invalid flash geometry.
 * \retval -ENOMEM  Heap allocation failure.
 * \retval -ENODEV  Flash device not ready.
 * \retval -EIO     Unrecoverable flash I/O error.
 */
int ubi_device_init(const struct ubi_mtd *mtd, struct ubi_device **ubi);

/**
 * \brief Query the current state of a UBI device.
 *
 * Populates \p info with PEB counts, LEB geometry, volume statistics,
 * and the degraded-mode flag. This is a lightweight operation that reads
 * only cached in-memory state (no flash I/O).
 *
 * \param[in] ubi 		UBI device handle (must be initialized).
 * \param[out] info 		Device information output (zeroed, then populated).
 *
 * \retval 0       Success.
 * \retval -EINVAL  NULL pointer.
 */
int ubi_device_get_info(struct ubi_device *ubi, struct ubi_device_info *info);

/**
 * \brief Reclaim one dirty PEB by erasing it.
 *
 * Erases the highest-priority dirty PEB and moves it to the free pool.
 * Also attempts to recover bad PEBs via erase torture (up to
 * CONFIG_UBI_BAD_PEB_TORTURE_CYCLES per call). If no dirty PEBs exist,
 * returns 0 immediately.
 *
 * \param[in] ubi 		UBI device handle.
 *
 * \retval 0       Success (or no dirty PEBs to reclaim).
 * \retval -EINVAL  NULL pointer.
 * \retval -ENOMEM  Allocation failure during bad-block handling.
 * \retval -EIO     Flash erase/write failure.
 */
int ubi_device_erase_peb(struct ubi_device *ubi);

/**
 * \brief Shut down a UBI device and release all resources.
 *
 * Frees all in-memory structures. The handle must not be used after this call.
 *
 * \param[in] ubi 		UBI device handle (may be partially initialized).
 *
 * \retval 0       Success.
 * \retval -EINVAL  NULL pointer.
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
 *
 * \note **Thread safety**: all functions in this group use a per-device mutex.
 *       They must NOT be called from ISR context.
 *
 * \note Volume create, resize, and remove modify on-flash metadata in the
 *       reserved PEB area. They will fail with \c -EROFS if the device is
 *       in degraded read-only mode (see \c ubi_device_info.read_only_degraded).
 * \{
 */

/**
 * \brief Create a new UBI volume.
 *
 * If a volume with the same name already exists and has an identical
 * configuration (type and leb_count), the call succeeds and returns the
 * existing volume's identifier (idempotent create). If the name matches
 * but the configuration differs, -EEXIST is returned.
 *
 * \pre \p vol_cfg->name must be NUL-terminated within UBI_VOLUME_NAME_MAX_LEN
 *      bytes and non-empty.
 * \pre \p vol_cfg->type must be a valid \c ubi_volume_type enumerator.
 * \pre \p vol_cfg->leb_count must be > 0.
 *
 * \param[in] ubi 		UBI device handle (must be initialized).
 * \param[in] vol_cfg 		Volume configuration (name, type, LEB count).
 * \param[out] vol_id 		Assigned volume identifier.
 *
 * \retval 0       Success (including idempotent duplicate).
 * \retval -EINVAL  NULL pointer or invalid volume name.
 * \retval -ENOSPC  Not enough free PEBs for the requested LEB count.
 * \retval -EEXIST  A volume with the same name but different configuration exists.
 * \retval -EROFS   Device is in degraded read-only mode.
 * \retval -ENOMEM  Heap allocation failure.
 */
int ubi_volume_create(struct ubi_device *ubi, const struct ubi_volume_config *vol_cfg, int *vol_id);

/**
 * \brief Resize an existing UBI volume.
 *
 * Only dynamic volumes may be resized. The new \c leb_count must differ
 * from the current one. When growing, the additional PEBs are reserved
 * but not mapped until written. When shrinking, mapped LEBs beyond the
 * new count are unmapped and their PEBs reclaimed to the dirty pool.
 *
 * \param[in] ubi 		UBI device handle.
 * \param[in] vol_id 		Volume identifier.
 * \param[in] vol_cfg 		New volume configuration (only leb_count is used).
 *
 * \retval 0        Success.
 * \retval -EINVAL   NULL pointer or volume not found.
 * \retval -ENOENT   Volume with given vol_id does not exist.
 * \retval -ECANCELED Static volume, or leb_count unchanged.
 * \retval -ENOSPC   Not enough free PEBs to grow.
 * \retval -EROFS    Device is in degraded read-only mode.
 */
int ubi_volume_resize(struct ubi_device *ubi, int vol_id, const struct ubi_volume_config *vol_cfg);

/**
 * \brief Remove an existing UBI volume.
 *
 * Removes the volume's on-flash header and reclaims all mapped PEBs
 * to the dirty pool.
 *
 * \param[in] ubi 		UBI device handle.
 * \param[in] vol_id 		Volume identifier.
 *
 * \retval 0       Success.
 * \retval -EINVAL  NULL pointer.
 * \retval -ENOENT  Volume with given vol_id does not exist.
 * \retval -EROFS   Device is in degraded read-only mode.
 */
int ubi_volume_remove(struct ubi_device *ubi, int vol_id);

/**
 * \brief Query volume configuration and allocation.
 *
 * \param[in] ubi 		UBI device handle.
 * \param[in] vol_id 		Volume identifier.
 * \param[out] vol_cfg 		Returned volume configuration.
 * \param[out] alloc_lebs	Number of LEBs currently mapped (with data written).
 *
 * \retval 0       Success.
 * \retval -EINVAL  NULL pointer or volume not found.
 */
int ubi_volume_get_info(struct ubi_device *ubi, int vol_id, struct ubi_volume_config *vol_cfg,
			size_t *alloc_lebs);

/** \} name ubi_volumes */

/**
 * \defgroup ubi_io UBI LEB Operations
 * \brief Functions to map, unmap, read, write, and query logical erase blocks.
 *
 * \note **Thread safety**: all functions in this group use a per-device mutex.
 *       They must NOT be called from ISR context.
 * \{
 */

/**
 * \brief Write data to a logical erase block (LEB).
 *
 * Maps the LEB if not already mapped, then writes \p len bytes from \p buf.
 * The LEB must belong to a dynamic volume. \p len must be aligned to
 * \c mtd.write_block_size and must not exceed the LEB data size.
 *
 * \param[in] ubi 		UBI device handle.
 * \param[in] vol_id 		Volume identifier.
 * \param[in] lnum 		Logical block number (must be < vol_cfg.leb_count).
 * \param[in] buf 		Data buffer to write (caller retains ownership).
 * \param[in] len 		Number of bytes to write from \p buf.
 *
 * \retval 0       Success.
 * \retval -EINVAL  NULL pointer, invalid vol_id, lnum out of range, or len misaligned.
 * \retval -EIO     Flash write failure.
 */
int ubi_leb_write(struct ubi_device *ubi, int vol_id, size_t lnum, const void *buf, size_t len);

/**
 * \brief Read data from a logical erase block (LEB).
 *
 * Reads \p len bytes starting at \p offset from the LEB into \p buf.
 * The LEB must be mapped (written to) before reading.
 *
 * \param[in] ubi 		UBI device handle.
 * \param[in] vol_id 		Volume identifier.
 * \param[in] lnum 		Logical block number (must be < vol_cfg.leb_count).
 * \param[in] offset 		Byte offset within the LEB to start reading.
 * \param[out] buf 		Buffer to receive the data.
 * \param[in] len		Number of bytes to read.
 *
 * \retval 0       Success.
 * \retval -EINVAL  NULL pointer, invalid vol_id, lnum out of range, or offset+len overflow.
 * \retval -EIO     Flash read failure.
 */
int ubi_leb_read(struct ubi_device *ubi, int vol_id, size_t lnum, size_t offset, void *buf,
		 size_t len);

/**
 * \brief Map a logical erase block (LEB) to a physical block.
 *
 * Allocates a free PEB for the given LEB. The PEB is erased and headers
 * are written. If the LEB is already mapped, returns success.
 *
 * \param[in] ubi 		UBI device handle.
 * \param[in] vol_id 		Volume identifier.
 * \param[in] lnum 		Logical block number (must be < vol_cfg.leb_count).
 *
 * \retval 0       Success.
 * \retval -EINVAL  NULL pointer, invalid vol_id, or lnum out of range.
 * \retval -ENOSPC  No free PEBs available.
 */
int ubi_leb_map(struct ubi_device *ubi, int vol_id, size_t lnum);

/**
 * \brief Unmap a logical erase block (LEB).
 *
 * Reclaims the PEB backing the given LEB to the dirty pool.
 * If the LEB is not mapped, returns success.
 *
 * \param[in] ubi 		UBI device handle.
 * \param[in] vol_id 		Volume identifier.
 * \param[in] lnum 		Logical block number (must be < vol_cfg.leb_count).
 *
 * \retval 0       Success.
 * \retval -EINVAL  NULL pointer, invalid vol_id, or lnum out of range.
 */
int ubi_leb_unmap(struct ubi_device *ubi, int vol_id, size_t lnum);

/**
 * \brief Check whether a logical erase block is mapped.
 *
 * \param[in] ubi 		UBI device handle.
 * \param[in] vol_id 		Volume identifier.
 * \param[in] lnum 		Logical block number (must be < vol_cfg.leb_count).
 * \param[out] is_mapped 	Set to true if the LEB is mapped, false otherwise.
 *
 * \retval 0       Success.
 * \retval -EINVAL  NULL pointer, invalid vol_id, or lnum out of range.
 */
int ubi_leb_is_mapped(struct ubi_device *ubi, int vol_id, size_t lnum, bool *is_mapped);

/**
 * \brief Get the data size of a mapped LEB.
 *
 * Returns the number of user data bytes stored in the LEB.
 * The LEB must be mapped.
 *
 * \param[in] ubi 		UBI device handle.
 * \param[in] vol_id 		Volume identifier.
 * \param[in] lnum 		Logical block number (must be < vol_cfg.leb_count).
 * \param[out] size		Data size in bytes stored in the LEB.
 *
 * \retval 0       Success.
 * \retval -EINVAL  NULL pointer, invalid vol_id, or lnum out of range.
 */
int ubi_leb_get_size(struct ubi_device *ubi, int vol_id, size_t lnum, size_t *size);

/** \} name ubi_io */

#endif /* UBI_H */
