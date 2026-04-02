/**
 * \file    ubi_volume.c
 * \author  Kamil Kielbasa
 * \brief   UBI volume management: create, resize, remove, get_info.
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files ------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_internal.h"
#include "ubi_mem.h"

/* Zephyr headers: */
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

/* Standard library headers: */
#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------ */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Static function declarations ---------------------------------------------------------------- */

static int dev_hdr_read_and_bump(const struct ubi_mtd *mtd, struct ubi_dev_hdr *hdr,
				 int vol_count_delta);
static int reclaim_peb_to_dirty(struct ubi_device *ubi, struct ubi_rbt_item *item);

/* Static function definitions ----------------------------------------------------------------- */

static int dev_hdr_read_and_bump(const struct ubi_mtd *mtd, struct ubi_dev_hdr *hdr,
				 int vol_count_delta)
{
	int ret = ubi_dev_hdr_read(mtd, hdr);

	if (ret == -EROFS) {
		LOG_ERR("Device in degraded read-only mode");
		return -EROFS;
	}

	if (ret != 0) {
		LOG_ERR("Device header read failure");
		return ret;
	}

	hdr->vol_count += vol_count_delta;
	hdr->revision += 1;
	hdr->hdr_crc = crc32_ieee((const uint8_t *)hdr, sizeof(*hdr) - sizeof(hdr->hdr_crc));

	return 0;
}

static int reclaim_peb_to_dirty(struct ubi_device *ubi, struct ubi_rbt_item *item)
{
	struct ubi_ec_hdr ec_hdr = { 0 };
	int ret = ubi_ec_hdr_read(&ubi->mtd, item->value.pnum, &ec_hdr);

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
	ubi->dirty_peb_count += 1;

	return 0;
}

/* Module interface function definitions ------------------------------------------------------- */

int ubi_volume_create(struct ubi_device *ubi, const struct ubi_volume_config *vol_cfg, int *vol_id)
{
	int ret = -EIO;

	if (!ubi || !vol_cfg || !vol_id)
		return -EINVAL;

	if (!ubi_volume_config_is_valid(vol_cfg))
		return -EINVAL;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	/* Return existing volume if name already exists with identical config. */
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

	/* Capacity check accounting for bad PEBs. */
	const size_t usable = ubi->total_data_peb_count - ubi->bad_peb_count;
	const size_t avail = usable - ubi_reserved_peb_count(ubi);

	if (vol_cfg->leb_count > avail) {
		LOG_ERR("Failed to allocate PEBs for volume");
		ret = -ENOSPC;
		goto exit;
	}

	/* Allocate RAM before any flash mutation so failures are side-effect free. */
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

	struct ubi_dev_hdr dev_hdr = { 0 };
	ret = dev_hdr_read_and_bump(&ubi->mtd, &dev_hdr, 1);

	if (ret != 0) {
		LOG_ERR("Device header read failure during create");
		ubi_mem_leaf_free(item);
		ubi_mem_volume_free(vol);
		goto exit;
	}

	struct ubi_vol_hdr new_vol_hdr = { 0 };
	new_vol_hdr.magic = UBI_VOL_HDR_MAGIC;
	new_vol_hdr.version = UBI_VOL_HDR_VERSION;
	new_vol_hdr.vol_type = vol_cfg->type;
	new_vol_hdr.vol_id = ubi->vol_next_id++;
	new_vol_hdr.leb_count = vol_cfg->leb_count;
	ubi_copy_name_to_hdr(new_vol_hdr.name, vol_cfg->name);
	new_vol_hdr.hdr_crc = crc32_ieee((const uint8_t *)&new_vol_hdr,
					 sizeof(new_vol_hdr) - sizeof(new_vol_hdr.hdr_crc));

	ret = ubi_vol_hdr_append(&ubi->mtd, &dev_hdr, &new_vol_hdr);

	if (ret != 0) {
		LOG_ERR("Volume header append failure");
		ubi_mem_leaf_free(item);
		ubi_mem_volume_free(vol);
		goto exit;
	}

	vol->vol_idx = dev_hdr.vol_count - 1;
	vol->vol_id = new_vol_hdr.vol_id;
	ubi_copy_name_from_hdr(vol->cfg.name, new_vol_hdr.name);
	vol->cfg.type = new_vol_hdr.vol_type;
	vol->cfg.leb_count = new_vol_hdr.leb_count;
	vol->eba_tbl_count = 0;
	vol->eba_tbl.lessthan_fn = ubi_cache_cmp;

	item->key = vol->vol_id;
	item->value.vol = vol;
	rb_insert(&ubi->vols, &item->node);
	ubi->vol_count += 1;

	*vol_id = vol->vol_id;

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

int ubi_volume_resize(struct ubi_device *ubi, int vol_id, const struct ubi_volume_config *vol_cfg)
{
	int ret = -EIO;

	if (!ubi || !vol_cfg)
		return -EINVAL;

	if (vol_cfg->leb_count == 0)
		return -EINVAL;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

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
			LOG_ERR("Not enough free PEBs to allocate requested LEBs");
			ret = -ENOSPC;
			goto exit;
		}
	}

	/* Commit metadata to flash BEFORE any in-RAM state mutation. */
	struct ubi_dev_hdr dev_hdr = { 0 };
	ret = dev_hdr_read_and_bump(&ubi->mtd, &dev_hdr, 0);

	if (ret != 0) {
		LOG_ERR("Device header read failure during resize");
		goto exit;
	}

	struct ubi_vol_hdr vol_hdr = { 0 };
	ret = ubi_vol_hdr_read(&ubi->mtd, vol->vol_idx, &vol_hdr);

	if (ret != 0) {
		LOG_ERR("Volume header read failure");
		goto exit;
	}

	vol_hdr.leb_count = vol_cfg->leb_count;
	vol_hdr.hdr_crc =
		crc32_ieee((const uint8_t *)&vol_hdr, sizeof(vol_hdr) - sizeof(vol_hdr.hdr_crc));

	ret = ubi_vol_hdr_update(&ubi->mtd, &dev_hdr, vol->vol_idx, &vol_hdr);

	if (ret != 0) {
		LOG_ERR("Volume header update failure");
		goto exit;
	}

	/* Flash commit succeeded -- now safe to mutate in-RAM state. */
	if (vol_cfg->leb_count < vol->cfg.leb_count) {
		for (size_t lnum = vol_cfg->leb_count; lnum < vol->cfg.leb_count; ++lnum) {
			struct ubi_rbt_item *item = ubi_cache_search(&vol->eba_tbl, lnum);

			if (item) {
				rb_remove(&vol->eba_tbl, &item->node);
				vol->eba_tbl_count -= 1;

				ret = reclaim_peb_to_dirty(ubi, item);

				if (ret != 0)
					goto exit;
			}
		}
	}

	vol->cfg.leb_count = vol_cfg->leb_count;

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

int ubi_volume_remove(struct ubi_device *ubi, int vol_id)
{
	int ret = -EIO;

	if (!ubi)
		return -EINVAL;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	if (ubi->vol_count == 0) {
		LOG_ERR("No volumes present on device");
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_rbt_item *entry = ubi_cache_search(&ubi->vols, vol_id);

	if (!entry) {
		LOG_ERR("Device volume not found");
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_dev_hdr dev_hdr = { 0 };
	ret = dev_hdr_read_and_bump(&ubi->mtd, &dev_hdr, -1);

	if (ret != 0) {
		LOG_ERR("Device header read failure during remove");
		goto exit;
	}

	struct ubi_volume *vol = entry->value.vol;
	ret = ubi_vol_hdr_remove(&ubi->mtd, &dev_hdr, vol->vol_idx);

	if (ret != 0) {
		LOG_ERR("Volume header remove failure");
		goto exit;
	}

	/*
	 * Flash commit succeeded -- volume is removed from persistent storage.
	 * Reclaim mapped PEBs best-effort; errors here must not abort since the
	 * volume is already gone on flash.
	 */
	struct rbnode *eba_node = NULL;

	while ((eba_node = rb_get_min(&vol->eba_tbl))) {
		struct ubi_rbt_item *item = CONTAINER_OF(eba_node, struct ubi_rbt_item, node);

		rb_remove(&vol->eba_tbl, &item->node);
		vol->eba_tbl_count -= 1;

		(void)reclaim_peb_to_dirty(ubi, item);
	}

	rb_remove(&ubi->vols, &entry->node);
	ubi->vol_count -= 1;

	ubi_mem_volume_free(entry->value.vol);
	ubi_mem_leaf_free(entry);

	/* Re-index remaining volumes to match flash layout. */
	for (size_t vol_idx = 0; vol_idx < dev_hdr.vol_count; ++vol_idx) {
		struct ubi_vol_hdr vol_hdr = { 0 };
		int idx_ret = ubi_vol_hdr_read(&ubi->mtd, vol_idx, &vol_hdr);

		if (idx_ret != 0) {
			LOG_ERR("Volume header read failure during re-index");
			continue;
		}

		entry = ubi_cache_search(&ubi->vols, vol_hdr.vol_id);

		if (!entry) {
			LOG_ERR("Inconsistency between cache and nvm");
			continue;
		}

		vol = entry->value.vol;
		vol->vol_idx = vol_idx;
	}

	ret = 0;

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

int ubi_volume_get_info(struct ubi_device *ubi, int vol_id, struct ubi_volume_config *vol_cfg,
			size_t *alloc_lebs)
{
	if (!ubi || vol_id < 0 || !vol_cfg || !alloc_lebs)
		return -EINVAL;

	int ret = -EIO;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	struct ubi_volume *vol = ubi_find_volume(ubi, vol_id);

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
