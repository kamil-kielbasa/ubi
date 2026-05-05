/**
 * \file    ubi_secure_leb.c
 * \author  Kamil Kielbasa
 * \brief   Secure backend LEB operations: write, read, map, unmap, is_mapped, get_size.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_secure_ops.h"
#include "ubi_secure_budget.h"
#include "ubi_secure_crypto.h"
#include "ubi_secure_event.h"
#include "ubi_secure_io.h"
#include "ubi_secure_types.h"
#include "ubi_internal.h"
#include "ubi_plain_io.h"
#include "ubi_mem.h"

/* Zephyr headers: */
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/crc.h>

/* Standard library headers: */
#include <errno.h>
#include <stdbool.h>

/* Module defines ------------------------------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Static function definitions ------------------------------------------------------------------ */

/**
 * \brief Mark a PEB that failed a write as bad.
 */
static void leb_mark_peb_bad(struct ubi_device *ubi, struct ubi_rbt_item *node)
{
	__ASSERT_NO_MSG(ubi != NULL);
	__ASSERT_NO_MSG(node != NULL);

	const size_t failed_pnum = node->value.pnum;
	const size_t failed_ec = node->key;

	struct ubi_list_item *bad_item = ubi_leaf_as_list(node);

	ubi->ec_sum -= failed_ec;
	ubi->ec_count--;
	ubi_move_to_bad_blocks(ubi, failed_pnum, failed_ec, bad_item);
}

/**
 * \brief Recover the previous VID secure metadata for an existing LEB mapping.
 *
 * If the LEB is already mapped, reads the full EC→VID auth chain from the old PEB
 * and returns the authenticated leb_write_counter and leb_total_auth_bytes.
 * For an unmapped LEB both are 0 (first write).
 */
static int leb_recover_old_counters(struct ubi_device *ubi, const struct ubi_volume *vol,
				    size_t lnum, uint64_t *old_write_counter,
				    uint64_t *old_total_auth_bytes)
{
	__ASSERT_NO_MSG(ubi != NULL);
	__ASSERT_NO_MSG(vol != NULL);
	__ASSERT_NO_MSG(old_write_counter != NULL);
	__ASSERT_NO_MSG(old_total_auth_bytes != NULL);

	const struct ubi_rbt_item *existing =
		ubi_cache_search((struct rbtree *)&vol->eba_tbl, lnum);

	if (!existing) {
		*old_write_counter = 0;
		*old_total_auth_bytes = 0;
		return 0;
	}

	struct ubi_ec_hdr ec_hdr = { 0 };
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };

	int ret = ubi_secure_ec_hdr_read(&ubi->flash, ubi->crypto_cfg, existing->value.pnum,
					 &ec_hdr, &ec_ctx);
	if (ret != 0) {
		LOG_ERR("EC header read for old LEB counter recovery failed");
		return ret;
	}

	struct ubi_vid_hdr vid_hdr = { 0 };
	struct ubi_vid_secure_meta vid_meta = { 0 };
	struct ubi_secure_vid_auth_ctx vid_ctx = { 0 };

	ret = ubi_secure_vid_hdr_read(&ubi->flash, ubi->crypto_cfg, existing->value.pnum, &ec_ctx,
				      &vid_hdr, &vid_meta, &vid_ctx);
	if (ret != 0) {
		LOG_ERR("VID header read for old LEB counter recovery failed");
		return ret;
	}

	*old_write_counter = vid_meta.leb_write_counter;
	*old_total_auth_bytes = vid_meta.leb_total_auth_bytes;
	return 0;
}

/**
 * \brief Allocate a free PEB, write optional data payload, then write VID header.
 *
 * Write order: LEB data first, VID second (commit point).
 * counter_base = old leb_write_counter. LEB data uses counter_base.
 * VID gets leb_write_counter = counter_base + aead_invocations (1 for single-tag).
 * VID gets leb_total_auth_bytes = old + leb_aad_bytes_this_write + payload_bytes.
 */
static int leb_prepare_new_mapping(struct ubi_device *ubi, struct ubi_volume *vol, size_t lnum,
				   const void *buf, size_t len, uint64_t old_write_counter,
				   uint64_t old_total_auth_bytes,
				   struct ubi_rbt_item **out_new_node)
{
	__ASSERT_NO_MSG(ubi != NULL);
	__ASSERT_NO_MSG(vol != NULL);
	__ASSERT_NO_MSG(out_new_node != NULL);

	/* Pre-write budget + nonce-overflow check.
	 * Reject BEFORE any flash mutation. */
	/* Compute AEAD invocations and auth bytes for this write. */
#if defined(CONFIG_UBI_CRYPTO_LEB_CHUNKED)
	const size_t chunk_size = CONFIG_UBI_CRYPTO_LEB_CHUNK_SIZE;
	const uint32_t aead_invocations =
		(len > 0) ? (uint32_t)((len + chunk_size - 1) / chunk_size) : 1;
	const uint64_t leb_auth_bytes_this_write =
		(len > 0) ? ((uint64_t)len +
			     (uint64_t)aead_invocations * UBI_SECURE_LEB_CHUNK_AAD_SIZE) :
			    (uint64_t)UBI_SECURE_LEB_AAD_SIZE;
#else /* !CONFIG_UBI_CRYPTO_LEB_CHUNKED */
	const uint32_t aead_invocations = 1;
	const uint64_t leb_auth_bytes_this_write = (uint64_t)UBI_SECURE_LEB_AAD_SIZE + len;
#endif /* CONFIG_UBI_CRYPTO_LEB_CHUNKED */

	const uint64_t projected_counter = old_write_counter + aead_invocations;
	const uint64_t projected_bytes = old_total_auth_bytes + leb_auth_bytes_this_write;
	const uint8_t write_kv = ubi->crypto_cfg->policy.requested_write_key_version;

	if (projected_counter > UBI_SECURE_COUNTER_MAX) {
		LOG_ERR("LEB AEAD counter would overflow (kv=%u vol_id=%d)", (unsigned)write_kv,
			vol->vol_id);
		struct ubi_crypto_event ev = {
			.type = UBI_CRYPTO_EVENT_KEY_ROTATE_NOW,
			.freshness = ubi_secure_freshness_snapshot(ubi),
			.rotation = { .key_version = write_kv,
				      .volume_id = (uint32_t)vol->vol_id,
				      .usage_pct = 100 },
		};
		ubi_secure_emit_event(ubi, &ev);
		return -EOVERFLOW;
	}

	int ret = ubi_secure_budget_leb_pre(ubi, write_kv, (uint32_t)vol->vol_id, projected_counter,
					    projected_bytes);
	if (ret != 0) {
		LOG_ERR("LEB-domain budget rejected write: vol_id=%d lnum=%zu", vol->vol_id, lnum);
		return ret;
	}

	ret = ubi_secure_budget_metadata_pre(ubi, UBI_SECURE_DOMAIN_VOLUME_IDENTIFIER,
					     ubi->next_vid_counter + 1, write_kv, 0);
	if (ret != 0) {
		LOG_ERR("VID-domain budget rejected write: vol_id=%d lnum=%zu", vol->vol_id, lnum);
		return ret;
	}

	struct rbnode *min_rbnode = rb_get_min(&ubi->free_pebs);
	struct ubi_rbt_item *new_node = CONTAINER_OF(min_rbnode, struct ubi_rbt_item, node);

	rb_remove(&ubi->free_pebs, &new_node->node);
	ubi->free_peb_count--;

	/* Read authentic EC context from the free PEB (needed for chained AAD). */
	struct ubi_ec_hdr ec_hdr = { 0 };
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };

	ret = ubi_secure_ec_hdr_read(&ubi->flash, ubi->crypto_cfg, new_node->value.pnum, &ec_hdr,
				     &ec_ctx);
	if (ret != 0) {
		LOG_ERR("EC header read failure on free PEB %zu", new_node->value.pnum);
		leb_mark_peb_bad(ubi, new_node);
		return ret;
	}

	/* Prepare VID header in RAM. */
	struct ubi_vid_hdr vid_hdr = { 0 };

	vid_hdr.magic = UBI_VID_HDR_MAGIC;
	vid_hdr.version = UBI_VID_HDR_VERSION;
	vid_hdr.lnum = lnum;
	vid_hdr.vol_id = vol->vol_id;
	vid_hdr.sqnum = ubi->global_sqnum++;
	vid_hdr.data_size = len;
	vid_hdr.hdr_crc =
		crc32_ieee((const uint8_t *)&vid_hdr, sizeof(vid_hdr) - sizeof(vid_hdr.hdr_crc));

	/*
	 * Write the new PEB:
	 *   counter_base = old_write_counter (next unused AEAD counter).
	 *   aead_invocations computed above (1 for single-tag, chunk_count for chunked).
	 *   LEB data written starting at counter_base.
	 *   New VID gets leb_write_counter = counter_base + aead_invocations.
	 *   New VID gets leb_total_auth_bytes = old_total + leb_auth_bytes_this_write.
	 */
	const uint64_t counter_base = old_write_counter;

	const struct ubi_vid_secure_meta vid_meta = {
		.leb_write_counter = counter_base + aead_invocations,
		.leb_total_auth_bytes = old_total_auth_bytes + leb_auth_bytes_this_write,
	};

	/* Step 1: Write LEB data payload first (if any). */
	if (buf != NULL && len > 0) {
#if defined(CONFIG_UBI_CRYPTO_LEB_CHUNKED)
		ret = ubi_secure_leb_data_write_chunked(&ubi->flash, ubi->crypto_cfg,
							new_node->value.pnum, &ec_ctx, &vid_hdr,
							write_kv, buf, len, write_kv, counter_base);
#else /* !CONFIG_UBI_CRYPTO_LEB_CHUNKED */
		ret = ubi_secure_leb_data_write(&ubi->flash, ubi->crypto_cfg, new_node->value.pnum,
						&ec_ctx, &vid_hdr, write_kv, buf, len, write_kv,
						counter_base);
#endif /* CONFIG_UBI_CRYPTO_LEB_CHUNKED */
		if (ret != 0) {
			LOG_ERR("LEB data write failure");
			ubi_secure_handle_write_error(ubi, ret, new_node->value.pnum);
			leb_mark_peb_bad(ubi, new_node);
			return ret;
		}
	}

	/* Step 2: Write VID header — this is the commit point.
	 * VID counter = global vid_next, independent of per-LEB counter. */
	const uint64_t vid_counter = ubi->next_vid_counter;

	ret = ubi_secure_vid_hdr_write(&ubi->flash, ubi->crypto_cfg, new_node->value.pnum, &ec_ctx,
				       &vid_hdr, &vid_meta, write_kv, vid_counter);
	if (ret != 0) {
		LOG_ERR("VID header write failure");
		ubi_secure_handle_write_error(ubi, ret, new_node->value.pnum);
		leb_mark_peb_bad(ubi, new_node);
		return ret;
	}

	ubi->next_vid_counter = vid_counter + 1;

	/* Track VID+LEB objects for key-version refcount. */
	ubi_secure_key_refcount_inc(ubi, write_kv);
	ubi_secure_key_refcount_inc(ubi, write_kv);

	ubi_secure_budget_leb_post(ubi, write_kv, (uint32_t)vol->vol_id, vid_meta.leb_write_counter,
				   vid_meta.leb_total_auth_bytes);

	ubi_secure_budget_metadata_post(ubi, UBI_SECURE_DOMAIN_VOLUME_IDENTIFIER,
					ubi->next_vid_counter, write_kv, 0);

	*out_new_node = new_node;
	return 0;
}

/**
 * \brief Swap old EBA entry for the newly written PEB.
 */
static void leb_commit_mapping_swap(struct ubi_device *ubi, struct ubi_volume *vol, size_t lnum,
				    struct ubi_rbt_item *new_node)
{
	__ASSERT_NO_MSG(ubi != NULL);
	__ASSERT_NO_MSG(vol != NULL);
	__ASSERT_NO_MSG(new_node != NULL);

	struct ubi_rbt_item *old_entry = ubi_cache_search(&vol->eba_tbl, lnum);

	if (old_entry) {
		struct ubi_ec_hdr old_ec = { 0 };
		struct ubi_secure_ec_auth_ctx old_ec_ctx = { 0 };

		const int ec_ret = ubi_secure_ec_hdr_read(
			&ubi->flash, ubi->crypto_cfg, old_entry->value.pnum, &old_ec, &old_ec_ctx);

		rb_remove(&vol->eba_tbl, &old_entry->node);
		vol->eba_tbl_count--;

		old_entry->key = (ec_ret == 0) ? old_ec.ec : 0;
		rb_insert(&ubi->dirty_pebs, &old_entry->node);
		ubi->dirty_peb_count++;
	}

	new_node->key = lnum;
	rb_insert(&vol->eba_tbl, &new_node->node);
	vol->eba_tbl_count++;
}

/* Module interface function definitions -------------------------------------------------------- */

int ubi_secure_leb_write(struct ubi_device *ubi, int vol_id, size_t lnum, const void *buf,
			 size_t len)
{
	if (ubi == NULL) {
		LOG_ERR("secure_leb_write: NULL argument");
		return -EINVAL;
	}

	if ((buf != NULL && len == 0) || (buf == NULL && len > 0)) {
		LOG_ERR("secure_leb_write: buf/len mismatch");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	int ret = ubi_mutation_allowed(ubi, UBI_MUT_DATA_PATH);

	if (ret != 0) {
		LOG_ERR("Mutation blocked: data-path writes not allowed");
		goto exit;
	}

	struct ubi_volume *vol = ubi_find_volume(ubi, vol_id);

	if (!vol) {
		LOG_ERR("Volume %d not found", vol_id);
		ret = -ENOENT;
		goto exit;
	}

	if (lnum >= vol->cfg.leb_count) {
		LOG_ERR("Volume LEB limit exceeded");
		ret = -EACCES;
		goto exit;
	}

	/* Preserve emergency free-PEB reserve. */
	ubi_secure_try_refill_reserve(ubi);

	if (ubi->free_peb_count == 0) {
		LOG_ERR("Lack of free PEBs");
		ret = -ENOSPC;
		goto exit;
	}

	if (len > ubi->leb_size) {
		LOG_ERR("Too big buffer to write in LEB");
		ret = -ENOSPC;
		goto exit;
	}

	/* Recover monotonic counter state from old mapping (if any). */
	uint64_t old_wc = 0;
	uint64_t old_tab = 0;

	ret = leb_recover_old_counters(ubi, vol, lnum, &old_wc, &old_tab);
	if (ret != 0) {
		LOG_ERR("LEB counter recovery failure");
		goto exit;
	}

	struct ubi_rbt_item *new_node = NULL;

	ret = leb_prepare_new_mapping(ubi, vol, lnum, buf, len, old_wc, old_tab, &new_node);
	if (ret != 0) {
		goto exit;
	}

	leb_commit_mapping_swap(ubi, vol, lnum, new_node);

	ubi_secure_maybe_sync_freshness(ubi);

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

int ubi_secure_leb_read(struct ubi_device *ubi, int vol_id, size_t lnum, size_t offset, void *buf,
			size_t len)
{
	if (ubi == NULL || buf == NULL) {
		LOG_ERR("secure_leb_read: NULL argument");
		return -EINVAL;
	}

	int ret = -EIO;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	const struct ubi_volume *vol = ubi_find_volume(ubi, vol_id);

	if (!vol) {
		LOG_ERR("Volume %d not found", vol_id);
		ret = -ENOENT;
		goto exit;
	}

	if (lnum >= vol->cfg.leb_count) {
		LOG_ERR("Volume LEB limit exceeded");
		ret = -EACCES;
		goto exit;
	}

	const struct ubi_rbt_item *entry = ubi_cache_search((struct rbtree *)&vol->eba_tbl, lnum);

	if (!entry) {
		LOG_ERR("LEB not found");
		ret = -ENOENT;
		goto exit;
	}

	/* Read and authenticate EC header. */
	struct ubi_ec_hdr ec_hdr = { 0 };
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };

	ret = ubi_secure_ec_hdr_read(&ubi->flash, ubi->crypto_cfg, entry->value.pnum, &ec_hdr,
				     &ec_ctx);
	if (ret != 0) {
		LOG_ERR("EC header read failure");
		ubi_secure_handle_read_error(ubi, ret, entry->value.pnum,
					     UBI_SECURE_DOMAIN_ERASE_COUNTER, ec_ctx.key_version);
		goto exit;
	}

	/* Check EC key version against allowlist. */
	if (!ubi_secure_check_allowlist(ubi, ec_ctx.key_version, entry->value.pnum)) {
		LOG_ERR("Key version not in allowlist");
		ret = -EACCES;
		goto exit;
	}

	/* Read and authenticate VID header. */
	struct ubi_vid_hdr vid_hdr = { 0 };
	struct ubi_vid_secure_meta vid_meta = { 0 };
	struct ubi_secure_vid_auth_ctx vid_ctx = { 0 };

	ret = ubi_secure_vid_hdr_read(&ubi->flash, ubi->crypto_cfg, entry->value.pnum, &ec_ctx,
				      &vid_hdr, &vid_meta, &vid_ctx);
	if (ret != 0) {
		LOG_ERR("VID header read failure");
		ubi_secure_handle_read_error(ubi, ret, entry->value.pnum,
					     UBI_SECURE_DOMAIN_VOLUME_IDENTIFIER,
					     vid_ctx.key_version);
		goto exit;
	}

	/* Check VID key version against allowlist. */
	if (!ubi_secure_check_allowlist(ubi, vid_ctx.key_version, entry->value.pnum)) {
		LOG_ERR("Key version not in allowlist");
		ret = -EACCES;
		goto exit;
	}

	/* Validate read range. */
	if ((offset + len) > vid_hdr.data_size) {
		LOG_ERR("Read beyond data_size: offset=%zu len=%zu data_size=%u", offset, len,
			vid_hdr.data_size);
		ret = -EINVAL;
		goto exit;
	}

	/* Read and authenticate LEB data. */
#if defined(CONFIG_UBI_CRYPTO_LEB_CHUNKED)
	ret = ubi_secure_leb_data_read_chunked(&ubi->flash, ubi->crypto_cfg, entry->value.pnum,
					       &vid_ctx, offset, buf, len);
#else /* !CONFIG_UBI_CRYPTO_LEB_CHUNKED */
	ret = ubi_secure_leb_data_read(&ubi->flash, ubi->crypto_cfg, entry->value.pnum, &vid_ctx,
				       offset, buf, len);
#endif /* CONFIG_UBI_CRYPTO_LEB_CHUNKED */
	if (ret != 0) {
		LOG_ERR("LEB data read failure");
		ubi_secure_handle_read_error(ubi, ret, entry->value.pnum, UBI_SECURE_DOMAIN_LEB,
					     vid_ctx.key_version);
		goto exit;
	}

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

int ubi_secure_leb_map(struct ubi_device *ubi, int vol_id, size_t lnum)
{
	if (ubi == NULL) {
		LOG_ERR("secure_leb_map: NULL argument");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	int ret = ubi_mutation_allowed(ubi, UBI_MUT_DATA_PATH);

	if (ret != 0) {
		LOG_ERR("Mutation blocked: data-path writes not allowed");
		goto exit;
	}

	struct ubi_volume *vol = ubi_find_volume(ubi, vol_id);

	if (!vol) {
		LOG_ERR("Volume %d not found", vol_id);
		ret = -ENOENT;
		goto exit;
	}

	if (lnum >= vol->cfg.leb_count) {
		LOG_ERR("Volume LEB limit exceeded");
		ret = -EACCES;
		goto exit;
	}

	if (ubi_cache_search(&vol->eba_tbl, lnum)) {
		ret = 0;
		goto exit;
	}

	/* Preserve emergency free-PEB reserve. */
	ubi_secure_try_refill_reserve(ubi);

	if (ubi->free_peb_count == 0) {
		LOG_ERR("Lack of free PEBs");
		ret = -ENOSPC;
		goto exit;
	}

	/* Map is a zero-length write — counters start at 0 (no existing mapping). */
	struct ubi_rbt_item *new_node = NULL;

	ret = leb_prepare_new_mapping(ubi, vol, lnum, NULL, 0, 0, 0, &new_node);
	if (ret != 0) {
		goto exit;
	}

	leb_commit_mapping_swap(ubi, vol, lnum, new_node);

	ubi_secure_maybe_sync_freshness(ubi);

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

int ubi_secure_leb_unmap(struct ubi_device *ubi, int vol_id, size_t lnum)
{
	if (ubi == NULL) {
		LOG_ERR("secure_leb_unmap: NULL argument");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	int ret = ubi_mutation_allowed(ubi, UBI_MUT_DATA_PATH);

	if (ret != 0) {
		LOG_ERR("Mutation blocked: data-path writes not allowed");
		goto exit;
	}

	struct ubi_volume *vol = ubi_find_volume(ubi, vol_id);

	if (!vol) {
		LOG_ERR("Volume %d not found", vol_id);
		ret = -ENOENT;
		goto exit;
	}

	if (lnum >= vol->cfg.leb_count) {
		LOG_ERR("Volume LEB limit exceeded");
		ret = -EACCES;
		goto exit;
	}

	struct ubi_rbt_item *entry = ubi_cache_search(&vol->eba_tbl, lnum);

	if (!entry) {
		ret = 0;
		goto exit;
	}

	/* Read EC to get erase counter for dirty pool key. */
	struct ubi_ec_hdr ec_hdr = { 0 };
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };

	ret = ubi_secure_ec_hdr_read(&ubi->flash, ubi->crypto_cfg, entry->value.pnum, &ec_hdr,
				     &ec_ctx);
	if (ret != 0) {
		LOG_ERR("EC header read failure");
		goto exit;
	}

	rb_remove(&vol->eba_tbl, &entry->node);
	vol->eba_tbl_count--;

	entry->key = ec_hdr.ec;
	rb_insert(&ubi->dirty_pebs, &entry->node);
	ubi->dirty_peb_count++;

	ret = 0;

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

int ubi_secure_leb_is_mapped(struct ubi_device *ubi, int vol_id, size_t lnum, bool *is_mapped)
{
	if (ubi == NULL || is_mapped == NULL) {
		LOG_ERR("secure_leb_is_mapped: NULL argument");
		return -EINVAL;
	}

	int ret = -EIO;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	const struct ubi_volume *vol = ubi_find_volume(ubi, vol_id);

	if (!vol) {
		LOG_ERR("Volume %d not found", vol_id);
		ret = -ENOENT;
		goto exit;
	}

	if (lnum >= vol->cfg.leb_count) {
		LOG_ERR("Volume LEB limit exceeded");
		ret = -EACCES;
		goto exit;
	}

	const struct ubi_rbt_item *entry = ubi_cache_search((struct rbtree *)&vol->eba_tbl, lnum);

	*is_mapped = (entry != NULL);
	ret = 0;

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

int ubi_secure_leb_get_size(struct ubi_device *ubi, int vol_id, size_t lnum, size_t *size)
{
	if (ubi == NULL || size == NULL) {
		LOG_ERR("secure_leb_get_size: NULL argument");
		return -EINVAL;
	}

	int ret = -EIO;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	const struct ubi_volume *vol = ubi_find_volume(ubi, vol_id);

	if (!vol) {
		LOG_ERR("Volume %d not found", vol_id);
		ret = -ENOENT;
		goto exit;
	}

	if (lnum >= vol->cfg.leb_count) {
		LOG_ERR("Volume LEB limit exceeded");
		ret = -EACCES;
		goto exit;
	}

	const struct ubi_rbt_item *entry = ubi_cache_search((struct rbtree *)&vol->eba_tbl, lnum);

	if (!entry) {
		LOG_ERR("LEB %zu in volume %d is not mapped", lnum, vol_id);
		ret = -ENOENT;
		goto exit;
	}

	/* Read EC and VID to get authenticated data_size. */
	struct ubi_ec_hdr ec_hdr = { 0 };
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };

	ret = ubi_secure_ec_hdr_read(&ubi->flash, ubi->crypto_cfg, entry->value.pnum, &ec_hdr,
				     &ec_ctx);
	if (ret != 0) {
		LOG_ERR("EC header read failure");
		goto exit;
	}

	struct ubi_vid_hdr vid_hdr = { 0 };
	struct ubi_vid_secure_meta vid_meta = { 0 };
	struct ubi_secure_vid_auth_ctx vid_ctx = { 0 };

	ret = ubi_secure_vid_hdr_read(&ubi->flash, ubi->crypto_cfg, entry->value.pnum, &ec_ctx,
				      &vid_hdr, &vid_meta, &vid_ctx);
	if (ret != 0) {
		LOG_ERR("VID header read failure");
		goto exit;
	}

	*size = vid_hdr.data_size;

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}
