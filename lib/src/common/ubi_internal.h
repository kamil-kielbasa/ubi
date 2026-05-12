/**
 * \file    ubi_internal.h
 * \author  Kamil Kielbasa
 * \brief   UBI internal types and helpers shared across implementation files.
 *
 * \copyright Copyright (c) 2025
 */

/* Include guard -------------------------------------------------------------------------------- */

#ifndef UBI_INTERNAL_H
#define UBI_INTERNAL_H

/* Include files -------------------------------------------------------------------------------- */

/* Public headers: */
#include "ubi.h"
#include "ubi_test.h"

#if defined(CONFIG_UBI_CRYPTO)
#include <ubi_crypto.h>
#endif /* CONFIG_UBI_CRYPTO */

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

/* Types and type definitions ------------------------------------------------------------------- */

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

	/**
	 * \brief Cached AEAD counter floor for this {kv, vol_id} (RAM-only mirror).
	 *
	 * Strict upper bound on any (leb_write_counter, leb_total_auth_bytes)
	 * ever authenticated for this volume on flash.  Maintained by:
	 *   - attach scan: MAX-merge over anchor + every data PEB whose VID auth
	 *     succeeds (init_scan_data_pebs, scan_map_first replace-anchor branch);
	 *   - ubi_secure_anchor_create(): seed to the just-written anchor values;
	 *   - leb_prepare_new_mapping(): bump to projected post-write values BEFORE
	 *     leb_data_write() (conservative nonce reservation: counters are a
	 *     one-way ratchet, so a partial-write retry cannot collide);
	 *   - ubi_secure_anchor_rewrite_for_dirty_witness(): refreshed on anchor rewrite.
	 *
	 * Invariant (strict-monotonic counter implies unique witness):
	 *   at any moment AT MOST ONE on-flash PEB (live or dirty) belonging to
	 *   this volume carries vid_meta.leb_write_counter == cached_leb_write_counter.
	 *   If none does, the anchor PEB carries it.
	 */
	uint64_t cached_leb_write_counter;
	uint64_t cached_leb_total_auth_bytes;
#endif /* CONFIG_UBI_CRYPTO */
};

#if defined(CONFIG_UBI_CRYPTO)
/**
 * \brief MAX-merge observed VID secure metadata into the per-volume cache.
 *
 * Called for every successful VID authentication that belongs to a known
 * volume (live mappings, anchor, duplicate-loser dirty PEBs).  Preserves
 * the cache invariant across attach scan and runtime updates.
 */
static inline void ubi_volume_observe_counters(struct ubi_volume *vol, uint64_t leb_write_counter,
					       uint64_t leb_total_auth_bytes)
{
	if (leb_write_counter > vol->cached_leb_write_counter) {
		vol->cached_leb_write_counter = leb_write_counter;
	}

	if (leb_total_auth_bytes > vol->cached_leb_total_auth_bytes) {
		vol->cached_leb_total_auth_bytes = leb_total_auth_bytes;
	}
}
#endif /* CONFIG_UBI_CRYPTO */

/**
 * \brief PEB pool: rbtree of PEBs keyed by erase counter plus its size.
 *
 * Keeping the count next to the tree makes "drop the tree, the count is
 * already wrong" classes of bugs harder to write. Used for both the free
 * and the dirty pool on every \ref ubi_device.
 */
struct ubi_peb_pool {
	struct rbtree tree; /**< Red-black tree: key=erase counter, value=PEB index. */
	size_t count; /**< Cached cardinality of \c tree. */
};

#if defined(CONFIG_UBI_CRYPTO)
/**
 * \brief Per-domain AEAD counter floor (RAM mirror of on-flash counters).
 *
 * \c next_vid / \c next_ec are bumped at every successful data-PEB write.
 * \c next_res_peb is shared by the DEVICE_HEADER and VOLUME_HEADER on-flash
 * domains: a single reserved-PEB commit writes one DEV record followed by N
 * VOL records, so it consumes \c 1 + N consecutive slots from the same
 * monotonic sequence (this guarantees nonce uniqueness for the reserved
 * PEB area). The two domains still get their own budget_base in
 * \ref ubi_secure_budget_bases because they use distinct HKDF child keys
 * and have different per-record AAD sizes, so their bytes-budgets fill at
 * different rates.
 */
struct ubi_secure_aead_counters {
	uint64_t next_res_peb; /**< Next unused reserved-PEB AEAD slot (DEV+VOL). */
	uint64_t next_vid; /**< Next unused VID-domain AEAD counter. */
	uint64_t next_ec; /**< Next unused EC-domain AEAD counter. */
};

/**
 * \brief Per-domain budget bases.
 *
 * Counter values captured at the moment the current write-active key version
 * was activated. Subtraction (current counter - base) yields the true number
 * of AEAD invocations performed under the active kv. RAM-only; captured by
 * \ref ubi_secure_budget_bases_init during attach. DEVICE_HEADER and
 * VOLUME_HEADER are tracked separately even though they share the on-flash
 * counter \c next_res_peb -- they use distinct HKDF child keys and have
 * different per-record AAD sizes, so the VOLUME_HEADER bytes-budget fills
 * faster than the DEVICE_HEADER one.
 */
struct ubi_secure_budget_bases {
	uint64_t dev; /**< Base for DEVICE_HEADER domain. */
	uint64_t vol; /**< Base for VOLUME_HEADER domain. */
	uint64_t ec; /**< Base for ERASE_COUNTER domain. */
	uint64_t vid; /**< Base for VOLUME_IDENTIFIER domain. */
};

/**
 * \brief Freshness state mirrored in RAM.
 *
 * \c cached_device_revision is the dev_hdr revision reported by the most
 * recent successful attach / sync; it is included in every freshness
 * snapshot we hand back to the host. \c mutations_since_sync counts how
 * many reserved-metadata mutations have been committed since the last
 * \c sync_freshness callback returned success, so that the runtime can
 * batch syncs by mutation count.
 */
struct ubi_secure_freshness_state {
	uint64_t cached_device_revision; /**< Cached dev_hdr revision for snapshots. */
	size_t mutations_since_sync; /**< Mutations since last sync_freshness call. */
};
#endif /* CONFIG_UBI_CRYPTO */

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

	struct ubi_flash_desc flash; /**< Underlying flash partition descriptor. */

	bool read_only_degraded; /**< True if reserved PEB redundancy is lost. */

#if defined(CONFIG_UBI_CRYPTO)
	const struct ubi_crypto_config
		*crypto_cfg; /**< Secure backend crypto config (NULL for plain). */
	bool read_only_crypto; /**< Sticky crypto-initiated read-only. */
	uint8_t reserved_key_version; /**< Key version currently used by reserved PEBs. */
	uint32_t key_peb_refcount[CONFIG_UBI_CRYPTO_MAX_KEY_VERSIONS]; /**< Per-allowlist-slot
	    PEB refcount: number of on-flash objects authenticated with each key version.
	    Includes data-PEB EC/VID/LEB objects AND reserved-PEB objects.
	    Indexed by allowlist position, not by raw key_version value. */
	struct ubi_secure_aead_counters aead; /**< Per-domain AEAD counter floor. */
	struct ubi_secure_budget_bases budget_bases; /**< Per-domain budget bases. */
	struct ubi_secure_freshness_state freshness; /**< Freshness state. */
#endif /* CONFIG_UBI_CRYPTO */

	size_t total_data_peb_count; /**< Total usable data PEBs (cached at init). */
	size_t leb_size; /**< Usable data size per LEB in bytes (cached at init). */

	struct ubi_peb_pool free_pool; /**< Free PEBs (key=erase counter, value=PEB idx). */
	struct ubi_peb_pool dirty_pool; /**< Dirty PEBs awaiting erasure. */

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
#endif /* CONFIG_UBI_TEST_API_ENABLE */
};

/* Mutation gate -------------------------------------------------------------------------------- */

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
#endif /* CONFIG_UBI_TEST_API_ENABLE */

#if defined(CONFIG_UBI_CRYPTO)
	if (ubi->read_only_crypto) {
		return -EROFS;
	}
#endif /* CONFIG_UBI_CRYPTO */

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
#endif /* CONFIG_UBI_CRYPTO */
	}
	return total;
}

/* Internal helper function declarations -------------------------------------------------------- */

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
 * \brief Validate a full volume configuration.
 *
 * Checks that the name is non-empty and NUL-terminated within
 * UBI_VOLUME_NAME_MAX_LEN bytes, the volume type enumerator is valid,
 * and that leb_count > 0.
 *
 * \param[in] cfg  Volume configuration to validate.
 *
 * \retval true  Configuration is valid.
 * \retval false One or more fields are invalid.
 */
static inline bool ubi_volume_config_is_valid(const struct ubi_volume_config *cfg)
{
	const size_t name_len = strnlen(cfg->name, UBI_VOLUME_NAME_MAX_LEN);

	if (name_len == 0 || name_len >= UBI_VOLUME_NAME_MAX_LEN)
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
 * \param[in] flash          Flash partition descriptor.
 * \param[out] erased_val  Erased byte value for the partition.
 *
 * \return 0 on success, or negative errno on failure.
 */
int ubi_get_erased_val(const struct ubi_flash_desc *flash, uint8_t *erased_val);

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
