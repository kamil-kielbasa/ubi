/**
 * \file    ubi_plain_core_runtime.c
 * \author  Kamil Kielbasa
 * \brief   UBI device runtime: get_info, erase_peb, deinit, test API.
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files -------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_internal.h"
#include "ubi_plain_io.h"
#include "ubi_plain_ops.h"
#include "ubi_mem.h"
#include "ubi_partition_guard.h"

/* Zephyr headers: */
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <zephyr/storage/flash_map.h>

/* Standard library headers: */
#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

/* Static function declarations ----------------------------------------------------------------- */

/**
 * \brief Attempt to recover bad PEBs by performing erase-only torture.
 *
 * Up to CONFIG_UBI_BAD_PEB_TORTURE_CYCLES bad PEBs are tested per call.
 * Each PEB is erased up to CONFIG_UBI_BAD_PEB_TORTURE_MAX_PER_ERASE times;
 * the first successful erase recovers the PEB to the free pool with ec = ec_avg.
 * If all attempts fail, the PEB remains in the bad list.
 *
 * \param[in,out] ubi  UBI device handle (caller holds mutex).
 */
static void torture_bad_blocks(struct ubi_device *ubi);

/* Module interface function definitions -------------------------------------------------------- */

int ubi_plain_device_get_info(struct ubi_device *ubi, struct ubi_device_info *info)
{
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

static void torture_bad_blocks(struct ubi_device *ubi)
{
	__ASSERT_NO_MSG(ubi);

	const struct flash_area *fa = NULL;
	int ret = flash_area_open(ubi->flash.partition_id, &fa);

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

		const size_t offset = item->pnum * ubi->flash.erase_block_size;
		bool passed = false;

		for (size_t i = 0; i < CONFIG_UBI_BAD_PEB_TORTURE_MAX_PER_ERASE; ++i) {
			ret = flash_area_erase(fa, offset, ubi->flash.erase_block_size);

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
			ec_hdr.hdr_crc = crc32_ieee((const uint8_t *)&ec_hdr,
						    sizeof(ec_hdr) - sizeof(ec_hdr.hdr_crc));

			ret = ubi_ec_hdr_write(&ubi->flash, item->pnum, &ec_hdr);

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

int ubi_plain_device_erase_peb(struct ubi_device *ubi)
{
	k_mutex_lock(&ubi->mutex, K_FOREVER);

	int ret = ubi_mutation_allowed(ubi, UBI_MUT_MAINTENANCE);

	if (ret != 0) {
		LOG_ERR("Mutation blocked: maintenance operations not allowed");
		goto exit;
	}

	if (ubi->dirty_peb_count > 0) {
		struct rbnode *node = rb_get_min(&ubi->dirty_pebs);
		struct ubi_rbt_item *entry = CONTAINER_OF(node, struct ubi_rbt_item, node);

		struct ubi_ec_hdr ec_hdr = { 0 };
		ret = ubi_ec_hdr_read(&ubi->flash, entry->value.pnum, &ec_hdr);

		if (ret != 0) {
			LOG_ERR("EC header read failure");

			const size_t pnum = entry->value.pnum;
			const size_t ec = entry->key;

			rb_remove(&ubi->dirty_pebs, &entry->node);
			ubi->dirty_peb_count -= 1;

			ubi->ec_sum -= ec;
			ubi->ec_count -= 1;

			struct ubi_list_item *bad_item = ubi_leaf_as_list(entry);
			ubi_move_to_bad_blocks(ubi, pnum, ec, bad_item);

			goto exit;
		}

		const struct flash_area *fa = NULL;
		ret = flash_area_open(ubi->flash.partition_id, &fa);

		if (ret != 0) {
			LOG_ERR("Flash area open failure");
			goto exit;
		}

		const size_t offset = entry->value.pnum * ubi->flash.erase_block_size;
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
		if (ubi_test_flash_erase_check_fail()) {
			ret = -EIO;
		} else {
			ret = flash_area_erase(fa, offset, ubi->flash.erase_block_size);
		}
#else /* !CONFIG_UBI_TEST_FAULT_INJECTION */
		ret = flash_area_erase(fa, offset, ubi->flash.erase_block_size);
#endif /* CONFIG_UBI_TEST_FAULT_INJECTION */
		flash_area_close(fa);

		if (ret != 0) {
			LOG_ERR("Flash erase failure");

			const size_t pnum = entry->value.pnum;
			const size_t ec = entry->key;

			rb_remove(&ubi->dirty_pebs, &entry->node);
			ubi->dirty_peb_count -= 1;

			ubi->ec_sum -= ec;
			ubi->ec_count -= 1;

			struct ubi_list_item *bad_item = ubi_leaf_as_list(entry);
			ubi_move_to_bad_blocks(ubi, pnum, ec, bad_item);

			goto exit;
		}

		ec_hdr.ec += 1;
		ec_hdr.hdr_crc = crc32_ieee((const uint8_t *)&ec_hdr,
					    sizeof(ec_hdr) - sizeof(ec_hdr.hdr_crc));
		ret = ubi_ec_hdr_write(&ubi->flash, entry->value.pnum, &ec_hdr);

		if (ret != 0) {
			LOG_ERR("EC header write failure");

			const size_t pnum = entry->value.pnum;
			const size_t ec = entry->key;

			rb_remove(&ubi->dirty_pebs, &entry->node);
			ubi->dirty_peb_count -= 1;

			ubi->ec_sum -= ec;
			ubi->ec_count -= 1;

			struct ubi_list_item *bad_item = ubi_leaf_as_list(entry);
			ubi_move_to_bad_blocks(ubi, pnum, ec, bad_item);

			goto exit;
		}

		rb_remove(&ubi->dirty_pebs, &entry->node);
		ubi->dirty_peb_count -= 1;

		ubi->ec_sum += 1;

		entry->key = ec_hdr.ec;
		rb_insert(&ubi->free_pebs, &entry->node);
		ubi->free_peb_count += 1;
	}

exit:
	if (ubi->bad_peb_count > 0) {
		torture_bad_blocks(ubi);
	}

	/* If the reserved PEB bank is degraded, attempt recovery.
	 * validate() scans all reserved PEBs and tries to erase+rewrite
	 * any that are not active. If recovery succeeds, clear the flag. */
	if (ubi->read_only_degraded) {
		struct ubi_dev_hdr dev_hdr = { 0 };
		int rc = ubi_dev_hdr_read(&ubi->flash, &dev_hdr);

		if (rc == 0) {
			LOG_INF("Reserved PEB bank recovered, leaving degraded mode");
			ubi->read_only_degraded = false;
		}
	}

	k_mutex_unlock(&ubi->mutex);
	return ret;
}

int ubi_plain_device_deinit(struct ubi_device *ubi)
{
	k_mutex_lock(&ubi->mutex, K_FOREVER);

	struct rbnode *node = NULL;
	struct ubi_rbt_item *rbt_item = NULL;
	struct ubi_rbt_item *vol_item = NULL;

	struct ubi_list_item *list_item = NULL;
	struct ubi_list_item *list_next = NULL;

	while ((node = rb_get_min(&ubi->free_pebs))) {
		rbt_item = CONTAINER_OF(node, struct ubi_rbt_item, node);
		rb_remove(&ubi->free_pebs, &rbt_item->node);
		ubi_mem_leaf_free(rbt_item);
		ubi->free_peb_count -= 1;
	}

	while ((node = rb_get_min(&ubi->dirty_pebs))) {
		rbt_item = CONTAINER_OF(node, struct ubi_rbt_item, node);
		rb_remove(&ubi->dirty_pebs, &rbt_item->node);
		ubi_mem_leaf_free(rbt_item);
		ubi->dirty_peb_count -= 1;
	}

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&ubi->bad_pebs, list_item, list_next, node)
	{
		sys_slist_remove(&ubi->bad_pebs, NULL, &list_item->node);
		ubi_mem_leaf_free(list_item);
		ubi->bad_peb_count -= 1;
	}

	while ((node = rb_get_min(&ubi->vols))) {
		rbt_item = CONTAINER_OF(node, struct ubi_rbt_item, node);
		rb_remove(&ubi->vols, &rbt_item->node);

		struct ubi_volume *vol = rbt_item->value.vol;
		while ((node = rb_get_min(&vol->eba_tbl))) {
			vol_item = CONTAINER_OF(node, struct ubi_rbt_item, node);
			rb_remove(&vol->eba_tbl, &vol_item->node);
			ubi_mem_leaf_free(vol_item);
			vol->eba_tbl_count -= 1;
		}

		ubi_mem_volume_free(rbt_item->value.vol);
		ubi_mem_leaf_free(rbt_item);
		ubi->vol_count -= 1;
	}

	ubi_partition_release(ubi->flash.partition_id);

	ubi_mem_device_free(ubi);
	return 0;
}

#if defined(CONFIG_UBI_TEST_API_ENABLE)

static size_t rbt_count_nodes(struct rbtree *tree)
{
	size_t count = 0;
	struct ubi_rbt_item *entry = NULL;

	RB_FOR_EACH_CONTAINER(tree, entry, node)
	{
		count++;
	}
	return count;
}

int ubi_device_check_invariants(struct ubi_device *ubi)
{
	if (!ubi) {
		LOG_ERR("ubi is NULL");
		return -EINVAL;
	}

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	int ret = 0;

	/* Count all tracked PEBs. */
	const size_t free_actual = rbt_count_nodes(&ubi->free_pebs);
	const size_t dirty_actual = rbt_count_nodes(&ubi->dirty_pebs);

	size_t bad_actual = 0;
	struct ubi_list_item *bad_entry = NULL;

	SYS_SLIST_FOR_EACH_CONTAINER(&ubi->bad_pebs, bad_entry, node)
	{
		bad_actual++;
	}

	size_t mapped_total = 0;
	size_t reserved_sum = 0;
	struct ubi_rbt_item *vol_entry = NULL;

	RB_FOR_EACH_CONTAINER(&ubi->vols, vol_entry, node)
	{
		struct ubi_volume *vol = vol_entry->value.vol;
		const size_t eba_actual = rbt_count_nodes(&vol->eba_tbl);

		if (eba_actual != vol->eba_tbl_count) {
			LOG_ERR("Invariant: vol %zu eba_tbl_count=%zu actual=%zu", vol->vol_id,
				vol->eba_tbl_count, eba_actual);
			ret = -EIO;
		}

		mapped_total += eba_actual;
		reserved_sum += vol->cfg.leb_count;
	}

	if (free_actual != ubi->free_peb_count) {
		LOG_ERR("Invariant: free_peb_count=%zu actual=%zu", ubi->free_peb_count,
			free_actual);
		ret = -EIO;
	}

	if (dirty_actual != ubi->dirty_peb_count) {
		LOG_ERR("Invariant: dirty_peb_count=%zu actual=%zu", ubi->dirty_peb_count,
			dirty_actual);
		ret = -EIO;
	}

	if (bad_actual != ubi->bad_peb_count) {
		LOG_ERR("Invariant: bad_peb_count=%zu actual=%zu", ubi->bad_peb_count, bad_actual);
		ret = -EIO;
	}

	const size_t total_tracked = free_actual + dirty_actual + bad_actual + mapped_total;

	if (total_tracked != ubi->total_data_peb_count) {
		LOG_ERR("Invariant: tracked PEBs=%zu != total_data_peb_count=%zu", total_tracked,
			ubi->total_data_peb_count);
		ret = -EIO;
	}

	if (reserved_sum != ubi_reserved_peb_count(ubi)) {
		LOG_ERR("Invariant: reserved_peb_count mismatch: computed=%zu helper=%zu",
			reserved_sum, ubi_reserved_peb_count(ubi));
		ret = -EIO;
	}

	k_mutex_unlock(&ubi->mutex);
	return ret;
}

int ubi_device_get_peb_ec(struct ubi_device *ubi, size_t **peb_ec, size_t *len)
{
	int ret = -EIO;

	if (!ubi || !peb_ec || !len)
		return -EINVAL;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	const struct flash_area *fa = NULL;

	ret = flash_area_open(ubi->flash.partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		goto exit;
	}

	const size_t nr_of_pebs =
		(fa->fa_size / ubi->flash.erase_block_size) - UBI_DEV_HDR_NR_OF_RES_PEBS;

	flash_area_close(fa);

	size_t *_peb_ec = NULL;
	ret = ubi_mem_diag_alloc(nr_of_pebs * sizeof(*_peb_ec), (void **)&_peb_ec);

	if (ret != 0) {
		LOG_ERR("Diagnostic allocation failure");
		goto exit;
	}

	for (size_t pnum = 0; pnum < nr_of_pebs; ++pnum) {
		struct ubi_ec_hdr ec_hdr = { 0 };
		ret = ubi_ec_hdr_read(&ubi->flash, pnum + UBI_DEV_HDR_NR_OF_RES_PEBS, &ec_hdr);

		if (ret != 0) {
			LOG_ERR("EC header read failure");
			ubi_mem_diag_free(_peb_ec);
			goto exit;
		}

		_peb_ec[pnum] = ec_hdr.ec;
	}

	*len = nr_of_pebs;
	*peb_ec = _peb_ec;
	ret = 0;

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

void ubi_test_partition_force_release_all(void)
{
	ubi_partition_force_release_all();
	/* Drop any device/volume/leaf/scratch handles leaked by tests that
	 * aborted via zassert before calling ubi_device_deinit(). Without
	 * this, a single leak in the static backend (default slab pool
	 * count = 1) cascades into -ENOMEM on every subsequent test. */
	ubi_mem_force_reset_all_slabs();
}

int ubi_test_get_erased_val(const struct ubi_flash_desc *flash, uint8_t *erased_val)
{
	return ubi_get_erased_val(flash, erased_val);
}

bool ubi_test_buf_is_erased(const void *buf, size_t len, uint8_t erased_val)
{
	return ubi_buf_is_erased(buf, len, erased_val);
}

void ubi_test_set_write_shutdown(struct ubi_device *ubi, bool shutdown)
{
	if (!ubi) {
		LOG_ERR("ubi is NULL");
		return;
	}

	k_mutex_lock(&ubi->mutex, K_FOREVER);
	ubi->test_write_shutdown = shutdown;
	k_mutex_unlock(&ubi->mutex);
}

#endif /* CONFIG_UBI_TEST_API_ENABLE */
