/**
 * \file    ubi_leb.c
 * \author  Kamil Kielbasa
 * \brief   UBI LEB operations: write, read, map, unmap, is_mapped, get_size.
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files ------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_internal.h"

/* Zephyr headers: */
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/crc.h>

/* Standard library headers: */
#include <errno.h>
#include <stdbool.h>

/* Module defines ------------------------------------------------------------------------------ */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Static function declarations ---------------------------------------------------------------- */

static int leb_prepare_new_mapping(struct ubi_device *ubi, struct ubi_volume *vol, size_t lnum,
				   const void *buf, size_t len, struct ubi_rbt_item **out_new_node);
static void leb_commit_mapping_swap(struct ubi_device *ubi, struct ubi_volume *vol, size_t lnum,
				    struct ubi_rbt_item *new_node);
static void leb_mark_peb_bad(struct ubi_device *ubi, struct ubi_rbt_item *node);

/* Static function definitions ----------------------------------------------------------------- */

/**
 * Allocate a free PEB, write VID header and optional data payload.
 * On success *out_new_node points to the rbt item (already removed from free pool).
 * On failure the PEB is marked bad and the function returns a negative errno.
 * Caller must hold ubi->mutex.
 */
static int leb_prepare_new_mapping(struct ubi_device *ubi, struct ubi_volume *vol, size_t lnum,
				   const void *buf, size_t len, struct ubi_rbt_item **out_new_node)
{
	struct rbnode *min_rbnode = rb_get_min(&ubi->free_pebs);
	struct ubi_rbt_item *new_node = CONTAINER_OF(min_rbnode, struct ubi_rbt_item, node);

	rb_remove(&ubi->free_pebs, &new_node->node);
	ubi->free_peb_count -= 1;

	struct ubi_vid_hdr vid_hdr = { 0 };
	vid_hdr.magic = UBI_VID_HDR_MAGIC;
	vid_hdr.version = UBI_VID_HDR_VERSION;
	vid_hdr.lnum = lnum;
	vid_hdr.vol_id = vol->vol_id;
	vid_hdr.sqnum = ubi->global_sqnum++;
	vid_hdr.data_size = len;
	vid_hdr.hdr_crc =
		crc32_ieee((const uint8_t *)&vid_hdr, sizeof(vid_hdr) - sizeof(vid_hdr.hdr_crc));

	int ret = ubi_vid_hdr_write(&ubi->mtd, new_node->value.pnum, &vid_hdr);

	if (ret != 0) {
		LOG_ERR("VID header write failure");
		leb_mark_peb_bad(ubi, new_node);
		return ret;
	}

	if (buf && len > 0) {
		ret = ubi_leb_data_write(&ubi->mtd, new_node->value.pnum, buf, len);

		if (ret != 0) {
			LOG_ERR("LEB data write failure");
			leb_mark_peb_bad(ubi, new_node);
			return ret;
		}
	}

	*out_new_node = new_node;
	return 0;
}

/**
 * Swap the old EBA entry (if any) for the newly written PEB.
 * Old PEB moves to dirty pool. Caller must hold ubi->mutex.
 */
static void leb_commit_mapping_swap(struct ubi_device *ubi, struct ubi_volume *vol, size_t lnum,
				    struct ubi_rbt_item *new_node)
{
	struct ubi_rbt_item *old_entry = ubi_cache_search(&vol->eba_tbl, lnum);

	if (old_entry) {
		struct ubi_ec_hdr old_ec = { 0 };
		int ec_ret = ubi_ec_hdr_read(&ubi->mtd, old_entry->value.pnum, &old_ec);

		rb_remove(&vol->eba_tbl, &old_entry->node);
		vol->eba_tbl_count -= 1;

		old_entry->key = (ec_ret == 0) ? old_ec.ec : 0;
		rb_insert(&ubi->dirty_pebs, &old_entry->node);
		ubi->dirty_peb_count += 1;
	}

	new_node->key = lnum;
	rb_insert(&vol->eba_tbl, &new_node->node);
	vol->eba_tbl_count += 1;
}

/**
 * Mark a PEB that failed a write as bad.
 * Frees the rbt item. Caller must hold ubi->mutex.
 */
static void leb_mark_peb_bad(struct ubi_device *ubi, struct ubi_rbt_item *node)
{
	const size_t failed_pnum = node->value.pnum;
	const size_t failed_ec = node->key;

	k_free(node);

	struct ubi_list_item *bad_item = k_malloc(sizeof(*bad_item));

	if (bad_item) {
		ubi->ec_sum -= failed_ec;
		ubi->ec_count -= 1;
		ubi_move_to_bad_blocks(ubi, failed_pnum, failed_ec, bad_item);
	} else {
		LOG_WRN("Cannot allocate bad PEB entry, PEB %zu lost from tracking", failed_pnum);
	}
}

static int leb_write(struct ubi_device *ubi, int vol_id, size_t lnum, const void *buf, size_t len)
{
	__ASSERT_NO_MSG(ubi);
	__ASSERT_NO_MSG(vol_id >= 0);
	__ASSERT_NO_MSG((buf && len > 0) || (!buf && len == 0));

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	int ret = -EIO;

	struct ubi_volume *vol = ubi_find_volume(ubi, vol_id);

	if (!vol) {
		ret = -ENOENT;
		goto exit;
	}

	if (lnum >= vol->cfg.leb_count) {
		LOG_ERR("Volume LEB limit exceeded");
		ret = -EACCES;
		goto exit;
	}

	if (ubi->free_peb_count == 0) {
		LOG_ERR("Lack of free PEBs");
		ret = -ENOSPC;
		goto exit;
	}

	if (len > (ubi->mtd.erase_block_size - UBI_EC_HDR_SIZE - UBI_VID_HDR_SIZE)) {
		LOG_ERR("Too big buffer to write in LEB");
		ret = -ENOSPC;
		goto exit;
	}

	struct ubi_rbt_item *new_node = NULL;

	ret = leb_prepare_new_mapping(ubi, vol, lnum, buf, len, &new_node);

	if (ret != 0)
		goto exit;

	leb_commit_mapping_swap(ubi, vol, lnum, new_node);

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

/* Module interface function definitions ------------------------------------------------------- */

int ubi_leb_write(struct ubi_device *ubi, int vol_id, size_t lnum, const void *buf, size_t len)
{
	if (!ubi || vol_id < 0 || !buf || len == 0)
		return -EINVAL;

	return leb_write(ubi, vol_id, lnum, buf, len);
}

int ubi_leb_read(struct ubi_device *ubi, int vol_id, size_t lnum, size_t offset, void *buf,
		 size_t len)
{
	int ret = -EIO;

	if (!ubi || vol_id < 0 || !buf || len == 0)
		return -EINVAL;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	struct ubi_volume *vol = ubi_find_volume(ubi, vol_id);

	if (!vol) {
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
		LOG_ERR("LEB not found");
		ret = -ENOENT;
		goto exit;
	}

	/* Validate read range against actual data size stored in VID header */
	struct ubi_vid_hdr vid_hdr = { 0 };
	ret = ubi_vid_hdr_read(&ubi->mtd, entry->value.pnum, &vid_hdr, true);

	if (ret != 0) {
		LOG_ERR("VID header read failure");
		goto exit;
	}

	if ((offset + len) > vid_hdr.data_size) {
		LOG_ERR("Read beyond data_size: offset=%zu len=%zu data_size=%u", offset, len,
			vid_hdr.data_size);
		ret = -EINVAL;
		goto exit;
	}

	ret = ubi_leb_data_read(&ubi->mtd, entry->value.pnum, offset, buf, len);

	if (ret != 0) {
		LOG_ERR("LEB data read failure");
		goto exit;
	}

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

int ubi_leb_map(struct ubi_device *ubi, int vol_id, size_t lnum)
{
	if (!ubi || vol_id < 0)
		return -EINVAL;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	int ret = -EIO;

	struct ubi_volume *vol = ubi_find_volume(ubi, vol_id);

	if (!vol) {
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

	if (ubi->free_peb_count == 0) {
		LOG_ERR("Lack of free PEBs");
		ret = -ENOSPC;
		goto exit;
	}

	struct ubi_rbt_item *new_node = NULL;

	ret = leb_prepare_new_mapping(ubi, vol, lnum, NULL, 0, &new_node);

	if (ret != 0)
		goto exit;

	leb_commit_mapping_swap(ubi, vol, lnum, new_node);

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

int ubi_leb_unmap(struct ubi_device *ubi, int vol_id, size_t lnum)
{
	int ret = -EIO;

	if (!ubi || vol_id < 0)
		return -EINVAL;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	struct ubi_volume *vol = ubi_find_volume(ubi, vol_id);

	if (!vol) {
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

	struct ubi_ec_hdr ec_hdr = { 0 };
	ret = ubi_ec_hdr_read(&ubi->mtd, entry->value.pnum, &ec_hdr);

	if (ret != 0) {
		LOG_ERR("EC header read failure");
		goto exit;
	}

	rb_remove(&vol->eba_tbl, &entry->node);
	vol->eba_tbl_count -= 1;

	entry->key = ec_hdr.ec;
	rb_insert(&ubi->dirty_pebs, &entry->node);
	ubi->dirty_peb_count += 1;

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

int ubi_leb_is_mapped(struct ubi_device *ubi, int vol_id, size_t lnum, bool *is_mapped)
{
	if (!ubi || vol_id < 0 || !is_mapped)
		return -EINVAL;

	int ret = -EIO;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	struct ubi_volume *vol = ubi_find_volume(ubi, vol_id);

	if (!vol) {
		ret = -ENOENT;
		goto exit;
	}

	if (lnum >= vol->cfg.leb_count) {
		LOG_ERR("Volume LEB limit exceeded");
		ret = -EACCES;
		goto exit;
	}

	struct ubi_rbt_item *entry = ubi_cache_search(&vol->eba_tbl, lnum);

	*is_mapped = (NULL == entry) ? false : true;
	ret = 0;

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

int ubi_leb_get_size(struct ubi_device *ubi, int vol_id, size_t lnum, size_t *size)
{
	int ret = -EIO;

	if (!ubi || vol_id < 0 || !size)
		return -EINVAL;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	struct ubi_volume *vol = ubi_find_volume(ubi, vol_id);

	if (!vol) {
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
		LOG_ERR("LEB %zu in volume %d is not mapped", lnum, vol_id);
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_vid_hdr vid_hdr = { 0 };
	ret = ubi_vid_hdr_read(&ubi->mtd, entry->value.pnum, &vid_hdr, true);

	if (ret != 0) {
		LOG_ERR("VID header read failure");
		goto exit;
	}

	*size = vid_hdr.data_size;

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}
