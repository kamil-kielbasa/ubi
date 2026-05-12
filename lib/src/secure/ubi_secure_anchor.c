/**
 * \file    ubi_secure_anchor.c
 * \author  Kamil Kielbasa
 * \brief   Hidden per-volume anchor PEB lifecycle for the secure backend.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_secure_anchor.h"
#include "ubi_secure_event.h"
#include "ubi_secure_io.h"
#include "ubi_secure_types.h"
#include "ubi_internal.h"
#include "ubi_mem.h"

/* Zephyr headers: */
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/crc.h>

/* Standard library headers: */
#include <errno.h>
#include <stdint.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Module interface function definitions -------------------------------------------------------- */

int ubi_secure_anchor_create(struct ubi_device *ubi, struct ubi_volume *vol)
{
	if (!ubi || !vol) {
		LOG_ERR("Invalid argument: ubi=%p vol=%p", (const void *)ubi, (const void *)vol);
		return -EINVAL;
	}

	if (ubi->free_pool.count == 0) {
		LOG_ERR("No free PEB for anchor allocation");
		return -ENOSPC;
	}

	/* 1. Take a free PEB. */
	struct rbnode *min_node = rb_get_min(&ubi->free_pool.tree);
	struct ubi_rbt_item *item = CONTAINER_OF(min_node, struct ubi_rbt_item, node);

	rb_remove(&ubi->free_pool.tree, &item->node);
	ubi->free_pool.count -= 1;

	const size_t pnum = item->value.pnum;

	/* 2. Read authentic EC context from the PEB. */
	struct ubi_ec_hdr ec_hdr = { 0 };
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };

	int ret = ubi_secure_ec_hdr_read(&ubi->flash, ubi->crypto_cfg, pnum, &ec_hdr, &ec_ctx);

	if (ret != 0) {
		LOG_ERR("EC read failure on anchor PEB %zu", pnum);
		goto mark_bad;
	}

	/* 3. Build VID header with INTERNAL_ANCHOR_LNUM and zero-length data. */
	struct ubi_vid_hdr vid_hdr = { 0 };

	vid_hdr.magic = UBI_VID_HDR_MAGIC;
	vid_hdr.version = UBI_VID_HDR_VERSION;
	vid_hdr.lnum = UBI_SECURE_INTERNAL_ANCHOR_LNUM;
	vid_hdr.vol_id = vol->vol_id;
	vid_hdr.sqnum = ubi->global_sqnum++;
	vid_hdr.data_size = 0;
	vid_hdr.hdr_crc =
		crc32_ieee((const uint8_t *)&vid_hdr, sizeof(vid_hdr) - sizeof(vid_hdr.hdr_crc));

	/*
	 * Initial counter state for this anchor:
	 *   leb_write_counter = 1 (one AEAD invocation for the zero-length LEB record).
	 *   leb_total_auth_bytes = UBI_SECURE_LEB_AAD_SIZE (AAD only, zero payload).
	 */
	const struct ubi_vid_secure_meta vid_meta = {
		.leb_write_counter = 1,
		.leb_total_auth_bytes = UBI_SECURE_LEB_AAD_SIZE,
	};

	const uint8_t write_kv = ubi->crypto_cfg->policy.requested_write_key_version;

	/* 4. Write zero-length LEB data (prefix32 + tag16, no payload). */
	ret = ubi_secure_leb_data_write(&ubi->flash, ubi->crypto_cfg, pnum, &ec_ctx, &vid_hdr,
					write_kv, NULL, 0, write_kv, 0);
	if (ret != 0) {
		LOG_ERR("Anchor LEB write failure on PEB %zu", pnum);
		goto mark_bad;
	}

	/* 5. Write VID header — commit point.
	 *    Use global VID counter for this key version. */
	const uint64_t vid_counter = ubi->aead.next_vid;

	if (vid_counter > UBI_SECURE_COUNTER_MAX) {
		LOG_ERR("VID counter overflow");
		const struct ubi_crypto_event event = {
			.type = UBI_CRYPTO_EVENT_KEY_ROTATE_NOW,
			.freshness = ubi_secure_freshness_get_snapshot(ubi),
			.rotation = { .key_version = write_kv,
				      .usage_pct = UBI_SECURE_PERCENT_BASE },
		};
		ubi_secure_event_emit(ubi, &event);
		ret = -EOVERFLOW;
		goto mark_bad;
	}

	ret = ubi_secure_vid_hdr_write(&ubi->flash, ubi->crypto_cfg, pnum, &ec_ctx, &vid_hdr,
				       &vid_meta, write_kv, vid_counter);
	if (ret != 0) {
		LOG_ERR("Anchor VID write failure on PEB %zu", pnum);
		goto mark_bad;
	}

	ubi->aead.next_vid = vid_counter + 1;

	/* 6. Success — track in volume. The item is not inserted into any tree;
	 *    anchor PEBs are tracked via vol->anchor_pnum, not via EBA or free/dirty. */
	vol->anchor_pnum = pnum;
	ubi_volume_observe_counters(vol, vid_meta.leb_write_counter, vid_meta.leb_total_auth_bytes);
	ubi_mem_leaf_free(item);
	/* clang-format off */
	return 0;

mark_bad: {
	/* clang-format on */
	const size_t ec_avg = (ubi->ec_count > 0) ? (ubi->ec_sum / ubi->ec_count) : 0;
	struct ubi_list_item *bad = ubi_leaf_as_list(item);

	ubi_move_to_bad_blocks(ubi, pnum, ec_avg, bad);
	return ret;
}
}

int ubi_secure_anchor_rewrite_for_dirty_witness(struct ubi_device *ubi, size_t dirty_pnum)
{
	__ASSERT_NO_MSG(ubi != NULL);

	/* 1. Read the dirty PEB's EC and VID headers.  This is the only
	 *    flash read on the hot path; the two pieces of information
	 *    extracted from it are:
	 *      - which volume the PEB belongs to (vid_hdr.vol_id),
	 *      - the counter state authenticated on it (vid_meta).
	 */
	struct ubi_ec_hdr ec_hdr = { 0 };
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };

	int ret =
		ubi_secure_ec_hdr_read(&ubi->flash, ubi->crypto_cfg, dirty_pnum, &ec_hdr, &ec_ctx);

	if (ret != 0) {
		/* Cannot read EC -- PEB might already be partially erased.
		 * No witness data is at risk. */
		return 0;
	}

	struct ubi_vid_hdr vid_hdr = { 0 };
	struct ubi_vid_secure_meta vid_meta = { 0 };
	struct ubi_secure_vid_auth_ctx vid_ctx = { 0 };

	ret = ubi_secure_vid_hdr_read(&ubi->flash, ubi->crypto_cfg, dirty_pnum, &ec_ctx, &vid_hdr,
				      &vid_meta, &vid_ctx);
	if (ret != 0) {
		/* VID unreadable -- no counter state to protect. */
		return 0;
	}

	/* 2. Locate the owning volume. */
	struct ubi_rbt_item *vol_entry = ubi_cache_search(&ubi->vols, vid_hdr.vol_id);

	if (vol_entry == NULL) {
		/* Orphan PEB -- volume was removed.  Counters of removed volumes
		 * are not preserved across re-creation (re-creation is a key
		 * rotation event by spec). */
		return 0;
	}

	struct ubi_volume *vol = vol_entry->value.vol;

	if (vol->anchor_pnum == SIZE_MAX) {
		/* No anchor for this volume -- nothing to protect. */
		return 0;
	}

	/* 3. Witness check (O(1) via RAM cache).
	 *
	 *    leb_write_counter is strict-monotonically increasing per write,
	 *    so AT MOST ONE on-flash PEB of this volume carries
	 *    vid_meta.leb_write_counter == vol->cached_leb_write_counter.
	 *    If the dirty PEB is that sole witness, erasing it would drop
	 *    the floor below the cache and break AEAD nonce-uniqueness for
	 *    a future cold attach.  Otherwise (dirty counter strictly less
	 *    than cache), the cache value is preserved by another live or
	 *    dirty PEB -- or by the anchor itself -- and the erase is safe.
	 *
	 *    Defensive: the dirty counter must never EXCEED the cache (cache
	 *    is the strict upper bound).  If it does, the cache invariant
	 *    has been violated; assert in debug, fall back conservatively
	 *    (force rewrite) in release. */
	__ASSERT_NO_MSG(vid_meta.leb_write_counter <= vol->cached_leb_write_counter);

	if (vid_meta.leb_write_counter < vol->cached_leb_write_counter) {
		/* Not the sole witness -- cache floor is preserved elsewhere. */
		return 0;
	}

	if (vid_meta.leb_write_counter > vol->cached_leb_write_counter) {
		LOG_ERR("Cache invariant violated for vol %zu: dirty=%llu cache=%llu -- "
			"forcing anchor rewrite",
			vol->vol_id, (unsigned long long)vid_meta.leb_write_counter,
			(unsigned long long)vol->cached_leb_write_counter);
	}

	/* 4. Dirty PEB is the sole on-flash witness of the counter floor.
	 *    Rewrite the anchor to a fresh PEB before allowing the erase. */
	if (ubi->free_pool.count == 0) {
		LOG_ERR("No free PEB for anchor rewrite -- erase deferred");
		return -ENOSPC;
	}

	struct rbnode *min_node = rb_get_min(&ubi->free_pool.tree);
	struct ubi_rbt_item *new_item = CONTAINER_OF(min_node, struct ubi_rbt_item, node);

	rb_remove(&ubi->free_pool.tree, &new_item->node);
	ubi->free_pool.count -= 1;

	const size_t new_pnum = new_item->value.pnum;

	/* Read EC of the new free PEB. */
	struct ubi_ec_hdr new_ec = { 0 };
	struct ubi_secure_ec_auth_ctx new_ec_ctx = { 0 };

	ret = ubi_secure_ec_hdr_read(&ubi->flash, ubi->crypto_cfg, new_pnum, &new_ec, &new_ec_ctx);
	if (ret != 0) {
		LOG_ERR("EC read failure on anchor rewrite PEB %zu", new_pnum);
		goto rewrite_bad;
	}

	/* Build new anchor VID with inherited counter state derived from the
	 * cache (strict upper bound).  This consumes one AEAD invocation and
	 * AAD-sized bytes; advance the cache accordingly post-commit. */
	struct ubi_vid_hdr new_vid = { 0 };

	new_vid.magic = UBI_VID_HDR_MAGIC;
	new_vid.version = UBI_VID_HDR_VERSION;
	new_vid.lnum = UBI_SECURE_INTERNAL_ANCHOR_LNUM;
	new_vid.vol_id = vol->vol_id;
	new_vid.sqnum = ubi->global_sqnum++;
	new_vid.data_size = 0;
	new_vid.hdr_crc =
		crc32_ieee((const uint8_t *)&new_vid, sizeof(new_vid) - sizeof(new_vid.hdr_crc));

	const struct ubi_vid_secure_meta new_meta = {
		.leb_write_counter = vol->cached_leb_write_counter + 1,
		.leb_total_auth_bytes = vol->cached_leb_total_auth_bytes + UBI_SECURE_LEB_AAD_SIZE,
	};

	const uint8_t write_kv = ubi->crypto_cfg->policy.requested_write_key_version;

	/* Conservative cache bump BEFORE flash mutation (same rule as
	 * leb_prepare_new_mapping): if the write fails partway, counters
	 * stay burned and a retry uses strictly higher values. */
	ubi_volume_observe_counters(vol, new_meta.leb_write_counter, new_meta.leb_total_auth_bytes);

	/* Write zero-length LEB data. */
	ret = ubi_secure_leb_data_write(&ubi->flash, ubi->crypto_cfg, new_pnum, &new_ec_ctx,
					&new_vid, write_kv, NULL, 0, write_kv, 0);
	if (ret != 0) {
		LOG_ERR("Anchor rewrite LEB failure on PEB %zu", new_pnum);
		goto rewrite_bad;
	}

	/* Write VID header -- commit point. */
	const uint64_t vid_counter = ubi->aead.next_vid;

	ret = ubi_secure_vid_hdr_write(&ubi->flash, ubi->crypto_cfg, new_pnum, &new_ec_ctx,
				       &new_vid, &new_meta, write_kv, vid_counter);
	if (ret != 0) {
		LOG_ERR("Anchor rewrite VID failure on PEB %zu", new_pnum);
		goto rewrite_bad;
	}

	ubi->aead.next_vid = vid_counter + 1;

	/* Old anchor PEB is now stale -- retire to dirty pool. */
	const size_t old_anchor_pnum = vol->anchor_pnum;

	/* Re-read old anchor's EC for the dirty-tree key.  Best-effort: if it
	 * fails we still need to retire the PEB; use ec_avg as the key. */
	struct ubi_ec_hdr old_anchor_ec = { 0 };
	struct ubi_secure_ec_auth_ctx old_anchor_ec_ctx = { 0 };
	const int old_ec_ret = ubi_secure_ec_hdr_read(&ubi->flash, ubi->crypto_cfg, old_anchor_pnum,
						      &old_anchor_ec, &old_anchor_ec_ctx);

	vol->anchor_pnum = new_pnum;
	ubi_mem_leaf_free(new_item);

	struct ubi_rbt_item *old_item = NULL;

	ret = ubi_mem_leaf_alloc((void **)&old_item);
	if (ret != 0) {
		/* Leaf allocation failed -- old anchor PEB is lost.  Not critical
		 * because it's now stale and the new anchor is committed. */
		LOG_WRN("Leaf alloc failed for old anchor PEB %zu", old_anchor_pnum);
		return 0;
	}

	old_item->value.pnum = old_anchor_pnum;
	old_item->key = (old_ec_ret == 0) ?
				old_anchor_ec.ec :
				((ubi->ec_count > 0) ? (ubi->ec_sum / ubi->ec_count) : 0);
	rb_insert(&ubi->dirty_pool.tree, &old_item->node);
	ubi->dirty_pool.count += 1;

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
