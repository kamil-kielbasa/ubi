/**
 * \file    ubi_internal.h
 * \author  Kamil Kielbasa
 * \brief   UBI internal types and helpers shared across implementation files.
 * \version 0.10
 * \date    2026-03-27
 *
 * \copyright Copyright (c) 2025
 */

/* Include guard ------------------------------------------------------------------------------- */
#ifndef UBI_INTERNAL_H
#define UBI_INTERNAL_H

/* Include files ------------------------------------------------------------------------------- */

/* Public header: */
#include "ubi.h"

/* Internal headers: */
#include "ubi_cache.h"
#include "ubi_io.h"
#include "ubi_res_peb.h"

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/sys/rb.h>
#include <zephyr/sys/slist.h>

/* Standard library headers: */
#include <stddef.h>
#include <stdint.h>

/* Types and type definitions ------------------------------------------------------------------ */

/**
 * \brief UBI volume representation.
 *
 * This structure describes a single UBI (Unsorted Block Images) volume
 * and its associated configuration and erase block mapping.
 */
struct ubi_volume {
	size_t vol_idx; /**< Index of the volume within the UBI device. */
	size_t vol_id; /**< Unique identifier of the volume. */
	struct ubi_volume_config cfg; /**< Volume configuration parameters. */

	size_t eba_tbl_count; /**< Size of the eraseblock association (EBA) table. */
	struct rbtree eba_tbl; /**< Red-black tree mapping:
                                     - Key: Logical Erase Block (LEB) index
                                     - Value: Physical Erase Block (PEB) index */
};

BUILD_ASSERT(sizeof(struct ubi_volume) == 48);

/**
 * \brief UBI device representation.
 *
 * This structure describes a UBI device with its PEB management
 * structures and global sequencing information.
 */
struct ubi_device {
	struct k_mutex mutex;

	struct ubi_mtd mtd; /**< Underlying MTD (Memory Technology Device). */

	size_t free_peb_count; /**< Number of free PEBs available. */
	struct rbtree free_pebs; /**< Red-black tree of free PEBs:
                                     - Key: Erase counter
                                     - Value: PEB index */

	size_t dirty_peb_count; /**< Number of dirty PEBs (need erasure). */
	struct rbtree dirty_pebs; /**< Red-black tree of dirty PEBs:
                                     - Key: Erase counter
                                     - Value: PEB index */

	size_t bad_peb_count; /**< Number of bad PEBs detected. */
	sys_slist_t bad_pebs; /**< Singly linked list of bad PEB indices. */

	uint64_t global_sqnum; /**< Global sequence number for updates. */

	size_t vol_next_id; /**< Volume sequence counter. */
	size_t vol_count; /**< Number of volumes tracked. */
	struct rbtree vols; /**< Red-black tree of volumes:
			       - Key: Volume identifier
			       - Value: Volume pointer */
};

/* Size varies by platform (pointer width, mutex implementation). */
BUILD_ASSERT(sizeof(struct ubi_device) > 0);

/* Internal helper function declarations ------------------------------------------------------- */

/**
 * \brief Move a PEB to the bad blocks list.
 *
 * Transfers the given physical erase block (PEB) into the list of bad blocks, updating its
 * associated erase counter metadata.
 *
 * \param[in] ubi     	Pointer to the UBI device structure.
 * \param pnum       	Physical erase block index.
 * \param erase_count 	Number of erasures performed on this PEB.
 * \param[in] bad_item	Pointer to the list item representing the bad PEB.
 */
void ubi_move_to_bad_blocks(struct ubi_device *ubi, size_t pnum, size_t erase_count,
			    struct ubi_list_item *bad_item);

/**
 * \brief Find a volume by its identifier.
 *
 * Searches the UBI device volume tree for the volume with the given ID.
 * Must be called with the device mutex held.
 *
 * \param[in] ubi    Pointer to the UBI device structure.
 * \param vol_id     Volume identifier to search for.
 *
 * \return Pointer to the volume on success, NULL if not found.
 */
struct ubi_volume *ubi_find_volume(struct ubi_device *ubi, int vol_id);

#endif /* UBI_INTERNAL_H */
