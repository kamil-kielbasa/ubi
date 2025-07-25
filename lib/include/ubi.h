/**
 * \file    ubi.h
 *
 * \brief   Unsorted Block Images (UBI) interface
 *
 * Provides APIs for initializing and managing UBI over raw flash memory.
 * UBI handles wear leveling, bad block management, and logical block mapping.
 *
 * \author  Kamil Kielbasa
 * \version 0.1
 * \date    2025-07-25
 *
 * \copyright Copyright (c) 2025
 */

#ifndef UBI_H
#define UBI_H

#ifdef __cplusplus
extern "C" {
#endif

/* Include files ----------------------------------------------------------- */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Defines ----------------------------------------------------------------- */

/* Types and type definitions ---------------------------------------------- */

/**
 * \defgroup ubi_structs UBI Data Structures
 * \{
 */

/**
 * \brief Memory technology device (MTD) descriptor for UBI.
 */
struct mem_tech_device {
	const struct device *dev; /*!< Pointer to hardware device. */

	size_t wb_size; /*!< Write block size in bytes. */
	size_t eb_size; /*!< Erase block size in bytes. */

	size_t p_off; /*!< Partition offset in bytes. */
	size_t p_size; /*!< Partition size in bytes. */
};

/**
 * \brief UBI device statistics.
 */
struct ubi_device_info {
	size_t alloc_pebs; /*!< Number of allocated physical erase blocks. */
	size_t free_pebs; /*!< Number of free physical erase blocks. */
	size_t dirty_pebs; /*!< Number of dirty physical erase blocks. */
	size_t bad_pebs; /*!< Number of bad physical erase blocks. */

	size_t leb_count; /*!< Total number of logical erase blocks. */
	size_t leb_size; /*!< Size of each logical erase block in bytes. */
};

/**
 * \brief UBI flash metadata and wear-leveling info.
 */
struct ubi_flash_info {
	size_t ec_average; /*!< Average erase counter across PEBs. */

	size_t peb_count; /*!< Total number of PEBs. */
	bool *peb_init; /*!< Initialization flags for each PEB. */
	uint32_t *peb_ec; /*!< Erase counters for each PEB. */
};

/** \} name ubi_structs */

/* Module interface variables and constants -------------------------------- */
/* Extern variables and constant declarations ------------------------------ */
/* Module interface function declarations ---------------------------------- */

/**
 * \defgroup ubi_lifecycle UBI Initialization, Shutdown, and Maintenance
 * \brief Functions to manage UBI lifecycle, device state, and maintenance.
 * \{
 */

/**
 * \brief Initialize the UBI subsystem with a given memory device.
 *
 * \param[in] mtd Pointer to memory technology device descriptor.
 *
 * \return \c 0 on success, or a negative error code on failure.
 */
int ubi_init(const struct mem_tech_device *mtd);

/**
 * \brief Deinitialize the UBI subsystem and release resources.
 *
 * \return \c 0 on success, or a negative error code on failure.
 */
int ubi_deinit(void);

/**
 * \brief Retrieve UBI device and flash statistics.
 *
 * \param[out] dev_info Pointer to structure to receive device stats.
 * \param[out] flash_info Pointer to structure to receive flash stats.
 *
 * \return \c 0 on success, or a negative error code.
 */
int ubi_info(struct ubi_device_info *dev_info, struct ubi_flash_info *flash_info);

/**
 * \brief Trigger erase operation on a physical erase block.
 *
 * \return \c 0 on success, or a negative error code.
 */
int ubi_peb_erase(void);

/** \} name ubi_lifecycle */

/**
 * \defgroup ubi_io Logical Block I/O Operations
 * \brief Read/write and mapping functions for logical erase blocks (LEBs).
 * \{
 */

/**
 * \brief Write data to a logical erase block (LEB).
 *
 * \param lnum Logical block number.
 * \param[in] buf Pointer to buffer containing data to write.
 * \param len Number of bytes to write.
 *
 * \return \c 0 on success, or a negative error code.
 */
int ubi_leb_write(size_t lnum, const void *buf, size_t len);

/**
 * \brief Read data from a logical erase block (LEB).
 *
 * \param lnum Logical block number.
 * \param[out] buf Buffer to store the read data.
 * \param offset Offset within the block to start reading.
 * \param[in,out] len Pointer to input/output length. Updated with bytes read.
 *
 * \return \c 0 on success, or a negative error code.
 */
int ubi_leb_read(size_t lnum, void *buf, size_t offset, size_t *len);

/**
 * \brief Map a logical erase block (LEB) to a physical block.
 *
 * \param lnum Logical block number.
 *
 * \return \c 0 on success, or a negative error code.
 */
int ubi_leb_map(size_t lnum);

/**
 * \brief Unmap a logical erase block (LEB).
 *
 * \param lnum Logical block number.
 *
 * \return \c 0 on success, or a negative error code.
 */
int ubi_leb_unmap(size_t lnum);

/**
 * \brief Check if a logical erase block (LEB) is currently mapped.
 *
 * \param lnum Logical block number.
 * \param[out] is_map Pointer to boolean set to true if mapped.
 *
 * \return \c 0 on success, or a negative error code.
 */
int ubi_leb_is_mapped(size_t lnum, bool *is_map);

/** \} name ubi_io */

#ifdef __cplusplus
}
#endif

#endif /* UBI_H */
