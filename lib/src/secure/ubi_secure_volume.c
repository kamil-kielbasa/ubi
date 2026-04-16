/**
 * \file    ubi_secure_volume.c
 * \author  Kamil Kielbasa
 * \brief   Secure backend volume management: create, resize, remove, get_info.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files ------------------------------------------------------------------------------- */
#include "ubi_secure_ops.h"
#include "ubi_secure_reserved.h"
#include "ubi_secure_crypto.h"
#include "ubi_secure_event.h"
#include "ubi_secure_io.h"
#include "ubi_secure_types.h"
#include "ubi_internal.h"
#include "ubi_io.h"
#include "ubi_mem.h"

#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/crc.h>
#include <zephyr/storage/flash_map.h>

#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------ */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Static function definitions ----------------------------------------------------------------- */

/**
 * \brief Allocate a free PEB and write a hidden anchor (zero-length LEB).
 *
 * Per §7.9 / §11.4: the anchor is a data PEB with INTERNAL_ANCHOR_LNUM,
 * zero-length secure LEB record, and initial VID secure metadata counters.
 * Write order: LEB data first (zero-length), VID second (commit point).
 *
 * \param[in]     ubi     UBI device (caller holds mutex, at least 1 free PEB).
 * \param[in,out] vol     Volume to bind the anchor to.
 *
 * \retval 0       Success — vol->anchor_pnum is set.
 * \retval -EIO    I/O or crypto failure.
 * \retval -ENOSPC No free PEBs.
 */
int ubi_secure_anchor_create(struct ubi_device *ubi, struct ubi_volume *vol)
{
	__ASSERT_NO_MSG(ubi != NULL);
	__ASSERT_NO_MSG(vol != NULL);

	if (ubi->free_peb_count == 0) {
		LOG_ERR("No free PEB for anchor allocation");
		return -ENOSPC;
	}

	/* 1. Take a free PEB. */
	struct rbnode *min_node = rb_get_min(&ubi->free_pebs);
	struct ubi_rbt_item *item = CONTAINER_OF(min_node, struct ubi_rbt_item, node);

	rb_remove(&ubi->free_pebs, &item->node);
	ubi->free_peb_count--;

	const size_t pnum = item->value.pnum;

	/* 2. Read authentic EC context from the PEB. */
	struct ubi_ec_hdr ec_hdr = { 0 };
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };

	int ret = ubi_secure_ec_hdr_read(&ubi->mtd, ubi->crypto_cfg, pnum, &ec_hdr, &ec_ctx);

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
	ret = ubi_secure_leb_data_write(&ubi->mtd, ubi->crypto_cfg, pnum, &ec_ctx, &vid_hdr,
					write_kv, NULL, 0, write_kv, 0);
	if (ret != 0) {
		LOG_ERR("Anchor LEB write failure on PEB %zu", pnum);
		goto mark_bad;
	}

	/* 5. Write VID header — commit point.
	 *    Use global VID counter for this key version per §9.8. */
	const uint64_t vid_counter = ubi->next_vid_counter;

	ret = ubi_secure_vid_hdr_write(&ubi->mtd, ubi->crypto_cfg, pnum, &ec_ctx, &vid_hdr,
				       &vid_meta, write_kv, vid_counter);
	if (ret != 0) {
		LOG_ERR("Anchor VID write failure on PEB %zu", pnum);
		goto mark_bad;
	}

	ubi->next_vid_counter = vid_counter + 1;

	/* 6. Success — track in volume. The item is not inserted into any tree;
	 *    anchor PEBs are tracked via vol->anchor_pnum, not via EBA or free/dirty. */
	vol->anchor_pnum = pnum;
	ubi_mem_leaf_free(item);
	return 0;

mark_bad : {
	const size_t ec_avg = (ubi->ec_count > 0) ? (ubi->ec_sum / ubi->ec_count) : 0;
	struct ubi_list_item *bad = ubi_leaf_as_list(item);

	ubi_move_to_bad_blocks(ubi, pnum, ec_avg, bad);
	return ret;
}
}

/**
 * \brief Read device header via secure reserved scan, bump revision.
 */
static int dev_hdr_read_and_bump(struct ubi_device *ubi, struct ubi_dev_hdr *hdr,
				 struct ubi_dev_secure_meta *meta, struct ubi_vol_hdr *vol_hdrs,
				 size_t *vol_count, int vol_count_delta)
{
	__ASSERT_NO_MSG(ubi != NULL);
	__ASSERT_NO_MSG(hdr != NULL);
	__ASSERT_NO_MSG(meta != NULL);
	__ASSERT_NO_MSG(vol_hdrs != NULL);
	__ASSERT_NO_MSG(vol_count != NULL);

	struct ubi_secure_res_peb_scan scan = { 0 };
	int ret = ubi_secure_res_peb_scan(&ubi->mtd, ubi->crypto_cfg, &scan);

	if (ret != 0) {
		LOG_ERR("Reserved PEB scan failure");
		return ret;
	}

	if (scan.auth_count == 0) {
		LOG_ERR("No authenticated reserved PEBs");
		return -EIO;
	}

	if (scan.auth_count < UBI_SECURE_RES_PEB_NR_ACTIVE) {
		LOG_WRN("Reserved PEB bank degraded at runtime");
		ubi->read_only_degraded = true;
	}

	*hdr = scan.dev_hdr;
	*meta = scan.dev_meta;

	/* Read volume headers. */
	if (scan.dev_hdr.vol_count > 0) {
		ret = ubi_secure_res_peb_read_vol_hdrs(&ubi->mtd, ubi->crypto_cfg, &scan, vol_hdrs,
						       CONFIG_UBI_MAX_NR_OF_VOLUMES);
		if (ret != 0) {
			LOG_ERR("Volume header read failure");
			return ret;
		}
	}
	*vol_count = scan.dev_hdr.vol_count;

	hdr->vol_count += vol_count_delta;
	hdr->revision += 1;
	hdr->hdr_crc = crc32_ieee((const uint8_t *)hdr, sizeof(*hdr) - sizeof(hdr->hdr_crc));

	/* Keep cached revision in sync so freshness snapshots are accurate. */
	ubi->cached_device_revision = hdr->revision;

	/* Snapshot vid_next_counter_floor per §9.8.5. */
	meta->vid_next_counter_floor = ubi->next_vid_counter;

	/* Refresh write_active_key_version so that a key-rotation that changed
	 * requested_write_key_version is persisted into the device metadata. */
	meta->write_active_key_version = ubi->crypto_cfg->policy.requested_write_key_version;

	return 0;
}

/**
 * \brief Reclaim a PEB to the dirty pool by reading its secure EC header.
 */
static int reclaim_peb_to_dirty(struct ubi_device *ubi, struct ubi_rbt_item *item)
{
	__ASSERT_NO_MSG(ubi != NULL);
	__ASSERT_NO_MSG(item != NULL);

	struct ubi_ec_hdr ec_hdr = { 0 };
	struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };

	const int ret = ubi_secure_ec_hdr_read(&ubi->mtd, ubi->crypto_cfg, item->value.pnum,
					       &ec_hdr, &ec_ctx);
	if (ret != 0) {
		LOG_WRN("EC header read failure for PEB %zu, marking bad", item->value.pnum);

		const size_t pnum = item->value.pnum;
		const size_t ec_avg = (ubi->ec_count > 0) ? (ubi->ec_sum / ubi->ec_count) : 0;

		struct ubi_list_item *bad_item = ubi_leaf_as_list(item);

		ubi_move_to_bad_blocks(ubi, pnum, ec_avg, bad_item);
		return 0;
	}

	item->key = ec_hdr.ec;
	rb_insert(&ubi->dirty_pebs, &item->node);
	ubi->dirty_peb_count++;

	return 0;
}

/* Module interface function definitions ------------------------------------------------------- */

int ubi_secure_volume_create(struct ubi_device *ubi, const struct ubi_volume_config *vol_cfg,
			     int *vol_id)
{
	if (ubi == NULL || vol_cfg == NULL || vol_id == NULL) {
		LOG_ERR("secure_vol_create: NULL argument");
		return -EINVAL;
	}

	int ret = -EIO;

	if (!ubi_volume_config_is_valid(vol_cfg)) {
		return -EINVAL;
	}

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	ret = ubi_mutation_allowed(ubi, UBI_MUT_RESERVED_METADATA);
	if (ret != 0) {
		LOG_ERR("Mutation blocked: reserved metadata writes not allowed");
		goto exit;
	}

	/* Check for existing volume with same name. */
	const size_t name_len = strnlen(vol_cfg->name, UBI_VOLUME_NAME_MAX_LEN);
	struct ubi_rbt_item *entry = NULL;

	RB_FOR_EACH_CONTAINER(&ubi->vols, entry, node)
	{
		const struct ubi_volume *vol = entry->value.vol;
		const size_t len = strnlen(vol->cfg.name, UBI_VOLUME_NAME_MAX_LEN);

		if (name_len == len && memcmp(vol_cfg->name, vol->cfg.name, name_len) == 0) {
			if (vol_cfg->type != vol->cfg.type ||
			    vol_cfg->leb_count != vol->cfg.leb_count) {
				LOG_ERR("Volume name exists with different config");
				ret = -EEXIST;
				goto exit;
			}

			*vol_id = vol->vol_id;
			ret = 0;
			goto exit;
		}
	}

	/* Capacity check — account for hidden anchor PEB per §7.9. */
	const size_t usable = ubi->total_data_peb_count - ubi->bad_peb_count;
	const size_t avail = usable - ubi_reserved_peb_count(ubi);
	const size_t needed = vol_cfg->leb_count + 1; /* +1 for anchor PEB */

	if (needed > avail) {
		LOG_ERR("Failed to allocate PEBs for volume");
		ret = -ENOSPC;
		goto exit;
	}

	/* Allocate RAM before any flash mutation. */
	struct ubi_volume *vol = NULL;

	ret = ubi_mem_volume_alloc(&vol);
	if (ret != 0) {
		LOG_ERR("Volume allocation failure");
		goto exit;
	}

	struct ubi_rbt_item *item = NULL;

	ret = ubi_mem_leaf_alloc((void **)&item);
	if (ret != 0) {
		LOG_ERR("Leaf item allocation failure");
		ubi_mem_volume_free(vol);
		goto exit;
	}

	/* Read authenticated device header and all vol headers. */
	struct ubi_dev_hdr dev_hdr = { 0 };
	struct ubi_dev_secure_meta dev_meta = { 0 };
	struct ubi_vol_hdr vol_hdrs[CONFIG_UBI_MAX_NR_OF_VOLUMES] = { 0 };
	size_t existing_vol_count = 0;

	ret = dev_hdr_read_and_bump(ubi, &dev_hdr, &dev_meta, vol_hdrs, &existing_vol_count, 1);
	if (ret != 0) {
		LOG_ERR("Device header read failure during create");
		ubi_mem_leaf_free(item);
		ubi_mem_volume_free(vol);
		goto exit;
	}

	/* Overflow guard. */
	if (dev_hdr.vol_id_watermark == UINT32_MAX) {
		LOG_ERR("Volume ID space exhausted");
		ubi_mem_leaf_free(item);
		ubi_mem_volume_free(vol);
		ret = -ENOSPC;
		goto exit;
	}

	/* Prepare new vol header. */
	struct ubi_vol_hdr new_vol_hdr = { 0 };

	new_vol_hdr.magic = UBI_VOL_HDR_MAGIC;
	new_vol_hdr.version = UBI_VOL_HDR_VERSION;
	new_vol_hdr.vol_type = vol_cfg->type;
	new_vol_hdr.vol_id = dev_hdr.vol_id_watermark;
	new_vol_hdr.leb_count = vol_cfg->leb_count;
	ubi_copy_name_to_hdr(new_vol_hdr.name, vol_cfg->name);
	new_vol_hdr.hdr_crc = crc32_ieee((const uint8_t *)&new_vol_hdr,
					 sizeof(new_vol_hdr) - sizeof(new_vol_hdr.hdr_crc));

	dev_hdr.vol_id_watermark += 1;
	dev_hdr.hdr_crc =
		crc32_ieee((const uint8_t *)&dev_hdr, sizeof(dev_hdr) - sizeof(dev_hdr.hdr_crc));

	/* Append new vol header to the existing list. */
	vol_hdrs[existing_vol_count] = new_vol_hdr;
	const size_t new_vol_count = existing_vol_count + 1;

	const uint8_t write_kv = ubi->crypto_cfg->policy.requested_write_key_version;

	ret = ubi_secure_res_peb_commit(&ubi->mtd, ubi->crypto_cfg, &dev_hdr, &dev_meta, vol_hdrs,
					new_vol_count, write_kv, 0);
	if (ret == -EROFS) {
		LOG_WRN("Reserved PEB bank degraded during create commit");
		ubi->read_only_degraded = true;
	}

	/* Deliberate: -EROFS means at least one PEB committed successfully.
	 * The secure backend continues to update RAM state, unlike plain which aborts.
	 * This is more resilient — the volume exists on-flash even if degraded. */
	if (ret != 0 && ret != -EROFS) {
		LOG_ERR("Reserved PEB commit failure during create");
		ubi_mem_leaf_free(item);
		ubi_mem_volume_free(vol);
		goto exit;
	}

	/* Commit succeeded — update RAM state. */
	vol->vol_id = new_vol_hdr.vol_id;
	ubi_copy_name_from_hdr(vol->cfg.name, new_vol_hdr.name);
	vol->cfg.type = new_vol_hdr.vol_type;
	vol->cfg.leb_count = new_vol_hdr.leb_count;
	vol->eba_tbl_count = 0;
	vol->eba_tbl.lessthan_fn = ubi_cache_cmp;
	vol->anchor_pnum = SIZE_MAX;

	item->key = vol->vol_id;
	item->value.vol = vol;
	rb_insert(&ubi->vols, &item->node);
	ubi->vol_count++;
	ubi->vol_id_watermark = dev_hdr.vol_id_watermark;

	/* Allocate hidden anchor PEB per §11.4.
	 * If anchor creation fails the volume must not be usable without
	 * a live authenticated anchor.  Roll back the RAM state and
	 * propagate the error.  The reserved metadata already carries
	 * the volume record on-flash; on next attach the init code will
	 * rediscover it (without anchor protection until re-created). */
	ret = ubi_secure_anchor_create(ubi, vol);
	if (ret != 0) {
		LOG_ERR("Hidden anchor creation failed for vol %zu — rolling back", vol->vol_id);
		rb_remove(&ubi->vols, &item->node);
		ubi->vol_count--;
		ubi_mem_leaf_free(item);
		ubi_mem_volume_free(vol);
		goto exit;
	}

	*vol_id = vol->vol_id;
	ubi_secure_maybe_sync_freshness(ubi);
	ret = 0;

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

int ubi_secure_volume_resize(struct ubi_device *ubi, int vol_id,
			     const struct ubi_volume_config *vol_cfg)
{
	if (ubi == NULL || vol_cfg == NULL) {
		LOG_ERR("secure_vol_resize: NULL argument");
		return -EINVAL;
	}

	int ret = -EIO;

	if (vol_cfg->leb_count == 0) {
		return -EINVAL;
	}

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	ret = ubi_mutation_allowed(ubi, UBI_MUT_RESERVED_METADATA);
	if (ret != 0) {
		LOG_ERR("Mutation blocked: reserved metadata writes not allowed");
		goto exit;
	}

	struct ubi_volume *vol = ubi_find_volume(ubi, vol_id);

	if (!vol) {
		ret = -ENOENT;
		goto exit;
	}

	if (vol->cfg.type != UBI_VOLUME_TYPE_DYNAMIC) {
		LOG_ERR("Static volume cannot be resized");
		ret = -ECANCELED;
		goto exit;
	}

	if (vol_cfg->leb_count == vol->cfg.leb_count) {
		LOG_ERR("Cannot resize for the same count of LEBs");
		ret = -ECANCELED;
		goto exit;
	}

	if (vol_cfg->leb_count > vol->cfg.leb_count) {
		const size_t avail = ubi->total_data_peb_count - ubi->bad_peb_count -
				     ubi_reserved_peb_count(ubi);
		const size_t diff = vol_cfg->leb_count - vol->cfg.leb_count;

		if (diff > avail) {
			LOG_ERR("Not enough free PEBs");
			ret = -ENOSPC;
			goto exit;
		}
	}

	/* Read and bump device header + all vol headers. */
	struct ubi_dev_hdr dev_hdr = { 0 };
	struct ubi_dev_secure_meta dev_meta = { 0 };
	struct ubi_vol_hdr vol_hdrs[CONFIG_UBI_MAX_NR_OF_VOLUMES] = { 0 };
	size_t existing_vol_count = 0;

	ret = dev_hdr_read_and_bump(ubi, &dev_hdr, &dev_meta, vol_hdrs, &existing_vol_count, 0);
	if (ret != 0) {
		LOG_ERR("Device header read failure during resize");
		goto exit;
	}

	/* Update the matching volume header's leb_count. */
	bool found = false;

	for (size_t i = 0; i < existing_vol_count; i++) {
		if (vol_hdrs[i].vol_id == (uint32_t)vol_id) {
			vol_hdrs[i].leb_count = vol_cfg->leb_count;
			vol_hdrs[i].hdr_crc =
				crc32_ieee((const uint8_t *)&vol_hdrs[i],
					   sizeof(vol_hdrs[i]) - sizeof(vol_hdrs[i].hdr_crc));
			found = true;
			break;
		}
	}

	if (!found) {
		LOG_ERR("Volume %d not found in reserved metadata", vol_id);
		ret = -ENOENT;
		goto exit;
	}

	const uint8_t write_kv = ubi->crypto_cfg->policy.requested_write_key_version;

	ret = ubi_secure_res_peb_commit(&ubi->mtd, ubi->crypto_cfg, &dev_hdr, &dev_meta, vol_hdrs,
					existing_vol_count, write_kv, 0);
	if (ret == -EROFS) {
		LOG_WRN("Reserved PEB bank degraded during resize commit");
		ubi->read_only_degraded = true;
	}

	/* Deliberate: continue past -EROFS — data committed to at least one bank. */
	if (ret != 0 && ret != -EROFS) {
		LOG_ERR("Reserved PEB commit failure during resize");
		goto exit;
	}

	/* Flash commit succeeded — now safe to mutate RAM state. */
	if (vol_cfg->leb_count < vol->cfg.leb_count) {
		for (size_t lnum = vol_cfg->leb_count; lnum < vol->cfg.leb_count; lnum++) {
			struct ubi_rbt_item *eba_item = ubi_cache_search(&vol->eba_tbl, lnum);

			if (eba_item) {
				rb_remove(&vol->eba_tbl, &eba_item->node);
				vol->eba_tbl_count--;

				ret = reclaim_peb_to_dirty(ubi, eba_item);
				if (ret != 0) {
					goto exit;
				}
			}
		}
	}

	vol->cfg.leb_count = vol_cfg->leb_count;
	ubi_secure_maybe_sync_freshness(ubi);
	ret = 0;

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

int ubi_secure_volume_remove(struct ubi_device *ubi, int vol_id)
{
	if (ubi == NULL) {
		LOG_ERR("secure_vol_remove: NULL argument");
		return -EINVAL;
	}

	int ret = -EIO;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	ret = ubi_mutation_allowed(ubi, UBI_MUT_RESERVED_METADATA);
	if (ret != 0) {
		LOG_ERR("Mutation blocked: reserved metadata writes not allowed");
		goto exit;
	}

	if (ubi->vol_count == 0) {
		LOG_ERR("No volumes present on device");
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_rbt_item *vol_entry = ubi_cache_search(&ubi->vols, vol_id);

	if (!vol_entry) {
		LOG_ERR("Device volume not found");
		ret = -ENOENT;
		goto exit;
	}

	/* Read and bump device header + all vol headers. */
	struct ubi_dev_hdr dev_hdr = { 0 };
	struct ubi_dev_secure_meta dev_meta = { 0 };
	struct ubi_vol_hdr vol_hdrs[CONFIG_UBI_MAX_NR_OF_VOLUMES] = { 0 };
	size_t existing_vol_count = 0;

	ret = dev_hdr_read_and_bump(ubi, &dev_hdr, &dev_meta, vol_hdrs, &existing_vol_count, -1);
	if (ret != 0) {
		LOG_ERR("Device header read failure during remove");
		goto exit;
	}

	/* Build new vol_hdrs list without the removed volume. */
	struct ubi_vol_hdr new_vol_hdrs[CONFIG_UBI_MAX_NR_OF_VOLUMES] = { 0 };
	size_t new_count = 0;

	for (size_t i = 0; i < existing_vol_count; i++) {
		if (vol_hdrs[i].vol_id != (uint32_t)vol_id) {
			new_vol_hdrs[new_count++] = vol_hdrs[i];
		}
	}

	const uint8_t write_kv = ubi->crypto_cfg->policy.requested_write_key_version;

	ret = ubi_secure_res_peb_commit(&ubi->mtd, ubi->crypto_cfg, &dev_hdr, &dev_meta,
					new_vol_hdrs, new_count, write_kv, 0);
	if (ret == -EROFS) {
		LOG_WRN("Reserved PEB bank degraded during remove commit");
		ubi->read_only_degraded = true;
	}

	/* Deliberate: continue past -EROFS — data committed to at least one bank. */
	if (ret != 0 && ret != -EROFS) {
		LOG_ERR("Reserved PEB commit failure during remove");
		goto exit;
	}

	/* Flash commit succeeded — reclaim PEBs. */
	struct ubi_volume *vol = vol_entry->value.vol;
	struct rbnode *eba_node = NULL;

	while ((eba_node = rb_get_min(&vol->eba_tbl))) {
		struct ubi_rbt_item *eba_item = CONTAINER_OF(eba_node, struct ubi_rbt_item, node);

		rb_remove(&vol->eba_tbl, &eba_item->node);
		vol->eba_tbl_count--;

		(void)reclaim_peb_to_dirty(ubi, eba_item);
	}

	/* Reclaim anchor PEB to dirty pool. */
	if (vol->anchor_pnum != SIZE_MAX) {
		struct ubi_rbt_item *anchor_item = NULL;

		ret = ubi_mem_leaf_alloc((void **)&anchor_item);
		if (ret == 0) {
			anchor_item->value.pnum = vol->anchor_pnum;
			(void)reclaim_peb_to_dirty(ubi, anchor_item);
		} else {
			LOG_WRN("Leaf alloc failed for anchor PEB during remove");
		}
		vol->anchor_pnum = SIZE_MAX;
	}

	rb_remove(&ubi->vols, &vol_entry->node);
	ubi->vol_count--;

	ubi_mem_volume_free(vol_entry->value.vol);
	ubi_mem_leaf_free(vol_entry);

	ubi_secure_maybe_sync_freshness(ubi);
	ret = 0;

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

int ubi_secure_volume_get_info(struct ubi_device *ubi, int vol_id,
			       struct ubi_volume_config *vol_cfg, size_t *alloc_lebs)
{
	if (ubi == NULL || vol_cfg == NULL || alloc_lebs == NULL) {
		LOG_ERR("secure_vol_get_info: NULL argument");
		return -EINVAL;
	}

	int ret = -EIO;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	const struct ubi_volume *vol = ubi_find_volume(ubi, vol_id);

	if (!vol) {
		ret = -ENOENT;
		goto exit;
	}

	*vol_cfg = vol->cfg;
	*alloc_lebs = vol->eba_tbl_count;
	ret = 0;

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}
