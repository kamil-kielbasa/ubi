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
#include "ubi_secure_io.h"
#include "ubi_secure_reserved.h"
#include "ubi_secure_types.h"
#include "ubi_internal.h"
#include "ubi_io.h"
#include "ubi_mem.h"
#include "ubi_partition_guard.h"

#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/storage/flash_map.h>

#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------ */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Static function declarations ---------------------------------------------------------------- */

static void torture_bad_blocks(struct ubi_device *ubi);

/* Static function definitions ----------------------------------------------------------------- */

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

		/* Read authentic EC to get current counter. */
		struct ubi_ec_hdr ec_hdr = { 0 };
		struct ubi_secure_ec_auth_ctx ec_ctx = { 0 };

		ret = ubi_secure_ec_hdr_read(&ubi->mtd, ubi->crypto_cfg, entry->value.pnum, &ec_hdr,
					     &ec_ctx);
		if (ret != 0) {
			LOG_ERR("EC header read failure for PEB %zu", (size_t)entry->value.pnum);

			const size_t pnum = entry->value.pnum;
			const size_t ec = entry->key;

			rb_remove(&ubi->dirty_pebs, &entry->node);
			ubi->dirty_peb_count--;

			ubi->ec_sum -= ec;
			ubi->ec_count--;

			struct ubi_list_item *bad_item = ubi_leaf_as_list(entry);

			ubi_move_to_bad_blocks(ubi, pnum, ec, bad_item);
			goto exit;
		}

		/* Erase the PEB. */
		const struct flash_area *fa = NULL;

		ret = flash_area_open(ubi->mtd.partition_id, &fa);
		if (ret != 0) {
			LOG_ERR("Flash area open failure");
			goto exit;
		}

		const size_t offset = entry->value.pnum * ubi->mtd.erase_block_size;

		ret = flash_area_erase(fa, offset, ubi->mtd.erase_block_size);
		flash_area_close(fa);

		if (ret != 0) {
			LOG_ERR("Flash erase failure");

			const size_t pnum = entry->value.pnum;
			const size_t ec = entry->key;

			rb_remove(&ubi->dirty_pebs, &entry->node);
			ubi->dirty_peb_count--;

			ubi->ec_sum -= ec;
			ubi->ec_count--;

			struct ubi_list_item *bad_item = ubi_leaf_as_list(entry);

			ubi_move_to_bad_blocks(ubi, pnum, ec, bad_item);
			goto exit;
		}

		/* Increment EC and write new secure EC header. */
		ec_hdr.ec += 1;

		const uint8_t write_kv = ubi->crypto_cfg->policy.requested_write_key_version;

		ret = ubi_secure_ec_hdr_write(&ubi->mtd, ubi->crypto_cfg, entry->value.pnum,
					      &ec_hdr, write_kv, 0);
		if (ret != 0) {
			LOG_ERR("EC header write failure");

			const size_t pnum = entry->value.pnum;
			const size_t ec = entry->key;

			rb_remove(&ubi->dirty_pebs, &entry->node);
			ubi->dirty_peb_count--;

			ubi->ec_sum -= ec;
			ubi->ec_count--;

			struct ubi_list_item *bad_item = ubi_leaf_as_list(entry);

			ubi_move_to_bad_blocks(ubi, pnum, ec, bad_item);
			goto exit;
		}

		/* Move from dirty to free. */
		rb_remove(&ubi->dirty_pebs, &entry->node);
		ubi->dirty_peb_count--;

		ubi->ec_sum += 1;

		entry->key = ec_hdr.ec;
		rb_insert(&ubi->free_pebs, &entry->node);
		ubi->free_peb_count++;
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
