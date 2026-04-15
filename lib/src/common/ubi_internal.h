/**
 * \file    ubi_internal.h
 * \author  Kamil Kielbasa
 * \brief   UBI internal types and helpers shared across implementation files.
 *
 * \copyright Copyright (c) 2025
 */

/* Include guard ------------------------------------------------------------------------------- */
#ifndef UBI_INTERNAL_H
#define UBI_INTERNAL_H

/* Include files ------------------------------------------------------------------------------- */

/* Public header: */
#include "ubi.h"
#include "ubi_test.h"

#if defined(CONFIG_UBI_CRYPTO)
#include <ubi_crypto.h>
#endif

/* Internal headers: */
#include "ubi_backend.h"
#include "ubi_cache.h"

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/sys/rb.h>
#include <zephyr/sys/slist.h>

/* Standard library headers: */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Types and type definitions ------------------------------------------------------------------ */

/**
 * \brief UBI volume representation.
 *
 * This structure describes a single UBI (Unsorted Block Images) volume
 * and its associated configuration and erase block mapping.
 */
struct ubi_volume {
	size_t vol_id; /**< Unique identifier of the volume. */
	struct ubi_volume_config cfg; /**< Volume configuration parameters. */

	size_t eba_tbl_count; /**< Size of the eraseblock association (EBA) table. */
	struct rbtree eba_tbl; /**< Red-black tree mapping:
                                     - Key: Logical Erase Block (LEB) index
                                     - Value: Physical Erase Block (PEB) index */

#if defined(CONFIG_UBI_CRYPTO)
	size_t anchor_pnum; /**< PEB index of hidden anchor (SIZE_MAX = none). */
#endif
};

/**
 * \brief UBI device representation.
 *
 * This structure describes a UBI device with its PEB management
 * structures and global sequencing information.
 */
struct ubi_device {
	struct k_mutex mutex;

	enum ubi_device_mode mode; /**< Backend mode (plain or secure). */
	const struct ubi_backend_ops *ops; /**< Backend operations vtable. */

	struct ubi_mtd mtd; /**< Underlying MTD (Memory Technology Device). */

#if defined(CONFIG_UBI_CRYPTO)
	const struct ubi_crypto_config
		*crypto_cfg; /**< Secure backend crypto config (NULL for plain). */
	uint64_t next_vid_counter; /**< Next unused VID-domain AEAD counter (write_active_kv). */
#endif

	bool read_only_degraded; /**< True if reserved PEB redundancy is lost. */

	size_t total_data_peb_count; /**< Total usable data PEBs (cached at init). */
	size_t leb_size; /**< Usable data size per LEB in bytes (cached at init). */

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

	size_t ec_sum; /**< Sum of all data-PEB erase counters. */
	size_t ec_count; /**< Number of data PEBs with readable EC headers. */

	size_t vol_id_watermark; /**< Monotonic volume ID counter (mirrors dev_hdr). */
	size_t vol_count; /**< Number of volumes tracked. */
	struct rbtree vols; /**< Red-black tree of volumes:
			       - Key: Volume identifier
			       - Value: Volume pointer */

#if defined(CONFIG_UBI_TEST_API_ENABLE)
	bool test_write_shutdown; /**< Test-only: when true, all mutations are blocked. */
#endif
};

/* Mutation gate ------------------------------------------------------------------------------ */

/**
 * \brief Classification of UBI mutation operations.
 *
 * Every public function that mutates flash or persistent state declares its
 * mutation class. The central gate ubi_mutation_allowed() uses this class to
 * decide whether the operation is currently permitted.
 */
enum ubi_mutation_class {
	/** Volume create / resize / remove — writes to reserved PEB metadata. */
	UBI_MUT_RESERVED_METADATA,

	/** leb_write / leb_map / leb_unmap — writes to data PEBs. */
	UBI_MUT_DATA_PATH,

	/** erase_peb — maintenance / garbage collection. */
	UBI_MUT_MAINTENANCE,
};

/**
 * \brief Central mutation gate — check whether a mutation is allowed.
 *
 * All public mutators call this function before performing any flash I/O.
 * The gate centralizes the read-only / degraded-mode policy in one place.
 *
 * Current policy (plain UBI):
 *   - UBI_MUT_RESERVED_METADATA: blocked when read_only_degraded is true.
 *   - UBI_MUT_DATA_PATH: always allowed.
 *   - UBI_MUT_MAINTENANCE: always allowed.
 *
 * Under CONFIG_UBI_TEST_API_ENABLE, the test_write_shutdown flag blocks
 * all mutation classes regardless of degraded state.
 *
 * \param[in] ubi       UBI device (caller holds mutex).
 * \param     op_class  Mutation class of the requested operation.
 *
 * \retval 0       Mutation is allowed.
 * \retval -EROFS  Mutation is blocked.
 */
static inline int ubi_mutation_allowed(const struct ubi_device *ubi,
				       enum ubi_mutation_class op_class)
{
#if defined(CONFIG_UBI_TEST_API_ENABLE)
	if (ubi->test_write_shutdown) {
		return -EROFS;
	}
#endif

	if (op_class == UBI_MUT_RESERVED_METADATA && ubi->read_only_degraded) {
		return -EROFS;
	}

	return 0;
}

/**
 * \brief Compute the total number of PEBs reserved by all volumes.
 *
 * Sums `cfg.leb_count` across all volumes in the device tree.
 * Must be called with the device mutex held.
 *
 * \param[in] ubi  UBI device (must not be NULL, must have initialized vols tree).
 *
 * \return Sum of leb_count across all volumes.
 */
static inline size_t ubi_reserved_peb_count(struct ubi_device *ubi)
{
	size_t total = 0;
	struct ubi_rbt_item *entry = NULL;

	RB_FOR_EACH_CONTAINER(&ubi->vols, entry, node)
	{
		total += entry->value.vol->cfg.leb_count;
#if defined(CONFIG_UBI_CRYPTO)
		if (ubi->mode == UBI_MODE_SECURE && entry->value.vol->anchor_pnum != SIZE_MAX) {
			total += 1;
		}
#endif
	}
	return total;
}

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

/**
 * \brief Validate a volume name supplied by the caller.
 *
 * A valid name is non-empty and NUL-terminated within UBI_VOLUME_NAME_MAX_LEN bytes.
 *
 * \param[in] name  Name buffer (UBI_VOLUME_NAME_MAX_LEN bytes).
 *
 * \retval true  Name is valid.
 * \retval false Name is empty or missing NUL terminator.
 */
static inline bool ubi_validate_volume_name(const char *name)
{
	const size_t len = strnlen(name, UBI_VOLUME_NAME_MAX_LEN);
	return (len > 0 && len < UBI_VOLUME_NAME_MAX_LEN);
}

/**
 * \brief Validate a full volume configuration.
 *
 * Checks name validity, volume type enumerator, and that leb_count > 0.
 *
 * \param[in] cfg  Volume configuration to validate.
 *
 * \retval true  Configuration is valid.
 * \retval false One or more fields are invalid.
 */
static inline bool ubi_volume_config_is_valid(const struct ubi_volume_config *cfg)
{
	if (!ubi_validate_volume_name(cfg->name))
		return false;
	if (cfg->type != UBI_VOLUME_TYPE_STATIC && cfg->type != UBI_VOLUME_TYPE_DYNAMIC)
		return false;
	if (cfg->leb_count == 0)
		return false;
	return true;
}

/**
 * \brief Get the erased byte value for the flash partition backing a UBI device.
 *
 * Opens the flash area, queries the hardware-reported erased value, and closes
 * the area. The returned value is typically 0xFF for NOR flash but may differ
 * on other technologies.
 *
 * \param[in] mtd          UBI MTD descriptor.
 * \param[out] erased_val  Erased byte value for the partition.
 *
 * \return 0 on success, or negative errno on failure.
 */
int ubi_get_erased_val(const struct ubi_mtd *mtd, uint8_t *erased_val);

/**
 * \brief Check whether a buffer is entirely filled with the erased byte value.
 *
 * \param[in] buf        Buffer to check.
 * \param len            Length of the buffer in bytes.
 * \param erased_val     Expected erased byte value.
 *
 * \retval true   Every byte in \p buf equals \p erased_val.
 * \retval false  At least one byte differs.
 */
static inline bool ubi_buf_is_erased(const void *buf, size_t len, uint8_t erased_val)
{
	const uint8_t *p = (const uint8_t *)buf;

	for (size_t i = 0; i < len; ++i) {
		if (p[i] != erased_val) {
			return false;
		}
	}
	return true;
}

#endif /* UBI_INTERNAL_H */
