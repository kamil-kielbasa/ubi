/**
 * \file    ubi_secure_runtime.c
 * \author  Kamil Kielbasa
 * \brief   Secure backend device runtime: get_info, erase_peb, deinit.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files ------------------------------------------------------------------------------- */
#include "ubi_secure_ops.h"
#include "ubi_secure_crypto.h"
#include "ubi_secure_event.h"
#include "ubi_secure_io.h"
#include "ubi_secure_reserved.h"
#include "ubi_secure_types.h"
#include "ubi_internal.h"
#include "ubi_plain_io.h"
#include "ubi_mem.h"
#include "ubi_partition_guard.h"

#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/crc.h>
#include <zephyr/storage/flash_map.h>

#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------ */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Static function declarations ---------------------------------------------------------------- */

static int erase_dirty_entry(struct ubi_device *ubi, struct ubi_rbt_item *entry);
static void torture_bad_blocks(struct ubi_device *ubi);
static int maybe_rewrite_anchor_for_dirty(struct ubi_device *ubi, size_t dirty_pnum);

/* Static function definitions ----------------------------------------------------------------- */

/**
 * \brief Erase a single dirty PEB and move it to the free pool.
 *
 * On I/O failure the PEB is moved to the bad-block list. The entry must
 * already reside in dirty_pebs.
 *
 * \param[in] ubi   UBI device (caller holds mutex).
 * \param[in] entry Dirty tree item to erase.
 *
 * \retval 0    Success — PEB erased and promoted to free pool.
 * \retval -EIO I/O or crypto failure — PEB marked bad.
 */
static int erase_dirty_entry(struct ubi_device *ubi, struct ubi_rbt_item *entry)
{
	__ASSERT_NO_MSG(ubi != NULL);
	__ASSERT_NO_MSG(entry != NULL);

	struct ubi_ec_hdr ec_hdr = { 0 };
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };

	int ret = ubi_secure_ec_hdr_read(&ubi->mtd, ubi->crypto_cfg, entry->value.pnum, &ec_hdr,
					 &ec_ctx);
	if (ret != 0) {
		LOG_ERR("EC header read failure for PEB %zu", (size_t)entry->value.pnum);
		goto mark_bad;
	}

	/* Probe VID header before erase — needed for full key-version refcount. */
	struct ubi_vid_hdr vid_hdr_probe = { 0 };
	struct ubi_vid_secure_meta vid_meta_probe = { 0 };
	struct ubi_secure_vid_auth_ctx vid_ctx_probe = { 0 };
	bool had_vid = false;

	if (ubi_secure_vid_hdr_read(&ubi->mtd, ubi->crypto_cfg, entry->value.pnum, &ec_ctx,
				    &vid_hdr_probe, &vid_meta_probe, &vid_ctx_probe) == 0) {
		had_vid = true;
	}

	const struct flash_area *fa = NULL;

	ret = flash_area_open(ubi->mtd.partition_id, &fa);
	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		return ret;
	}

	const size_t offset = entry->value.pnum * ubi->mtd.erase_block_size;

	ret = flash_area_erase(fa, offset, ubi->mtd.erase_block_size);
	flash_area_close(fa);

	if (ret != 0) {
		LOG_ERR("Flash erase failure");
		goto mark_bad;
	}

	ec_hdr.ec += 1;

	const uint8_t write_kv = ubi->crypto_cfg->policy.requested_write_key_version;

	ret = ubi_secure_ec_hdr_write(&ubi->mtd, ubi->crypto_cfg, entry->value.pnum, &ec_hdr,
				      write_kv, ubi->next_ec_counter);
	if (ret != 0) {
		LOG_ERR("EC header write failure");
		ubi_secure_handle_write_error(ubi, ret, entry->value.pnum);
		goto mark_bad;
	}

	ubi->next_ec_counter++;

	/* Update key-version refcounts: old objects destroyed, new EC written. */
	ubi_secure_key_refcount_dec_and_check(ubi, ec_ctx.key_version);
	if (had_vid) {
		ubi_secure_key_refcount_dec_and_check(ubi, vid_ctx_probe.key_version);
		ubi_secure_key_refcount_dec_and_check(ubi, vid_ctx_probe.key_version);
	}
	ubi_secure_key_refcount_inc(ubi, write_kv);

	/* Move from dirty to free. */
	rb_remove(&ubi->dirty_pebs, &entry->node);
	ubi->dirty_peb_count--;

	ubi->ec_sum += 1;

	entry->key = ec_hdr.ec;
	rb_insert(&ubi->free_pebs, &entry->node);
	ubi->free_peb_count++;
	/* clang-format off */
	return 0;

mark_bad: {
	/* clang-format on */
	const size_t pnum = entry->value.pnum;
	const size_t ec = entry->key;

	rb_remove(&ubi->dirty_pebs, &entry->node);
	ubi->dirty_peb_count--;

	ubi->ec_sum -= ec;
	ubi->ec_count--;

	struct ubi_list_item *bad_item = ubi_leaf_as_list(entry);

	ubi_move_to_bad_blocks(ubi, pnum, ec, bad_item);
	return ret;
}
}

/**
 * \brief If the dirty PEB is the last writable witness, rewrite the anchor.
 *
 * Before erasing a dirty PEB, check whether its VID carries a
 * leb_write_counter higher than the volume's hidden anchor.  If so — and no
 * mapped PEB for the same volume still carries that counter — the anchor
 * must be rewritten to inherit the counter state before the dirty PEB is
 * destroyed.
 *
 * \param[in] ubi        UBI device (caller holds mutex).
 * \param[in] dirty_pnum Physical erase block number of the dirty PEB.
 *
 * \retval 0       No rewrite needed, or anchor rewritten successfully.
 * \retval -EIO    I/O or crypto failure during rewrite.
 * \retval -ENOSPC No free PEB for anchor rewrite.
 */
static int maybe_rewrite_anchor_for_dirty(struct ubi_device *ubi, size_t dirty_pnum)
{
	__ASSERT_NO_MSG(ubi != NULL);

	/* 1. Read the dirty PEB's VID header to get volume and counter state. */
	struct ubi_ec_hdr ec_hdr = { 0 };
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };

	int ret = ubi_secure_ec_hdr_read(&ubi->mtd, ubi->crypto_cfg, dirty_pnum, &ec_hdr, &ec_ctx);

	if (ret != 0) {
		/* Cannot read EC — PEB might already be partially erased.
		 * Treat as safe to erase (no witness data). */
		return 0;
	}

	struct ubi_vid_hdr vid_hdr = { 0 };
	struct ubi_vid_secure_meta vid_meta = { 0 };
	struct ubi_secure_vid_auth_ctx vid_ctx = { 0 };

	ret = ubi_secure_vid_hdr_read(&ubi->mtd, ubi->crypto_cfg, dirty_pnum, &ec_ctx, &vid_hdr,
				      &vid_meta, &vid_ctx);
	if (ret != 0) {
		/* VID unreadable — no counter state to protect. */
		return 0;
	}

	/* 2. Find the volume this VID belongs to. */
	struct ubi_rbt_item *vol_entry = ubi_cache_search(&ubi->vols, vid_hdr.vol_id);

	if (vol_entry == NULL) {
		/* Orphan PEB — volume was removed. Safe to erase. */
		return 0;
	}

	struct ubi_volume *vol = vol_entry->value.vol;

	if (vol->anchor_pnum == SIZE_MAX) {
		/* No anchor for this volume — nothing to protect. */
		return 0;
	}

	/* 3. Read anchor's VID meta to compare counter state. */
	struct ubi_ec_hdr anchor_ec = { 0 };
	struct ubi_secure_ec_auth_ctx anchor_ec_ctx = { 0 };

	ret = ubi_secure_ec_hdr_read(&ubi->mtd, ubi->crypto_cfg, vol->anchor_pnum, &anchor_ec,
				     &anchor_ec_ctx);
	if (ret != 0) {
		LOG_WRN("Anchor EC read failure for vol %zu — skipping witness check", vol->vol_id);
		return 0;
	}

	struct ubi_vid_hdr anchor_vid = { 0 };
	struct ubi_vid_secure_meta anchor_meta = { 0 };
	struct ubi_secure_vid_auth_ctx anchor_vid_ctx = { 0 };

	ret = ubi_secure_vid_hdr_read(&ubi->mtd, ubi->crypto_cfg, vol->anchor_pnum, &anchor_ec_ctx,
				      &anchor_vid, &anchor_meta, &anchor_vid_ctx);
	if (ret != 0) {
		LOG_WRN("Anchor VID read failure for vol %zu — skipping witness check",
			vol->vol_id);
		return 0;
	}

	/* 4. If the dirty PEB's counter is not higher, no rewrite needed. */
	if (vid_meta.leb_write_counter <= anchor_meta.leb_write_counter) {
		return 0;
	}

	/* 5. Check if any mapped or other dirty PEB for this volume still
	 *    carries a counter >= the dirty PEB's counter.  If so, the
	 *    counter state is not lost by erasing this PEB. */
	struct ubi_rbt_item *eba_entry = NULL;

	RB_FOR_EACH_CONTAINER(&vol->eba_tbl, eba_entry, node)
	{
		struct ubi_ec_hdr m_ec = { 0 };
		struct ubi_secure_ec_auth_ctx m_ec_ctx = { 0 };
		struct ubi_vid_hdr m_vid = { 0 };
		struct ubi_vid_secure_meta m_meta = { 0 };
		struct ubi_secure_vid_auth_ctx m_vid_ctx = { 0 };

		if (ubi_secure_ec_hdr_read(&ubi->mtd, ubi->crypto_cfg, eba_entry->value.pnum, &m_ec,
					   &m_ec_ctx) != 0) {
			continue;
		}
		if (ubi_secure_vid_hdr_read(&ubi->mtd, ubi->crypto_cfg, eba_entry->value.pnum,
					    &m_ec_ctx, &m_vid, &m_meta, &m_vid_ctx) != 0) {
			continue;
		}
		if (m_meta.leb_write_counter >= vid_meta.leb_write_counter) {
			return 0;
		}
	}

	/* Also check other dirty PEBs — a dirty PEB for the same volume
	 * with counter >= ours is still a witness on flash. */
	struct ubi_rbt_item *dirty_entry = NULL;

	RB_FOR_EACH_CONTAINER(&ubi->dirty_pebs, dirty_entry, node)
	{
		if (dirty_entry->value.pnum == dirty_pnum) {
			continue;
		}

		struct ubi_ec_hdr d_ec = { 0 };
		struct ubi_secure_ec_auth_ctx d_ec_ctx = { 0 };
		struct ubi_vid_hdr d_vid = { 0 };
		struct ubi_vid_secure_meta d_meta = { 0 };
		struct ubi_secure_vid_auth_ctx d_vid_ctx = { 0 };

		if (ubi_secure_ec_hdr_read(&ubi->mtd, ubi->crypto_cfg, dirty_entry->value.pnum,
					   &d_ec, &d_ec_ctx) != 0) {
			continue;
		}
		if (ubi_secure_vid_hdr_read(&ubi->mtd, ubi->crypto_cfg, dirty_entry->value.pnum,
					    &d_ec_ctx, &d_vid, &d_meta, &d_vid_ctx) != 0) {
			continue;
		}
		/* Must be same volume AND carry high enough counter. */
		if (d_vid.vol_id == vid_hdr.vol_id &&
		    d_meta.leb_write_counter >= vid_meta.leb_write_counter) {
			return 0;
		}
	}

	/* 6. Dirty PEB is the last writable witness — rewrite anchor.
	 *    Need a free PEB for the new anchor. */
	if (ubi->free_peb_count == 0) {
		LOG_ERR("No free PEB for anchor rewrite — erase deferred");
		return -ENOSPC;
	}

	struct rbnode *min_node = rb_get_min(&ubi->free_pebs);
	struct ubi_rbt_item *new_item = CONTAINER_OF(min_node, struct ubi_rbt_item, node);

	rb_remove(&ubi->free_pebs, &new_item->node);
	ubi->free_peb_count--;

	const size_t new_pnum = new_item->value.pnum;

	/* Read EC of the new free PEB. */
	struct ubi_ec_hdr new_ec = { 0 };
	struct ubi_secure_ec_auth_ctx new_ec_ctx = { 0 };

	ret = ubi_secure_ec_hdr_read(&ubi->mtd, ubi->crypto_cfg, new_pnum, &new_ec, &new_ec_ctx);
	if (ret != 0) {
		LOG_ERR("EC read failure on anchor rewrite PEB %zu", new_pnum);
		goto rewrite_bad;
	}

	/* Build new anchor VID with inherited counter state. */
	struct ubi_vid_hdr new_vid = { 0 };

	new_vid.magic = UBI_VID_HDR_MAGIC;
	new_vid.version = UBI_VID_HDR_VERSION;
	new_vid.lnum = UBI_SECURE_INTERNAL_ANCHOR_LNUM;
	new_vid.vol_id = vol->vol_id;
	new_vid.sqnum = ubi->global_sqnum++;
	new_vid.data_size = 0;
	new_vid.hdr_crc =
		crc32_ieee((const uint8_t *)&new_vid, sizeof(new_vid) - sizeof(new_vid.hdr_crc));

	/* Advance anchor counters for the rewritten witness. */
	const struct ubi_vid_secure_meta new_meta = {
		.leb_write_counter = vid_meta.leb_write_counter + 1,
		.leb_total_auth_bytes = vid_meta.leb_total_auth_bytes + UBI_SECURE_LEB_AAD_SIZE,
	};

	const uint8_t write_kv = ubi->crypto_cfg->policy.requested_write_key_version;

	/* Write zero-length LEB data. */
	ret = ubi_secure_leb_data_write(&ubi->mtd, ubi->crypto_cfg, new_pnum, &new_ec_ctx, &new_vid,
					write_kv, NULL, 0, write_kv, 0);
	if (ret != 0) {
		LOG_ERR("Anchor rewrite LEB failure on PEB %zu", new_pnum);
		goto rewrite_bad;
	}

	/* Write VID header — commit point. */
	const uint64_t vid_counter = ubi->next_vid_counter;

	ret = ubi_secure_vid_hdr_write(&ubi->mtd, ubi->crypto_cfg, new_pnum, &new_ec_ctx, &new_vid,
				       &new_meta, write_kv, vid_counter);
	if (ret != 0) {
		LOG_ERR("Anchor rewrite VID failure on PEB %zu", new_pnum);
		goto rewrite_bad;
	}

	ubi->next_vid_counter = vid_counter + 1;

	/* Old anchor PEB is now stale — return its leaf to pool and reclaim
	 * the PEB to dirty for eventual erase. We don't need to allocate a
	 * new leaf because anchor PEBs are not in any tree. Instead, we
	 * reuse new_item for the old anchor. */
	const size_t old_anchor_pnum = vol->anchor_pnum;

	vol->anchor_pnum = new_pnum;
	ubi_mem_leaf_free(new_item);

	/* Put old anchor into dirty pool. Read its EC for insertion key. */
	struct ubi_rbt_item *old_item = NULL;

	ret = ubi_mem_leaf_alloc((void **)&old_item);
	if (ret != 0) {
		/* Leaf allocation failed — old anchor PEB is lost. Not critical
		 * because it's now stale and the new anchor is committed. */
		LOG_WRN("Leaf alloc failed for old anchor PEB %zu", old_anchor_pnum);
		return 0;
	}

	old_item->value.pnum = old_anchor_pnum;
	old_item->key = anchor_ec.ec;
	rb_insert(&ubi->dirty_pebs, &old_item->node);
	ubi->dirty_peb_count++;

	/* clang-format off */
	return 0;

rewrite_bad: {
	/* clang-format on */
	const size_t ec_avg = (ubi->ec_count > 0) ? (ubi->ec_sum / ubi->ec_count) : 0;
	struct ubi_list_item *bad = ubi_leaf_as_list(new_item);

	ubi_move_to_bad_blocks(ubi, new_pnum, ec_avg, bad);
	LOG_ERR("Anchor rewrite failed, PEB %zu marked bad", new_pnum);
	return -EIO;
}
}

/**
 * \brief Attempt to recover bad PEBs by performing erase-only torture.
 *
 * Up to CONFIG_UBI_BAD_PEB_TORTURE_CYCLES bad PEBs are tested per call.
 * Each PEB is erased up to CONFIG_UBI_BAD_PEB_TORTURE_MAX_PER_ERASE times;
 * the first successful erase recovers the PEB to the free pool with ec = ec_avg.
 * If all attempts fail, the PEB remains in the bad list.
 */
static void torture_bad_blocks(struct ubi_device *ubi)
{
	const struct flash_area *fa = NULL;
	int ret = flash_area_open(ubi->mtd.partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failure during torture");
		return;
	}

	size_t tortured = 0;

	sys_snode_t *prev = NULL;
	struct ubi_list_item *item = NULL;
	struct ubi_list_item *next = NULL;

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&ubi->bad_pebs, item, next, node)
	{
		if (tortured >= CONFIG_UBI_BAD_PEB_TORTURE_CYCLES) {
			break;
		}

		const size_t offset = item->pnum * ubi->mtd.erase_block_size;
		bool passed = false;

		for (size_t i = 0; i < CONFIG_UBI_BAD_PEB_TORTURE_MAX_PER_ERASE; ++i) {
			ret = flash_area_erase(fa, offset, ubi->mtd.erase_block_size);

			if (ret == 0) {
				passed = true;
				break;
			}
		}

		if (passed) {
			const size_t ec_avg = (ubi->ec_count > 0) ? (ubi->ec_sum / ubi->ec_count) :
								    0;

			struct ubi_ec_hdr ec_hdr = { 0 };

			ec_hdr.magic = UBI_EC_HDR_MAGIC;
			ec_hdr.version = UBI_EC_HDR_VERSION;
			ec_hdr.ec = ec_avg;

			const uint8_t write_kv =
				ubi->crypto_cfg->policy.requested_write_key_version;

			ret = ubi_secure_ec_hdr_write(&ubi->mtd, ubi->crypto_cfg, item->pnum,
						      &ec_hdr, write_kv, 0);

			if (ret != 0) {
				LOG_WRN("Torture passed but EC write failed for PEB %u",
					item->pnum);
				prev = &item->node;
				tortured += 1;
				continue;
			}

			const size_t recovered_pnum = item->pnum;

			sys_slist_remove(&ubi->bad_pebs, prev, &item->node);
			ubi->bad_peb_count -= 1;

			struct ubi_rbt_item *free_item = ubi_leaf_as_rbt(item);

			free_item->key = ec_avg;
			free_item->value.pnum = recovered_pnum;
			rb_insert(&ubi->free_pebs, &free_item->node);
			ubi->free_peb_count += 1;

			ubi->ec_sum += ec_avg;
			ubi->ec_count += 1;

			LOG_INF("Torture recovered PEB %u", free_item->value.pnum);
		} else {
			prev = &item->node;
		}

		tortured += 1;
	}

	flash_area_close(fa);
}

/* Module interface function definitions ------------------------------------------------------- */

int ubi_secure_device_get_info(struct ubi_device *ubi, struct ubi_device_info *info)
{
	if (ubi == NULL || info == NULL) {
		LOG_ERR("secure_get_info: NULL argument");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	memset(info, 0, sizeof(*info));

	info->read_only_degraded = ubi->read_only_degraded;
	info->total_peb_count = ubi->total_data_peb_count;
	info->leb_size = ubi->leb_size;

	info->free_peb_count = ubi->free_peb_count;
	info->dirty_peb_count = ubi->dirty_peb_count;
	info->bad_peb_count = ubi->bad_peb_count;
	info->ec_avg = (ubi->ec_count > 0) ? (ubi->ec_sum / ubi->ec_count) : 0;

	info->reserved_peb_count = ubi_reserved_peb_count(ubi);
	info->volume_count = ubi->vol_count;

	k_mutex_unlock(&ubi->mutex);
	return 0;
}

int ubi_secure_device_erase_peb(struct ubi_device *ubi)
{
	if (ubi == NULL) {
		LOG_ERR("secure_erase_peb: NULL argument");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	int ret = ubi_mutation_allowed(ubi, UBI_MUT_MAINTENANCE);

	if (ret != 0) {
		LOG_ERR("Mutation blocked: maintenance operations not allowed");
		goto exit;
	}

	if (ubi->dirty_peb_count > 0) {
		struct rbnode *node = rb_get_min(&ubi->dirty_pebs);
		struct ubi_rbt_item *entry = CONTAINER_OF(node, struct ubi_rbt_item, node);

		/* Check if dirty PEB is last writable witness.
		 * If so and no free PEB for anchor rewrite, find a safe
		 * non-witness dirty PEB to erase first (creates a free PEB
		 * for the witness rewrite on the next call). */
		ret = maybe_rewrite_anchor_for_dirty(ubi, entry->value.pnum);
		if (ret == -ENOSPC) {
			struct ubi_rbt_item *alt = NULL;

			RB_FOR_EACH_CONTAINER(&ubi->dirty_pebs, alt, node)
			{
				if (alt == entry) {
					continue;
				}
				if (maybe_rewrite_anchor_for_dirty(ubi, alt->value.pnum) == 0) {
					entry = alt;
					ret = 0;
					break;
				}
			}
			if (ret != 0) {
				LOG_ERR("No safe dirty PEB to erase");
				goto exit;
			}
		} else if (ret != 0) {
			LOG_ERR("Anchor witness check/rewrite failed");
			goto exit;
		}

		ret = erase_dirty_entry(ubi, entry);
	}

	if (ret == 0) {
		ubi_secure_maybe_sync_freshness(ubi);
	}

exit:
	if (ubi->bad_peb_count > 0) {
		torture_bad_blocks(ubi);
	}

	/* If the reserved PEB bank is degraded, attempt recovery.
	 * Re-scan reserved PEBs — if all are now authenticated, clear the flag. */
	if (ubi->read_only_degraded) {
		struct ubi_secure_res_peb_scan rescan = { 0 };
		const int rc = ubi_secure_res_peb_scan(&ubi->mtd, ubi->crypto_cfg, &rescan);

		if (rc == 0 && rescan.auth_count >= UBI_SECURE_RES_PEB_NR_ACTIVE) {
			LOG_INF("Reserved PEB bank recovered, leaving degraded mode");
			ubi->read_only_degraded = false;
		}
	}

	k_mutex_unlock(&ubi->mutex);
	return ret;
}

void ubi_secure_try_refill_reserve(struct ubi_device *ubi)
{
	__ASSERT_NO_MSG(ubi != NULL);

	if (ubi->free_peb_count > 1 || ubi->dirty_peb_count == 0) {
		return;
	}

	/* The last free data PEB is
	 * reserved for hidden-anchor maintenance.  Attempt to erase one
	 * dirty PEB through the standard witness-safe path to push
	 * free_peb_count from 1 to 2, preserving the emergency reserve
	 * when the upcoming write consumes a PEB. */
	struct rbnode *node = rb_get_min(&ubi->dirty_pebs);
	struct ubi_rbt_item *entry = CONTAINER_OF(node, struct ubi_rbt_item, node);

	int ret = maybe_rewrite_anchor_for_dirty(ubi, entry->value.pnum);

	if (ret == -ENOSPC) {
		/* Witness; no free PEB for anchor rewrite — try another dirty PEB. */
		struct ubi_rbt_item *alt = NULL;

		RB_FOR_EACH_CONTAINER(&ubi->dirty_pebs, alt, node)
		{
			if (alt == entry) {
				continue;
			}
			if (maybe_rewrite_anchor_for_dirty(ubi, alt->value.pnum) == 0) {
				entry = alt;
				ret = 0;
				break;
			}
		}
	}

	if (ret != 0) {
		return;
	}

	(void)erase_dirty_entry(ubi, entry);
}

int ubi_secure_device_deinit(struct ubi_device *ubi)
{
	if (ubi == NULL) {
		LOG_ERR("secure_deinit: NULL argument");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	struct rbnode *node = NULL;
	struct ubi_rbt_item *rbt_item = NULL;

	struct ubi_list_item *list_item = NULL;
	struct ubi_list_item *list_next = NULL;

	while ((node = rb_get_min(&ubi->free_pebs))) {
		rbt_item = CONTAINER_OF(node, struct ubi_rbt_item, node);
		rb_remove(&ubi->free_pebs, &rbt_item->node);
		ubi_mem_leaf_free(rbt_item);
		ubi->free_peb_count--;
	}

	while ((node = rb_get_min(&ubi->dirty_pebs))) {
		rbt_item = CONTAINER_OF(node, struct ubi_rbt_item, node);
		rb_remove(&ubi->dirty_pebs, &rbt_item->node);
		ubi_mem_leaf_free(rbt_item);
		ubi->dirty_peb_count--;
	}

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&ubi->bad_pebs, list_item, list_next, node)
	{
		sys_slist_remove(&ubi->bad_pebs, NULL, &list_item->node);
		ubi_mem_leaf_free(list_item);
		ubi->bad_peb_count--;
	}

	while ((node = rb_get_min(&ubi->vols))) {
		rbt_item = CONTAINER_OF(node, struct ubi_rbt_item, node);
		rb_remove(&ubi->vols, &rbt_item->node);

		struct ubi_volume *vol = rbt_item->value.vol;

		while ((node = rb_get_min(&vol->eba_tbl))) {
			struct ubi_rbt_item *vol_item =
				CONTAINER_OF(node, struct ubi_rbt_item, node);
			rb_remove(&vol->eba_tbl, &vol_item->node);
			ubi_mem_leaf_free(vol_item);
			vol->eba_tbl_count--;
		}

		ubi_mem_volume_free(rbt_item->value.vol);
		ubi_mem_leaf_free(rbt_item);
		ubi->vol_count--;
	}

	ubi_partition_release(ubi->mtd.partition_id);
	ubi_mem_device_free(ubi);
	return 0;
}
