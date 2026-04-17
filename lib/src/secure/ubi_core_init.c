/**
 * \file    ubi_core_init.c
 * \author  Kamil Kielbasa
 * \brief   Secure backend device initialization: mode detection, format, attach.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files ------------------------------------------------------------------------------- */
#include "ubi_secure_reserved.h"
#include "ubi_secure_crypto.h"
#include "ubi_secure_event.h"
#include "ubi_secure_ser.h"
#include "ubi_secure_io.h"
#include "ubi_secure_test_hooks.h"
#include "ubi_secure_types.h"
#include "ubi_secure_ops.h"
#include "ubi_internal.h"
#include "ubi_backend.h"
#include "ubi_plain_io.h"
#include "ubi_mem.h"
#include "ubi_partition_guard.h"

#include <ubi_crypto.h>

#include <psa/crypto.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/crc.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/device.h>

#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------ */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/** Return values for scan helper functions. */
enum scan_result {
	SCAN_NEXT_STEP = 0,
	SCAN_PEB_HANDLED = 1,
};

/* Static function declarations ---------------------------------------------------------------- */

/**
 * \brief Validate crypto config: all callbacks must be non-NULL.
 */
static int validate_crypto_cfg(const struct ubi_crypto_config *cfg)
{
	__ASSERT_NO_MSG(cfg != NULL);
	if (!cfg->get_key_id || !cfg->check_freshness || !cfg->sync_freshness || !cfg->event_cb) {
		LOG_ERR("Crypto config has NULL callbacks");
		return -EINVAL;
	}

	if (cfg->policy.allowed_key_versions_len == 0 || !cfg->policy.allowed_key_versions) {
		LOG_ERR("Crypto config has empty allowlist");
		return -EINVAL;
	}

	return 0;
}

/**
 * \brief Check if the requested write key version is in the allowlist.
 */
static bool key_version_is_allowed(const struct ubi_crypto_policy *policy, uint8_t kv)
{
	__ASSERT_NO_MSG(policy != NULL);
	__ASSERT_NO_MSG(policy->allowed_key_versions != NULL);
	for (size_t i = 0; i < policy->allowed_key_versions_len; i++) {
		if (policy->allowed_key_versions[i] == kv) {
			return true;
		}
	}
	return false;
}

/**
 * \brief Detect mode from reserved PEBs: blank, secure, or plain.
 *
 * \retval 0     All PEBs classified.
 * \retval -EIO  Flash error.
 */
static int detect_reserved_mode(const struct ubi_mtd *mtd, bool *any_blank, bool *any_secure,
				bool *any_plain)
{
	__ASSERT_NO_MSG(mtd != NULL);
	__ASSERT_NO_MSG(any_blank != NULL);
	__ASSERT_NO_MSG(any_secure != NULL);
	__ASSERT_NO_MSG(any_plain != NULL);

	*any_blank = false;
	*any_secure = false;
	*any_plain = false;

	for (size_t peb = 0; peb < UBI_DEV_HDR_NR_OF_RES_PEBS; peb++) {
		bool is_secure = false;
		bool is_blank = false;

		const int ret = ubi_secure_res_peb_detect_mode(mtd, peb, &is_secure, &is_blank);

		if (ret != 0) {
			LOG_ERR("detect_reserved_mode: PEB %zu detection failed: %d", peb, ret);
			return ret;
		}

		if (is_blank) {
			*any_blank = true;
		} else if (is_secure) {
			*any_secure = true;
		} else {
			/* Has content but not secure magic → plain. */
			*any_plain = true;
		}
	}

	return 0;
}

/**
 * \brief Format data PEBs: erase all and write secure EC headers.
 */
static int init_format_data_pebs(struct ubi_device *ubi_dev, size_t nr_of_pebs)
{
	__ASSERT_NO_MSG(ubi_dev != NULL);

	const struct ubi_crypto_config *cfg = ubi_dev->crypto_cfg;
	const uint8_t write_kv = cfg->policy.requested_write_key_version;

	const struct flash_area *fa = NULL;
	int ret = flash_area_open(ubi_dev->mtd.partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		return -EIO;
	}

	for (size_t peb = UBI_DEV_HDR_NR_OF_RES_PEBS; peb < nr_of_pebs; peb++) {
		const size_t offset = peb * ubi_dev->mtd.erase_block_size;

		ret = flash_area_erase(fa, offset, ubi_dev->mtd.erase_block_size);
		if (ret != 0) {
			LOG_ERR("Flash erase failure at PEB %zu", peb);
			flash_area_close(fa);
			return ret;
		}
	}

	flash_area_close(fa);

	const struct ubi_ec_hdr ec_hdr = {
		.magic = UBI_EC_HDR_MAGIC,
		.version = UBI_EC_HDR_VERSION,
		.ec = 0,
	};

	for (size_t peb = UBI_DEV_HDR_NR_OF_RES_PEBS; peb < nr_of_pebs; peb++) {
		ret = ubi_secure_ec_hdr_write(&ubi_dev->mtd, cfg, peb, &ec_hdr, write_kv,
					      ubi_dev->next_ec_counter);
		if (ret != 0) {
			LOG_ERR("Secure EC header write failure at PEB %zu", peb);
			return ret;
		}

		ubi_dev->next_ec_counter++;
	}

	return 0;
}

/**
 * \brief Collect volumes from authenticated volume headers into RAM.
 */
static int init_collect_volumes(struct ubi_device *ubi_dev, const struct ubi_vol_hdr *vol_hdrs,
				size_t vol_count)
{
	__ASSERT_NO_MSG(ubi_dev != NULL);
	__ASSERT_NO_MSG(vol_hdrs != NULL || vol_count == 0);

	for (size_t i = 0; i < vol_count; i++) {
		const struct ubi_vol_hdr *vh = &vol_hdrs[i];

		if (!ubi_vol_hdr_semantically_valid(vh)) {
			LOG_ERR("Volume header %zu semantically invalid", i);
			return -EIO;
		}

		struct ubi_volume *vol = NULL;
		int ret = ubi_mem_volume_alloc(&vol);

		if (ret != 0) {
			LOG_ERR("Volume allocation failure");
			return ret;
		}

		vol->vol_id = vh->vol_id;
		ubi_copy_name_from_hdr(vol->cfg.name, vh->name);
		vol->cfg.type = vh->vol_type;
		vol->cfg.leb_count = vh->leb_count;
		vol->eba_tbl_count = 0;
		vol->eba_tbl.lessthan_fn = ubi_cache_cmp;
		vol->anchor_pnum = SIZE_MAX;

		struct ubi_rbt_item *item = NULL;

		ret = ubi_mem_leaf_alloc((void **)&item);
		if (ret != 0) {
			LOG_ERR("Leaf item allocation failure");
			ubi_mem_volume_free(vol);
			return ret;
		}

		item->key = vol->vol_id;
		item->value.vol = vol;
		rb_insert(&ubi_dev->vols, &item->node);
		ubi_dev->vol_count += 1;
	}

	return 0;
}

/**
 * \brief Compute average erase counter by reading all secure EC headers.
 */
static void init_compute_ec_average(struct ubi_device *ubi_dev, size_t nr_of_pebs)
{
	__ASSERT_NO_MSG(ubi_dev != NULL);

	size_t ec_sum = 0;
	size_t ec_count = 0;

	for (size_t pnum = UBI_DEV_HDR_NR_OF_RES_PEBS; pnum < nr_of_pebs; pnum++) {
		struct ubi_ec_hdr ec_hdr = { 0 };
		struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };

		const int ret = ubi_secure_ec_hdr_read(&ubi_dev->mtd, ubi_dev->crypto_cfg, pnum,
						       &ec_hdr, &ec_ctx);
		if (ret == 0) {
			ec_sum += ec_hdr.ec;
			ec_count++;
		}
	}

	ubi_dev->ec_sum = ec_sum;
	ubi_dev->ec_count = ec_count;
}

/* Scan helpers ---------------------------------------------------------------- */

/**
 * \brief Validate the secure EC header; mark PEB as bad if the read fails.
 */
static int scan_validate_ec(struct ubi_device *dev, size_t pnum, size_t ec_avg,
			    struct ubi_ec_hdr *ec_hdr, struct ubi_secure_ec_auth_ctx *ec_ctx)
{
	__ASSERT_NO_MSG(dev != NULL);
	__ASSERT_NO_MSG(ec_hdr != NULL);
	__ASSERT_NO_MSG(ec_ctx != NULL);

	const int ret = ubi_secure_ec_hdr_read(&dev->mtd, dev->crypto_cfg, pnum, ec_hdr, ec_ctx);

	if (ret != 0) {
		struct ubi_list_item *item = NULL;
		const int alloc_ret = ubi_mem_leaf_alloc((void **)&item);

		if (alloc_ret != 0) {
			LOG_ERR("Leaf item allocation failure");
			return alloc_ret;
		}

		ubi_move_to_bad_blocks(dev, pnum, ec_avg, item);
		return SCAN_PEB_HANDLED;
	}

	return SCAN_NEXT_STEP;
}

/**
 * \brief Classify PEB by VID region: free, dirty (uncommitted), or continue to VID read.
 */
static int scan_classify_vid_region(struct ubi_device *dev, size_t pnum,
				    const struct ubi_ec_hdr *ec_hdr)
{
	__ASSERT_NO_MSG(dev != NULL);
	__ASSERT_NO_MSG(ec_hdr != NULL);

	bool vid_erased = false;
	int ret = ubi_secure_vid_region_is_erased(&dev->mtd, pnum, &vid_erased);

	if (ret != 0) {
		LOG_ERR("VID region erased check failed for PEB %zu", pnum);
		goto classify_bad;
	}

	if (!vid_erased) {
		return SCAN_NEXT_STEP;
	}

	/* VID erased — check LEB prefix to distinguish free from uncommitted. */
	bool leb_erased = false;

	ret = ubi_secure_leb_prefix_is_erased(&dev->mtd, pnum, &leb_erased);
	if (ret != 0) {
		LOG_ERR("LEB prefix erased check failed for PEB %zu", pnum);
		goto classify_bad;
	}

	struct ubi_rbt_item *item = NULL;

	ret = ubi_mem_leaf_alloc((void **)&item);
	if (ret != 0) {
		LOG_ERR("Leaf item allocation failure");
		return ret;
	}

	item->key = ec_hdr->ec;
	item->value.pnum = pnum;

	if (leb_erased) {
		rb_insert(&dev->free_pebs, &item->node);
		dev->free_peb_count++;
	} else {
		LOG_WRN("PEB %zu: erased VID but non-erased LEB prefix — dirty", pnum);
		rb_insert(&dev->dirty_pebs, &item->node);
		dev->dirty_peb_count++;
	}

	/* clang-format off */
	return SCAN_PEB_HANDLED;

classify_bad: {
	/* clang-format on */
	struct ubi_list_item *bad = NULL;
	const int alloc_ret = ubi_mem_leaf_alloc((void **)&bad);

	if (alloc_ret != 0) {
		LOG_ERR("Leaf item allocation failure");
		return alloc_ret;
	}

	ubi_move_to_bad_blocks(dev, pnum, ec_hdr->ec, bad);
	return SCAN_PEB_HANDLED;
}
}

/**
 * \brief Classify an orphan PEB (volume deleted) by moving it to the dirty pool.
 *
 * Hidden anchor PEBs (INTERNAL_ANCHOR_LNUM) for deleted volumes are also
 * classified as orphans and sent to the dirty pool.
 */
static int scan_classify_orphan(struct ubi_device *dev, size_t pnum,
				const struct ubi_ec_hdr *ec_hdr, const struct ubi_vid_hdr *vid_hdr)
{
	__ASSERT_NO_MSG(dev != NULL);
	__ASSERT_NO_MSG(ec_hdr != NULL);
	__ASSERT_NO_MSG(vid_hdr != NULL);

	const struct ubi_rbt_item *vol_entry = ubi_cache_search(&dev->vols, vid_hdr->vol_id);

	if (vol_entry != NULL) {
		return SCAN_NEXT_STEP;
	}

	struct ubi_rbt_item *item = NULL;
	const int ret = ubi_mem_leaf_alloc((void **)&item);

	if (ret != 0) {
		LOG_ERR("Leaf item allocation failure");
		return ret;
	}

	item->key = ec_hdr->ec;
	item->value.pnum = pnum;
	rb_insert(&dev->dirty_pebs, &item->node);
	dev->dirty_peb_count++;

	return SCAN_PEB_HANDLED;
}

/**
 * \brief Map a LEB that appears for the first time into the volume EBA table.
 *
 * Hidden anchor PEBs (lnum == INTERNAL_ANCHOR_LNUM) are bound to the
 * volume via vol->anchor_pnum instead of the EBA table.
 */
static int scan_map_first(struct ubi_device *dev, size_t pnum, const struct ubi_ec_hdr *ec_hdr,
			  const struct ubi_vid_hdr *vid_hdr, struct ubi_volume *vol)
{
	__ASSERT_NO_MSG(dev != NULL);
	__ASSERT_NO_MSG(ec_hdr != NULL);
	__ASSERT_NO_MSG(vid_hdr != NULL);
	__ASSERT_NO_MSG(vol != NULL);

	/* Hidden anchor PEB — track in volume, not in EBA table. */
	if (vid_hdr->lnum == UBI_SECURE_INTERNAL_ANCHOR_LNUM) {
		if (vol->anchor_pnum != SIZE_MAX) {
			/* Duplicate anchor — keep the one with higher sqnum. */
			struct ubi_ec_hdr old_ec = { 0 };
			struct ubi_secure_ec_auth_ctx old_ec_ctx = { 0 };
			struct ubi_vid_hdr old_vid = { 0 };
			struct ubi_vid_secure_meta old_vid_meta = { 0 };
			struct ubi_secure_vid_auth_ctx old_vid_ctx = { 0 };

			int ret = ubi_secure_ec_hdr_read(&dev->mtd, dev->crypto_cfg,
							 vol->anchor_pnum, &old_ec, &old_ec_ctx);
			if (ret != 0) {
				/* Old anchor unreadable — replace with current. */
				goto replace_anchor;
			}

			ret = ubi_secure_vid_hdr_read(&dev->mtd, dev->crypto_cfg, vol->anchor_pnum,
						      &old_ec_ctx, &old_vid, &old_vid_meta,
						      &old_vid_ctx);
			if (ret != 0) {
				goto replace_anchor;
			}

			if (vid_hdr->sqnum > old_vid.sqnum) {
				goto replace_anchor;
			}

			/* Current PEB is older — discard to dirty. */
			struct ubi_rbt_item *item = NULL;

			ret = ubi_mem_leaf_alloc((void **)&item);
			if (ret != 0) {
				return ret;
			}

			item->key = ec_hdr->ec;
			item->value.pnum = pnum;
			rb_insert(&dev->dirty_pebs, &item->node);
			dev->dirty_peb_count++;
			/* clang-format off */
			return SCAN_PEB_HANDLED;

replace_anchor: {
	/* clang-format on */
	/* Move old anchor PEB to dirty. */
	struct ubi_rbt_item *old_item = NULL;

	ret = ubi_mem_leaf_alloc((void **)&old_item);
	if (ret != 0) {
		return ret;
	}

	old_item->key = (ret == 0) ? old_ec.ec : ec_hdr->ec;
	old_item->value.pnum = vol->anchor_pnum;
	rb_insert(&dev->dirty_pebs, &old_item->node);
	dev->dirty_peb_count++;
}
		}

		vol->anchor_pnum = pnum;
		return SCAN_PEB_HANDLED;
	}

	const struct ubi_rbt_item *existing = ubi_cache_search(&vol->eba_tbl, vid_hdr->lnum);

	if (existing != NULL) {
		return SCAN_NEXT_STEP;
	}

	struct ubi_rbt_item *item = NULL;
	const int ret = ubi_mem_leaf_alloc((void **)&item);

	if (ret != 0) {
		LOG_ERR("Leaf item allocation failure");
		return ret;
	}

	if (vid_hdr->lnum >= vol->cfg.leb_count) {
		item->key = ec_hdr->ec;
		item->value.pnum = pnum;
		rb_insert(&dev->dirty_pebs, &item->node);
		dev->dirty_peb_count++;
		return SCAN_PEB_HANDLED;
	}

	item->key = vid_hdr->lnum;
	item->value.pnum = pnum;
	rb_insert(&vol->eba_tbl, &item->node);
	vol->eba_tbl_count++;

	return SCAN_PEB_HANDLED;
}

/**
 * \brief Resolve a duplicate LEB mapping by comparing sequence numbers.
 */
static int scan_resolve_dup(struct ubi_device *dev, size_t pnum, size_t ec_avg,
			    const struct ubi_ec_hdr *ec_hdr, const struct ubi_vid_hdr *vid_hdr,
			    struct ubi_volume *vol, struct ubi_rbt_item *existing)
{
	__ASSERT_NO_MSG(dev != NULL);
	__ASSERT_NO_MSG(ec_hdr != NULL);
	__ASSERT_NO_MSG(vid_hdr != NULL);
	__ASSERT_NO_MSG(vol != NULL);
	__ASSERT_NO_MSG(existing != NULL);

	struct ubi_rbt_item *item = NULL;
	int ret = ubi_mem_leaf_alloc((void **)&item);

	if (ret != 0) {
		LOG_ERR("Leaf item allocation failure");
		return ret;
	}

	/* Read EC and VID of the existing PEB. */
	struct ubi_ec_hdr exist_ec = { 0 };
	struct ubi_secure_ec_auth_ctx exist_ec_ctx = { 0 };

	ret = ubi_secure_ec_hdr_read(&dev->mtd, dev->crypto_cfg, existing->value.pnum, &exist_ec,
				     &exist_ec_ctx);
	if (ret != 0) {
		rb_remove(&vol->eba_tbl, &existing->node);
		vol->eba_tbl_count--;

		const size_t bad_pnum = existing->value.pnum;
		struct ubi_list_item *bad = ubi_leaf_as_list(existing);

		ubi_move_to_bad_blocks(dev, bad_pnum, ec_avg, bad);

		item->key = vid_hdr->lnum;
		item->value.pnum = pnum;
		rb_insert(&vol->eba_tbl, &item->node);
		vol->eba_tbl_count++;

		return SCAN_PEB_HANDLED;
	}

	struct ubi_vid_hdr exist_vid = { 0 };
	struct ubi_vid_secure_meta exist_vid_meta = { 0 };
	struct ubi_secure_vid_auth_ctx exist_vid_ctx = { 0 };

	ret = ubi_secure_vid_hdr_read(&dev->mtd, dev->crypto_cfg, existing->value.pnum,
				      &exist_ec_ctx, &exist_vid, &exist_vid_meta, &exist_vid_ctx);
	if (ret != 0) {
		rb_remove(&vol->eba_tbl, &existing->node);
		vol->eba_tbl_count--;

		const size_t bad_pnum = existing->value.pnum;
		struct ubi_list_item *bad = ubi_leaf_as_list(existing);

		ubi_move_to_bad_blocks(dev, bad_pnum, ec_hdr->ec, bad);

		item->key = vid_hdr->lnum;
		item->value.pnum = pnum;
		rb_insert(&vol->eba_tbl, &item->node);
		vol->eba_tbl_count++;

		return SCAN_PEB_HANDLED;
	}

	if (vid_hdr->sqnum < exist_vid.sqnum) {
		/* Current PEB is older — discard to dirty pool. */
		item->key = ec_hdr->ec;
		item->value.pnum = pnum;
		rb_insert(&dev->dirty_pebs, &item->node);
		dev->dirty_peb_count++;
	} else {
		/* Current PEB is newer — replace the existing mapping. */
		rb_remove(&vol->eba_tbl, &existing->node);
		vol->eba_tbl_count--;

		existing->key = exist_ec.ec;
		rb_insert(&dev->dirty_pebs, &existing->node);
		dev->dirty_peb_count++;

		item->key = vid_hdr->lnum;
		item->value.pnum = pnum;
		rb_insert(&vol->eba_tbl, &item->node);
		vol->eba_tbl_count++;
	}

	return SCAN_PEB_HANDLED;
}

/**
 * \brief Scan all data PEBs — classify into free, dirty, bad, or EBA entries.
 */
static int init_scan_data_pebs(struct ubi_device *ubi_dev, size_t nr_of_pebs, size_t ec_avg)
{
	__ASSERT_NO_MSG(ubi_dev != NULL);

	for (size_t pnum = UBI_DEV_HDR_NR_OF_RES_PEBS; pnum < nr_of_pebs; pnum++) {
		struct ubi_ec_hdr ec_hdr = { 0 };
		struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };

		int ret = scan_validate_ec(ubi_dev, pnum, ec_avg, &ec_hdr, &ec_ctx);

		if (ret < 0) {
			return ret;
		}
		if (ret == SCAN_PEB_HANDLED) {
			continue;
		}

		/* Track key-version PEB refcount from EC header. */
		ubi_secure_key_refcount_inc(ubi_dev, ec_ctx.key_version);

		/* Track max EC-domain AEAD counter for nonce monotonicity. */
		if (ec_ctx.aead_counter >= ubi_dev->next_ec_counter) {
			ubi_dev->next_ec_counter = ec_ctx.aead_counter + 1;
		}

		/* Check if VID region is erased — classifies free/dirty. */
		ret = scan_classify_vid_region(ubi_dev, pnum, &ec_hdr);
		if (ret < 0) {
			return ret;
		}
		if (ret == SCAN_PEB_HANDLED) {
			continue;
		}

		/* VID region is not erased — read and authenticate the VID header. */
		struct ubi_vid_hdr vid_hdr = { 0 };
		struct ubi_vid_secure_meta vid_meta = { 0 };
		struct ubi_secure_vid_auth_ctx vid_ctx = { 0 };

		ret = ubi_secure_vid_hdr_read(&ubi_dev->mtd, ubi_dev->crypto_cfg, pnum, &ec_ctx,
					      &vid_hdr, &vid_meta, &vid_ctx);
		if (ret != 0) {
			LOG_ERR("VID header auth failure for PEB %zu — marking bad", pnum);
			struct ubi_list_item *bad = NULL;

			ret = ubi_mem_leaf_alloc((void **)&bad);
			if (ret != 0) {
				LOG_ERR("Leaf item allocation failure");
				return ret;
			}

			ubi_move_to_bad_blocks(ubi_dev, pnum, ec_hdr.ec, bad);
			continue;
		}

		/* Track VID+LEB key refcount (2 objects per VID-bearing PEB). */
		ubi_secure_key_refcount_inc(ubi_dev, vid_ctx.key_version);
		ubi_secure_key_refcount_inc(ubi_dev, vid_ctx.key_version);

		/* Track global sequence number. */
		if (vid_hdr.sqnum > ubi_dev->global_sqnum) {
			ubi_dev->global_sqnum = vid_hdr.sqnum;
		}

		/* Track max VID counter for write-active key version. */
		if (vid_ctx.key_version ==
		    ubi_dev->crypto_cfg->policy.requested_write_key_version) {
			if (vid_ctx.vid_counter >= ubi_dev->next_vid_counter) {
				ubi_dev->next_vid_counter = vid_ctx.vid_counter + 1;
			}
		}

		/* Check if volume exists — orphan PEBs go to dirty. */
		ret = scan_classify_orphan(ubi_dev, pnum, &ec_hdr, &vid_hdr);
		if (ret < 0) {
			return ret;
		}
		if (ret == SCAN_PEB_HANDLED) {
			continue;
		}

		struct ubi_rbt_item *vol_entry = ubi_cache_search(&ubi_dev->vols, vid_hdr.vol_id);
		struct ubi_volume *vol = vol_entry->value.vol;

		ret = scan_map_first(ubi_dev, pnum, &ec_hdr, &vid_hdr, vol);
		if (ret < 0) {
			return ret;
		}
		if (ret == SCAN_PEB_HANDLED) {
			continue;
		}

		struct ubi_rbt_item *existing = ubi_cache_search(&vol->eba_tbl, vid_hdr.lnum);

		ret = scan_resolve_dup(ubi_dev, pnum, ec_avg, &ec_hdr, &vid_hdr, vol, existing);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

/**
 * \brief Format a blank device in secure mode.
 */
static int secure_format(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			 struct ubi_device *ubi_dev)
{
	__ASSERT_NO_MSG(mtd != NULL);
	__ASSERT_NO_MSG(crypto_cfg != NULL);
	__ASSERT_NO_MSG(ubi_dev != NULL);

	const struct flash_area *fa = NULL;
	int ret = flash_area_open(mtd->partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		return -EIO;
	}

	/* Build initial device header. */
	struct ubi_dev_hdr dev_hdr = {
		.magic = UBI_DEV_HDR_MAGIC,
		.version = UBI_DEV_HDR_VERSION,
		.offset = fa->fa_off,
		.size = fa->fa_size,
		.revision = 0,
		.vol_count = 0,
		.vol_id_watermark = 0,
	};

	dev_hdr.hdr_crc =
		crc32_ieee((const uint8_t *)&dev_hdr, sizeof(dev_hdr) - sizeof(dev_hdr.hdr_crc));

	const size_t fa_size_for_format = fa->fa_size;

	flash_area_close(fa);

	struct ubi_dev_secure_meta dev_meta = {
		.write_active_key_version = crypto_cfg->policy.requested_write_key_version,
		.vid_next_counter_floor = 0,
	};
	memset(dev_meta.reserved0, 0, sizeof(dev_meta.reserved0));

	/* Commit encrypted reserved PEBs. */
	ret = ubi_secure_res_peb_commit(mtd, crypto_cfg, &dev_hdr, &dev_meta, NULL, 0,
					crypto_cfg->policy.requested_write_key_version,
					ubi_dev->next_dev_hdr_counter);
	if (ret != 0 && ret != -EROFS) {
		LOG_ERR("Secure format commit failure");
		return ret;
	}

	/* Advance dev_hdr counter: 1 for dev_hdr + 0 vol headers. */
	ubi_dev->next_dev_hdr_counter += 1;

	/* Populate ubi_device fields from formatted state. */
	ubi_dev->vol_id_watermark = 0;
	ubi_dev->vol_count = 0;
	ubi_dev->read_only_degraded = (ret == -EROFS);

	/* Track reserved-PEB key version and refcount. */
	ubi_dev->reserved_key_version = crypto_cfg->policy.requested_write_key_version;
	for (size_t i = 0; i < UBI_DEV_HDR_NR_OF_RES_PEBS; i++) {
		ubi_secure_key_refcount_inc(ubi_dev,
					    crypto_cfg->policy.requested_write_key_version);
	}

	/* Format data PEBs: erase and write secure EC headers. */
	const size_t nr_of_pebs = fa_size_for_format / ubi_dev->mtd.erase_block_size;

	ret = init_format_data_pebs(ubi_dev, nr_of_pebs);
	if (ret != 0) {
		LOG_ERR("Data PEB format failure");
		return ret;
	}

	return 0;
}

/**
 * \brief Attach to an existing secure device.
 */
static int secure_attach(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			 struct ubi_device *ubi_dev, uint64_t *out_device_revision)
{
	__ASSERT_NO_MSG(mtd != NULL);
	__ASSERT_NO_MSG(crypto_cfg != NULL);
	__ASSERT_NO_MSG(ubi_dev != NULL);
	__ASSERT_NO_MSG(out_device_revision != NULL);

	/* Scan and authenticate reserved PEBs. */
	struct ubi_secure_res_peb_scan scan = { 0 };
	int ret = ubi_secure_res_peb_scan(mtd, crypto_cfg, &scan);

	if (ret != 0) {
		LOG_ERR("Secure reserved PEB scan failure");
		return ret;
	}

	if (scan.auth_count == 0) {
		LOG_ERR("No authenticated reserved PEBs found");
		return -EIO;
	}

	/* Validate allowlist: the device's write_active_key_version must be allowlisted. */
	if (!key_version_is_allowed(&crypto_cfg->policy, scan.dev_meta.write_active_key_version)) {
		LOG_ERR("On-flash write_active_key_version %u not in allowlist",
			scan.dev_meta.write_active_key_version);
		return -EACCES;
	}

	/* Authenticate volume headers and collect into RAM. */
	struct ubi_vol_hdr vol_hdrs[CONFIG_UBI_MAX_NR_OF_VOLUMES] = { 0 };

	if (scan.dev_hdr.vol_count > 0) {
		ret = ubi_secure_res_peb_read_vol_hdrs(mtd, crypto_cfg, &scan, vol_hdrs,
						       CONFIG_UBI_MAX_NR_OF_VOLUMES);
		if (ret != 0) {
			LOG_ERR("Volume header authentication failure");
			return ret;
		}
	}

	/* Populate device state from authenticated headers. */
	ubi_dev->vol_id_watermark = scan.dev_hdr.vol_id_watermark;
	ubi_dev->read_only_degraded = (scan.auth_count < UBI_SECURE_RES_PEB_NR_ACTIVE);
	ubi_dev->next_vid_counter = scan.dev_meta.vid_next_counter_floor;

	/* Recover reserved-PEB AEAD counter from the canonical device header prefix.
	 * The last commit used counter values [c, c+1, .., c+vol_count].
	 * Next available = c + 1 + vol_count. */
	const uint64_t dev_hdr_counter = ubi_secure_decode_counter48(scan.dev_prefix.counter);

	ubi_dev->next_dev_hdr_counter = dev_hdr_counter + 1 + scan.dev_hdr.vol_count;

	*out_device_revision = scan.dev_hdr.revision;

	/* Eagerly upgrade reserved PEBs when the requested write key version
	 * differs from what is on flash.  This ensures KEY_RETIRABLE can fire
	 * as soon as all data PEBs are cleaned up, without waiting for a
	 * volume mutation to trigger the upgrade. */
	const uint8_t new_kv = crypto_cfg->policy.requested_write_key_version;

	if (new_kv != scan.dev_prefix.key_version) {
		struct ubi_dev_hdr upd_hdr = scan.dev_hdr;
		struct ubi_dev_secure_meta upd_meta = scan.dev_meta;

		upd_hdr.revision += 1;
		upd_hdr.hdr_crc = crc32_ieee((const uint8_t *)&upd_hdr,
					     sizeof(upd_hdr) - sizeof(upd_hdr.hdr_crc));
		upd_meta.write_active_key_version = new_kv;

		ret = ubi_secure_res_peb_commit(mtd, crypto_cfg, &upd_hdr, &upd_meta, vol_hdrs,
						scan.dev_hdr.vol_count, new_kv,
						ubi_dev->next_dev_hdr_counter);
		if (ret == -EROFS) {
			LOG_WRN("Reserved PEB bank degraded during key upgrade");
			ubi_dev->read_only_degraded = true;
			/* At least one bank succeeded — treat as upgraded. */
			ubi_dev->next_dev_hdr_counter += 1 + scan.dev_hdr.vol_count;
			*out_device_revision = upd_hdr.revision;
			ubi_dev->reserved_key_version = new_kv;
			for (size_t i = 0; i < UBI_DEV_HDR_NR_OF_RES_PEBS; i++) {
				ubi_secure_key_refcount_inc(ubi_dev, new_kv);
			}
		} else if (ret != 0) {
			/* Key may not be provisioned yet — defer upgrade. */
			LOG_WRN("Key upgrade deferred: commit failed (%d)", ret);
			ubi_dev->reserved_key_version = scan.dev_prefix.key_version;
			for (size_t i = 0; i < scan.auth_count; i++) {
				ubi_secure_key_refcount_inc(ubi_dev, scan.dev_prefix.key_version);
			}
		} else {
			ubi_dev->next_dev_hdr_counter += 1 + scan.dev_hdr.vol_count;
			*out_device_revision = upd_hdr.revision;
			ubi_dev->reserved_key_version = new_kv;
			for (size_t i = 0; i < UBI_DEV_HDR_NR_OF_RES_PEBS; i++) {
				ubi_secure_key_refcount_inc(ubi_dev, new_kv);
			}
		}
	} else {
		ubi_dev->reserved_key_version = scan.dev_prefix.key_version;
		for (size_t i = 0; i < scan.auth_count; i++) {
			ubi_secure_key_refcount_inc(ubi_dev, scan.dev_prefix.key_version);
		}
	}

	/* Collect volumes into RAM — vol_count is incremented per-insert. */
	ret = init_collect_volumes(ubi_dev, vol_hdrs, scan.dev_hdr.vol_count);
	if (ret != 0) {
		LOG_ERR("Volume collection failure");
		return ret;
	}

	return 0;
}

/* Public function ----------------------------------------------------------------------------- */

int ubi_secure_device_init(const struct ubi_mtd *mtd, const struct ubi_crypto_config *crypto_cfg,
			   struct ubi_device **ubi)
{
	__ASSERT_NO_MSG(mtd != NULL);
	__ASSERT_NO_MSG(crypto_cfg != NULL);
	__ASSERT_NO_MSG(ubi != NULL);

	int ret = validate_crypto_cfg(crypto_cfg);

	if (ret != 0) {
		LOG_ERR("Crypto config validation failed");
		*ubi = NULL;
		return ret;
	}

	/* Initialize PSA crypto subsystem. */
	const psa_status_t psa_ret = psa_crypto_init();

	if (psa_ret != PSA_SUCCESS) {
		LOG_ERR("PSA crypto init failure: %d", (int)psa_ret);
		*ubi = NULL;
		return -EIO;
	}

	/* Validate that the write key version is allowlisted. */
	if (!key_version_is_allowed(&crypto_cfg->policy,
				    crypto_cfg->policy.requested_write_key_version)) {
		LOG_ERR("Requested write key version %u not in allowlist",
			crypto_cfg->policy.requested_write_key_version);
		*ubi = NULL;
		return -EINVAL;
	}

	/* Acquire partition. */
	ret = ubi_partition_acquire(mtd->partition_id);
	if (ret != 0) {
		LOG_ERR("Partition %u already in use", mtd->partition_id);
		*ubi = NULL;
		return -EBUSY;
	}

	/* Allocate device structure. */
	struct ubi_device *ubi_dev = NULL;

	ret = ubi_mem_device_alloc(&ubi_dev);
	if (ret != 0) {
		LOG_ERR("Device allocation failure");
		ubi_partition_release(mtd->partition_id);
		*ubi = NULL;
		return ret;
	}

	k_mutex_init(&ubi_dev->mutex);
	ubi_dev->mtd = *mtd;
	ubi_dev->mode = UBI_MODE_SECURE;
	ubi_dev->ops = ubi_secure_backend();
	ubi_dev->crypto_cfg = crypto_cfg;
	ubi_dev->free_pebs.lessthan_fn = ubi_cache_cmp;
	ubi_dev->dirty_pebs.lessthan_fn = ubi_cache_cmp;
	sys_slist_init(&ubi_dev->bad_pebs);
	ubi_dev->vols.lessthan_fn = ubi_cache_cmp;

	/* Validate flash geometry. */
	const struct flash_area *fa = NULL;

	ret = flash_area_open(ubi_dev->mtd.partition_id, &fa);
	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		goto exit;
	}

	if (!device_is_ready(flash_area_get_device(fa))) {
		LOG_ERR("Flash area is not ready");
		flash_area_close(fa);
		ret = -ENODEV;
		goto exit;
	}

	/* Validate flash geometry. */
	if (ubi_dev->mtd.write_block_size == 0 || ubi_dev->mtd.erase_block_size == 0) {
		LOG_ERR("Invalid geometry: write_block_size or erase_block_size is zero");
		flash_area_close(fa);
		ret = -EINVAL;
		goto exit;
	}

	if (fa->fa_size % ubi_dev->mtd.erase_block_size != 0) {
		LOG_ERR("Partition size not a multiple of erase block size");
		flash_area_close(fa);
		ret = -EINVAL;
		goto exit;
	}

	if (ubi_dev->mtd.erase_block_size % ubi_dev->mtd.write_block_size != 0) {
		LOG_ERR("Erase block size not a multiple of write block size");
		flash_area_close(fa);
		ret = -EINVAL;
		goto exit;
	}

	if (ubi_dev->mtd.write_block_size > WRITE_BLOCK_SIZE_ALIGNMENT) {
		LOG_ERR("write_block_size %zu exceeds max supported alignment %d",
			ubi_dev->mtd.write_block_size, WRITE_BLOCK_SIZE_ALIGNMENT);
		flash_area_close(fa);
		ret = -EINVAL;
		goto exit;
	}

	const size_t nr_of_pebs = fa->fa_size / ubi_dev->mtd.erase_block_size;

	if (nr_of_pebs <= UBI_DEV_HDR_NR_OF_RES_PEBS) {
		LOG_ERR("Partition too small: need > %d PEBs", UBI_DEV_HDR_NR_OF_RES_PEBS);
		flash_area_close(fa);
		ret = -EINVAL;
		goto exit;
	}

	if (ubi_dev->mtd.erase_block_size < (UBI_SECURE_LEB_OFFSET + UBI_SECURE_LEB_OVERHEAD)) {
		LOG_ERR("Erase block too small for secure headers + LEB overhead");
		flash_area_close(fa);
		ret = -EINVAL;
		goto exit;
	}

	flash_area_close(fa);

	/* Cache geometry for fast internal lookups. */
	ubi_dev->total_data_peb_count = nr_of_pebs - UBI_DEV_HDR_NR_OF_RES_PEBS;

#if defined(CONFIG_UBI_CRYPTO_LEB_CHUNKED)
	/* Chunked-mode geometry check.
	 * chunk_size must be a multiple of the flash write alignment.
	 * leb_size accounts for per-chunk tag overhead:
	 *   payload_space = erase_block_size - LEB_OFFSET - PREFIX_SIZE
	 *   n_full = payload_space / (chunk_size + TAG_SIZE)
	 *   remaining = payload_space - n_full * (chunk_size + TAG_SIZE)
	 *   leb_size = n_full * chunk_size + max(0, remaining - TAG_SIZE) */
	{
		const size_t chunk_size = CONFIG_UBI_CRYPTO_LEB_CHUNK_SIZE;

		if (chunk_size % ubi_dev->mtd.write_block_size != 0) {
			LOG_ERR("Chunk size %zu not aligned to write block size %zu", chunk_size,
				ubi_dev->mtd.write_block_size);
			ret = -EINVAL;
			goto exit;
		}

		const size_t payload_space = ubi_dev->mtd.erase_block_size - UBI_SECURE_LEB_OFFSET -
					     UBI_SECURE_PREFIX_SIZE;
		const size_t n_full = payload_space / (chunk_size + UBI_SECURE_TAG_SIZE);
		const size_t remaining =
			payload_space - n_full * (chunk_size + UBI_SECURE_TAG_SIZE);

		if (remaining > UBI_SECURE_TAG_SIZE) {
			ubi_dev->leb_size = n_full * chunk_size + (remaining - UBI_SECURE_TAG_SIZE);
		} else {
			ubi_dev->leb_size = n_full * chunk_size;
		}

		if (ubi_dev->leb_size == 0) {
			LOG_ERR("Chunked geometry invalid: no payload space "
				"(erase_block=%zu chunk=%zu)",
				ubi_dev->mtd.erase_block_size, chunk_size);
			ret = -EINVAL;
			goto exit;
		}
	}
#else
	ubi_dev->leb_size =
		ubi_dev->mtd.erase_block_size - UBI_SECURE_LEB_OFFSET - UBI_SECURE_LEB_OVERHEAD;
#endif

	/* Detect mode: blank, secure, or plain. */
	bool any_blank = false;
	bool any_secure = false;
	bool any_plain = false;

	ret = detect_reserved_mode(mtd, &any_blank, &any_secure, &any_plain);
	if (ret != 0) {
		LOG_ERR("Reserved PEB mode detection failed");
		goto exit;
	}

	/* Mode mismatch: plain media + secure config. */
	if (any_plain) {
		LOG_ERR("Plain media detected but secure config provided — mode mismatch");
		ret = -EPROTO;
		goto exit;
	}

	uint64_t device_revision = 0;

	if (any_secure) {
		/* Existing secure media → attach. */
		ret = secure_attach(mtd, crypto_cfg, ubi_dev, &device_revision);
	} else {
		/* All blank → format. */
		ret = secure_format(mtd, crypto_cfg, ubi_dev);
	}

	if (ret != 0) {
		LOG_ERR("Secure %s failed: %d", any_secure ? "attach" : "format", ret);
		goto exit;
	}

	ubi_dev->cached_device_revision = device_revision;

	/* Compute average erase counter. */
	init_compute_ec_average(ubi_dev, nr_of_pebs);
	const size_t ec_avg = (ubi_dev->ec_count > 0) ? (ubi_dev->ec_sum / ubi_dev->ec_count) : 0;

	/* Scan all data PEBs and classify into free, dirty, bad, or EBA entries. */
	ret = init_scan_data_pebs(ubi_dev, nr_of_pebs, ec_avg);
	if (ret != 0) {
		LOG_ERR("Data PEB scan failure");
		goto exit;
	}

	/* Re-create missing hidden anchors for orphaned volumes.
	 * After a failed anchor_create during volume_create, or after anchor PEB
	 * corruption, the volume exists in reserved PEB metadata but has no live
	 * anchor on flash.  Re-create it now if free PEBs are available. */
	if (any_secure) {
		struct ubi_rbt_item *vol_entry = NULL;

		RB_FOR_EACH_CONTAINER(&ubi_dev->vols, vol_entry, node)
		{
			struct ubi_volume *vol = vol_entry->value.vol;

			if (vol->anchor_pnum == SIZE_MAX && ubi_dev->free_peb_count > 0) {
				LOG_WRN("Volume %u missing anchor — re-creating", vol->vol_id);
				ret = ubi_secure_anchor_create(ubi_dev, vol);
				if (ret != 0) {
					LOG_ERR("Anchor re-creation failed for vol %u",
						vol->vol_id);
					goto exit;
				}
			}
		}
	}

	/* Ensure next sqnum is strictly greater than any existing one. */
	ubi_dev->global_sqnum += 1;

	/* Freshness check — now that global_sqnum reflects all data PEBs. */
	if (any_secure) {
		const struct ubi_crypto_freshness freshness = {
			.device_revision = device_revision,
			.global_sqnum = ubi_dev->global_sqnum,
		};

		const enum ubi_crypto_rollback_verdict verdict =
			crypto_cfg->check_freshness(&freshness, crypto_cfg->user_data);

		if (verdict == UBI_CRYPTO_ROLLBACK_REJECT
#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)
		    || ubi_secure_test_hook_check(UBI_SECURE_HOOK_FRESHNESS_REJECT)
#endif
		) {
			LOG_ERR("Freshness check rejected — rollback detected");
			struct ubi_crypto_event ev = {
				.type = UBI_CRYPTO_EVENT_ROLLBACK_POLICY_MISMATCH,
				.freshness = freshness,
				.rollback = { ._reserved = 0 },
			};
			ubi_secure_emit_event(ubi_dev, &ev);
#if defined(CONFIG_UBI_CRYPTO_STRICT_RO_ON_POLICY_FAILURE)
			ubi_dev->read_only_crypto = true;
#endif
			ret = -EACCES;
			goto exit;
		}
	}

	*ubi = ubi_dev;
	return 0;

exit:
	ubi_secure_device_deinit(ubi_dev);
	*ubi = NULL;
	return ret;
}
