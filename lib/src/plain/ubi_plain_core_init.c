/**
 * \file    ubi_plain_core_init.c
 * \author  Kamil Kielbasa
 * \brief   UBI device initialization: format, scan, mount.
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files -------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_internal.h"
#include "ubi_backend.h"
#include "ubi_plain_io.h"
#include "ubi_plain_flash_res_peb.h"
#include "ubi_plain_ops.h"
#include "ubi_mem.h"
#include "ubi_partition_guard.h"

/* Zephyr headers: */
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <zephyr/storage/flash_map.h>

/* Zephyr device API (for device_is_ready): */
#include <zephyr/device.h>

/* Standard library headers: */
#include <errno.h>
#include <stdbool.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

LOG_MODULE_REGISTER(ubi, CONFIG_UBI_LOG_LEVEL);

/* Static function declarations ----------------------------------------------------------------- */

/**
 * \brief Return codes for PEB scan helpers.
 *
 * Each helper returns SCAN_NEXT_STEP to continue processing, SCAN_PEB_HANDLED
 * when the PEB is fully classified, or a negative errno on fatal errors.
 */
enum scan_result {
	/** Continue to the next classification step. */
	SCAN_NEXT_STEP = 0,
	/** PEB fully classified — skip to the next PEB. */
	SCAN_PEB_HANDLED = 1,
};

/**
 * \brief Format the UBI device by mounting and initializing all PEBs.
 *
 * Called when a UBI device is not yet mounted. Mounts the device header
 * and erases + writes EC headers for all data PEBs.
 *
 * \param[in,out] ubi_dev  UBI device handle.
 * \param nr_of_pebs       Total number of PEBs in the flash partition.
 *
 * \return 0 on success, negative errno on failure.
 */
static int init_format_device(struct ubi_device *ubi_dev, size_t nr_of_pebs);

/**
 * \brief Collect volumes from device headers into the in-memory volume tree.
 *
 * \param[in,out] ubi_dev  UBI device handle.
 * \param[in] dev_hdr      Device header with volume count and watermark.
 *
 * \return 0 on success, negative errno on failure.
 */
static int init_collect_volumes(struct ubi_device *ubi_dev, const struct ubi_dev_hdr *dev_hdr);

/**
 * \brief Compute the average erase counter across all valid PEBs.
 *
 * Stores ec_sum and ec_count in the device structure for runtime tracking.
 *
 * \param[in,out] ubi_dev  UBI device handle.
 * \param nr_of_pebs       Total number of PEBs in the flash partition.
 */
static void init_compute_ec_average(struct ubi_device *ubi_dev, size_t nr_of_pebs);

/**
 * \brief Validate the EC header; mark PEB as bad if the read fails.
 *
 * \param[in,out] dev   UBI device handle.
 * \param pnum          Physical eraseblock number.
 * \param ec_avg        Average erase count (used for bad-block bookkeeping).
 * \param[out] ec_hdr   Receives the validated EC header on success.
 *
 * \return SCAN_NEXT_STEP, SCAN_PEB_HANDLED, or negative errno.
 */
static int validate_ec_header(struct ubi_device *dev, size_t pnum, size_t ec_avg,
			      struct ubi_ec_hdr *ec_hdr);

/**
 * \brief Read and validate the VID header. Classify PEB as free, dirty, or bad.
 *
 * An erased VID does not necessarily mean the PEB is free. The VID header is
 * written last (after the data payload), so an erased VID with non-erased data
 * indicates an interrupted write that must be classified as dirty.
 *
 * \param[in,out] dev   UBI device handle.
 * \param pnum          Physical eraseblock number.
 * \param[in] ec_hdr    Validated EC header for this PEB.
 * \param[out] vid_hdr  Receives the validated VID header on SCAN_NEXT_STEP.
 * \param erased_val    Hardware-reported erased byte value.
 *
 * \return SCAN_NEXT_STEP, SCAN_PEB_HANDLED, or negative errno.
 */
static int validate_vid_header(struct ubi_device *dev, size_t pnum, const struct ubi_ec_hdr *ec_hdr,
			       struct ubi_vid_hdr *vid_hdr, uint8_t erased_val);

/**
 * \brief Classify an orphan PEB (volume deleted) by moving it to the dirty pool.
 *
 * \param[in,out] dev   UBI device handle.
 * \param pnum          Physical eraseblock number.
 * \param[in] ec_hdr    EC header for this PEB.
 * \param[in] vid_hdr   VID header for this PEB.
 *
 * \return SCAN_NEXT_STEP, SCAN_PEB_HANDLED, or negative errno.
 */
static int classify_orphan_peb(struct ubi_device *dev, size_t pnum, const struct ubi_ec_hdr *ec_hdr,
			       const struct ubi_vid_hdr *vid_hdr);

/**
 * \brief Map a LEB that appears for the first time into the volume EBA table.
 *
 * If the LEB index exceeds the volume capacity, the PEB is moved to the dirty pool.
 *
 * \param[in,out] dev   UBI device handle.
 * \param pnum          Physical eraseblock number.
 * \param[in] ec_hdr    EC header for this PEB.
 * \param[in] vid_hdr   VID header for this PEB.
 * \param[in,out] vol   Target volume.
 *
 * \return SCAN_NEXT_STEP, SCAN_PEB_HANDLED, or negative errno.
 */
static int map_leb_first_occurrence(struct ubi_device *dev, size_t pnum,
				    const struct ubi_ec_hdr *ec_hdr,
				    const struct ubi_vid_hdr *vid_hdr, struct ubi_volume *vol);

/**
 * \brief Resolve a duplicate LEB by comparing sequence numbers.
 *
 * The PEB with the higher sequence number wins the EBA table slot;
 * the loser is moved to the dirty pool. If the existing PEB's headers
 * cannot be read, it is moved to the bad blocks list.
 *
 * \param[in,out] dev   UBI device handle.
 * \param pnum          Physical eraseblock number of the new candidate.
 * \param ec_avg        Average erase count for bad-block bookkeeping.
 * \param[in] ec_hdr    EC header for the new PEB.
 * \param[in] vid_hdr   VID header for the new PEB.
 * \param[in,out] vol   Target volume.
 * \param[in,out] existing  Current EBA entry for the same LEB.
 *
 * \return SCAN_PEB_HANDLED or negative errno.
 */
static int resolve_duplicate_leb(struct ubi_device *dev, size_t pnum, size_t ec_avg,
				 const struct ubi_ec_hdr *ec_hdr, const struct ubi_vid_hdr *vid_hdr,
				 struct ubi_volume *vol, struct ubi_rbt_item *existing);

/**
 * \brief Scan all PEBs and classify into free, dirty, bad, or EBA table entries.
 *
 * Iterates over every data PEB and applies the classification pipeline:
 * validate_ec_header -> validate_vid_header -> classify_orphan_peb ->
 * map_leb_first_occurrence -> resolve_duplicate_leb.
 *
 * A negative return from any helper is a fatal allocation failure that
 * aborts the scan. SCAN_PEB_HANDLED means skip to next PEB.
 *
 * \param[in,out] ubi_dev  UBI device handle.
 * \param nr_of_pebs       Total number of PEBs in the flash partition.
 * \param ec_avg           Average erase count for bad-block bookkeeping.
 *
 * \return 0 on success, negative errno on failure.
 */
static int init_scan_pebs(struct ubi_device *ubi_dev, size_t nr_of_pebs, size_t ec_avg);

/* Internal helper definitions ------------------------------------------------------------------ */

int ubi_get_erased_val(const struct ubi_flash_desc *flash, uint8_t *erased_val)
{
	if (!flash || !erased_val) {
		LOG_ERR("Invalid argument: flash=%p erased_val=%p", (const void *)flash,
			(const void *)erased_val);
		return -EINVAL;
	}

	const struct flash_area *fa = NULL;
	int ret = flash_area_open(flash->partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failure for erased value query");
		return ret;
	}

	*erased_val = flash_area_erased_val(fa);
	flash_area_close(fa);
	return 0;
}

void ubi_move_to_bad_blocks(struct ubi_device *ubi, size_t pnum, size_t erase_count,
			    struct ubi_list_item *bad_item)
{
	if (!ubi || !bad_item) {
		LOG_ERR("Invalid argument: ubi=%p bad_item=%p", (const void *)ubi,
			(const void *)bad_item);
		return;
	}

	bad_item->pnum = pnum;
	bad_item->erase_count = erase_count;
	sys_slist_append(&ubi->bad_pebs, &bad_item->node);
	ubi->bad_peb_count += 1;
}

struct ubi_volume *ubi_find_volume(struct ubi_device *ubi, int vol_id)
{
	if (ubi->vol_count == 0) {
		LOG_ERR("No volumes present on device");
		return NULL;
	}

	struct ubi_rbt_item *entry = ubi_cache_search(&ubi->vols, vol_id);

	if (!entry) {
		LOG_ERR("Device volume not found");
		return NULL;
	}

	return entry->value.vol;
}

/* Init sub-functions --------------------------------------------------------------------------- */

static int init_format_device(struct ubi_device *ubi_dev, size_t nr_of_pebs)
{
	__ASSERT_NO_MSG(ubi_dev);

	int ret = ubi_dev_mount(&ubi_dev->flash);

	if (ret != 0) {
		LOG_ERR("Device mount failure");
		return ret;
	}

	struct ubi_ec_hdr ec_hdr = { 0 };
	ec_hdr.magic = UBI_EC_HDR_MAGIC;
	ec_hdr.version = UBI_EC_HDR_VERSION;
	ec_hdr.ec = 0;
	ec_hdr.hdr_crc =
		crc32_ieee((const uint8_t *)&ec_hdr, sizeof(ec_hdr) - sizeof(ec_hdr.hdr_crc));

	const struct flash_area *fa = NULL;
	ret = flash_area_open(ubi_dev->flash.partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		return ret;
	}

	for (size_t peb_idx = UBI_DEV_HDR_NR_OF_RES_PEBS; peb_idx < nr_of_pebs; ++peb_idx) {
		const size_t offset = peb_idx * ubi_dev->flash.erase_block_size;
		ret = flash_area_erase(fa, offset, ubi_dev->flash.erase_block_size);

		if (ret != 0) {
			LOG_ERR("Flash erase failure");
			flash_area_close(fa);
			return ret;
		}
	}

	flash_area_close(fa);

	for (size_t peb_idx = UBI_DEV_HDR_NR_OF_RES_PEBS; peb_idx < nr_of_pebs; ++peb_idx) {
		ret = ubi_ec_hdr_write(&ubi_dev->flash, peb_idx, &ec_hdr);

		if (ret != 0) {
			LOG_ERR("EC header write failure");
			return ret;
		}
	}

	return 0;
}

static int init_collect_volumes(struct ubi_device *ubi_dev, const struct ubi_dev_hdr *dev_hdr)
{
	__ASSERT_NO_MSG(ubi_dev);
	__ASSERT_NO_MSG(dev_hdr);

	for (size_t vol_idx = 0; vol_idx < dev_hdr->vol_count; ++vol_idx) {
		struct ubi_vol_hdr vol_hdr = { 0 };
		int ret = ubi_vol_hdr_read(&ubi_dev->flash, vol_idx, &vol_hdr);

		if (ret != 0) {
			LOG_ERR("Volume header read failure");
			return ret;
		}

		if (!ubi_vol_hdr_semantically_valid(&vol_hdr)) {
			LOG_ERR("Volume header %zu semantically invalid", vol_idx);
			return -EIO;
		}

		struct ubi_volume *vol = NULL;
		ret = ubi_mem_volume_alloc(&vol);

		if (ret != 0) {
			LOG_ERR("Volume allocation failure");
			return ret;
		}
		vol->vol_id = vol_hdr.vol_id;
		ubi_copy_name_from_hdr(vol->cfg.name, vol_hdr.name);
		vol->cfg.type = vol_hdr.vol_type;
		vol->cfg.leb_count = vol_hdr.leb_count;
		vol->eba_tbl_count = 0;
		vol->eba_tbl.lessthan_fn = ubi_cache_cmp;

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

	ubi_dev->vol_id_watermark = dev_hdr->vol_id_watermark;

	return 0;
}

static void init_compute_ec_average(struct ubi_device *ubi_dev, size_t nr_of_pebs)
{
	__ASSERT_NO_MSG(ubi_dev);

	size_t ec_sum = 0;
	size_t ec_count = 0;

	for (size_t pnum = UBI_DEV_HDR_NR_OF_RES_PEBS; pnum < nr_of_pebs; ++pnum) {
		struct ubi_ec_hdr ec_hdr = { 0 };
		int ret = ubi_ec_hdr_read(&ubi_dev->flash, pnum, &ec_hdr);

		if (ret == 0) {
			ec_sum += ec_hdr.ec;
			ec_count += 1;
		}
	}

	ubi_dev->ec_sum = ec_sum;
	ubi_dev->ec_count = ec_count;
}

static int validate_ec_header(struct ubi_device *dev, size_t pnum, size_t ec_avg,
			      struct ubi_ec_hdr *ec_hdr)
{
	__ASSERT_NO_MSG(dev);
	__ASSERT_NO_MSG(ec_hdr);

	int ret = ubi_ec_hdr_read(&dev->flash, pnum, ec_hdr);

	if (ret != 0) {
		struct ubi_list_item *item = NULL;
		ret = ubi_mem_leaf_alloc((void **)&item);

		if (ret != 0) {
			LOG_ERR("Leaf item allocation failure");
			return ret;
		}

		ubi_move_to_bad_blocks(dev, pnum, ec_avg, item);
		return SCAN_PEB_HANDLED;
	}

	return SCAN_NEXT_STEP;
}

static int validate_vid_header(struct ubi_device *dev, size_t pnum, const struct ubi_ec_hdr *ec_hdr,
			       struct ubi_vid_hdr *vid_hdr, uint8_t erased_val)
{
	__ASSERT_NO_MSG(dev);
	__ASSERT_NO_MSG(ec_hdr);
	__ASSERT_NO_MSG(vid_hdr);

	/* First read without CRC — detect empty (free/uncommitted) PEBs. */
	int ret = ubi_vid_hdr_read(&dev->flash, pnum, vid_hdr, false);

	if (ret != 0) {
		LOG_ERR("VID header read failure for PEB %zu", pnum);
		goto classify_bad;
	}

	if (ubi_buf_is_erased(vid_hdr, sizeof(*vid_hdr), erased_val)) {
		/*
		 * VID is erased. Probe the beginning of the data area to
		 * distinguish a truly free PEB from an uncommitted write
		 * (data was written but VID commit did not complete).
		 *
		 * Read min(write_block_size, leb_size) bytes starting at
		 * offset 0 of the data area. Since writes always start at
		 * offset 0, a non-erased prefix proves partial data presence.
		 */
		const size_t probe_len = MIN(dev->flash.write_block_size, dev->leb_size);
		uint8_t probe_buf[WRITE_BLOCK_SIZE_ALIGNMENT] = { 0 };

		ret = ubi_leb_data_read(&dev->flash, pnum, 0, probe_buf, probe_len);

		if (ret != 0) {
			LOG_ERR("Data area probe read failure for PEB %zu", pnum);
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

		if (ubi_buf_is_erased(probe_buf, probe_len, erased_val)) {
			/* VID erased + data erased -> genuinely free. */
			rb_insert(&dev->free_pool.tree, &item->node);
			dev->free_pool.count += 1;
		} else {
			/* VID erased + data present -> uncommitted / dirty. */
			LOG_WRN("PEB %zu: erased VID but non-erased data — "
				"classifying as dirty (uncommitted write)",
				pnum);
			rb_insert(&dev->dirty_pool.tree, &item->node);
			dev->dirty_pool.count += 1;
		}

		return SCAN_PEB_HANDLED;
	}

	/* Re-read with CRC validation; corrupt header means bad PEB. */
	memset(vid_hdr, 0, sizeof(*vid_hdr));
	ret = ubi_vid_hdr_read(&dev->flash, pnum, vid_hdr, true);

	if (ret != 0) {
		LOG_ERR("VID header CRC validation failure for PEB %zu", pnum);
		goto classify_bad;
	}

	/* clang-format off */
	return SCAN_NEXT_STEP;

classify_bad: {
	/* clang-format on */
	struct ubi_list_item *item = NULL;
	ret = ubi_mem_leaf_alloc((void **)&item);

	if (ret != 0) {
		LOG_ERR("Leaf item allocation failure");
		return ret;
	}

	ubi_move_to_bad_blocks(dev, pnum, ec_hdr->ec, item);
	return SCAN_PEB_HANDLED;
}
}

static int classify_orphan_peb(struct ubi_device *dev, size_t pnum, const struct ubi_ec_hdr *ec_hdr,
			       const struct ubi_vid_hdr *vid_hdr)
{
	__ASSERT_NO_MSG(dev);
	__ASSERT_NO_MSG(ec_hdr);
	__ASSERT_NO_MSG(vid_hdr);

	struct ubi_rbt_item *vol_entry = ubi_cache_search(&dev->vols, vid_hdr->vol_id);

	if (vol_entry) {
		return SCAN_NEXT_STEP;
	}

	struct ubi_rbt_item *item = NULL;
	int ret = ubi_mem_leaf_alloc((void **)&item);

	if (ret != 0) {
		LOG_ERR("Leaf item allocation failure");
		return ret;
	}

	item->key = ec_hdr->ec;
	item->value.pnum = pnum;
	rb_insert(&dev->dirty_pool.tree, &item->node);
	dev->dirty_pool.count += 1;

	return SCAN_PEB_HANDLED;
}

static int map_leb_first_occurrence(struct ubi_device *dev, size_t pnum,
				    const struct ubi_ec_hdr *ec_hdr,
				    const struct ubi_vid_hdr *vid_hdr, struct ubi_volume *vol)
{
	__ASSERT_NO_MSG(dev);
	__ASSERT_NO_MSG(ec_hdr);
	__ASSERT_NO_MSG(vid_hdr);
	__ASSERT_NO_MSG(vol);

	struct ubi_rbt_item *existing = ubi_cache_search(&vol->eba_tbl, vid_hdr->lnum);

	if (existing) {
		return SCAN_NEXT_STEP;
	}

	struct ubi_rbt_item *item = NULL;
	int ret = ubi_mem_leaf_alloc((void **)&item);

	if (ret != 0) {
		LOG_ERR("Leaf item allocation failure");
		return ret;
	}

	if (vid_hdr->lnum >= vol->cfg.leb_count) {
		item->key = ec_hdr->ec;
		item->value.pnum = pnum;
		rb_insert(&dev->dirty_pool.tree, &item->node);
		dev->dirty_pool.count += 1;
		return SCAN_PEB_HANDLED;
	}

	item->key = vid_hdr->lnum;
	item->value.pnum = pnum;
	rb_insert(&vol->eba_tbl, &item->node);
	vol->eba_tbl_count += 1;

	return SCAN_PEB_HANDLED;
}

static int resolve_duplicate_leb(struct ubi_device *dev, size_t pnum, size_t ec_avg,
				 const struct ubi_ec_hdr *ec_hdr, const struct ubi_vid_hdr *vid_hdr,
				 struct ubi_volume *vol, struct ubi_rbt_item *existing)
{
	__ASSERT_NO_MSG(dev);
	__ASSERT_NO_MSG(ec_hdr);
	__ASSERT_NO_MSG(vid_hdr);
	__ASSERT_NO_MSG(vol);
	__ASSERT_NO_MSG(existing);

	struct ubi_rbt_item *item = NULL;
	int ret = ubi_mem_leaf_alloc((void **)&item);

	if (ret != 0) {
		LOG_ERR("Leaf item allocation failure");
		return ret;
	}

	struct ubi_ec_hdr exist_ec = { 0 };
	ret = ubi_ec_hdr_read(&dev->flash, existing->value.pnum, &exist_ec);

	if (ret != 0) {
		rb_remove(&vol->eba_tbl, &existing->node);
		vol->eba_tbl_count -= 1;

		const size_t bad_pnum = existing->value.pnum;
		struct ubi_list_item *bad = ubi_leaf_as_list(existing);
		ubi_move_to_bad_blocks(dev, bad_pnum, ec_avg, bad);

		item->key = vid_hdr->lnum;
		item->value.pnum = pnum;
		rb_insert(&vol->eba_tbl, &item->node);
		vol->eba_tbl_count += 1;

		return SCAN_PEB_HANDLED;
	}

	struct ubi_vid_hdr exist_vid = { 0 };
	ret = ubi_vid_hdr_read(&dev->flash, existing->value.pnum, &exist_vid, true);

	if (ret != 0) {
		rb_remove(&vol->eba_tbl, &existing->node);
		vol->eba_tbl_count -= 1;

		const size_t bad_pnum = existing->value.pnum;
		struct ubi_list_item *bad = ubi_leaf_as_list(existing);
		ubi_move_to_bad_blocks(dev, bad_pnum, ec_hdr->ec, bad);

		item->key = vid_hdr->lnum;
		item->value.pnum = pnum;
		rb_insert(&vol->eba_tbl, &item->node);
		vol->eba_tbl_count += 1;

		return SCAN_PEB_HANDLED;
	}

	if (vid_hdr->sqnum < exist_vid.sqnum) {
		/* Current PEB is older — discard to dirty pool. */
		item->key = ec_hdr->ec;
		item->value.pnum = pnum;
		rb_insert(&dev->dirty_pool.tree, &item->node);
		dev->dirty_pool.count += 1;
	} else {
		/* Current PEB is newer — replace the existing mapping. */
		rb_remove(&vol->eba_tbl, &existing->node);
		vol->eba_tbl_count -= 1;

		existing->key = exist_ec.ec;
		rb_insert(&dev->dirty_pool.tree, &existing->node);
		dev->dirty_pool.count += 1;

		item->key = vid_hdr->lnum;
		item->value.pnum = pnum;
		rb_insert(&vol->eba_tbl, &item->node);
		vol->eba_tbl_count += 1;
	}

	return SCAN_PEB_HANDLED;
}

static int init_scan_pebs(struct ubi_device *ubi_dev, size_t nr_of_pebs, size_t ec_avg)
{
	__ASSERT_NO_MSG(ubi_dev);

	uint8_t erased_val = 0xFF;
	int ev_ret = ubi_get_erased_val(&ubi_dev->flash, &erased_val);

	if (ev_ret != 0) {
		LOG_ERR("Failed to query erased value");
		return ev_ret;
	}

	/*
	 * Each scan helper returns SCAN_NEXT_STEP (0) to continue to the next
	 * classification stage, SCAN_PEB_HANDLED (1) when the PEB is fully
	 * classified, or a negative errno on fatal error.
	 *
	 * All I/O failures (bad reads, CRC mismatches) are handled internally
	 * by the helpers — the affected PEB is classified as bad/dirty and the
	 * helper returns SCAN_PEB_HANDLED.  A negative return can only occur
	 * when ubi_mem_leaf_alloc() fails, meaning the slab allocator is
	 * exhausted.  In that case no subsequent PEB can be classified either,
	 * so the scan is aborted.
	 */
	for (size_t pnum = UBI_DEV_HDR_NR_OF_RES_PEBS; pnum < nr_of_pebs; ++pnum) {
		struct ubi_ec_hdr ec_hdr = { 0 };
		int ret = validate_ec_header(ubi_dev, pnum, ec_avg, &ec_hdr);

		if (ret < 0) {
			LOG_ERR("EC header validation failed for PEB %zu: %d", pnum, ret);
			return ret;
		}

		if (ret == SCAN_PEB_HANDLED)
			continue;

		struct ubi_vid_hdr vid_hdr = { 0 };
		ret = validate_vid_header(ubi_dev, pnum, &ec_hdr, &vid_hdr, erased_val);

		if (ret < 0) {
			LOG_ERR("VID header validation failed for PEB %zu: %d", pnum, ret);
			return ret;
		}

		if (ret == SCAN_PEB_HANDLED)
			continue;

		if (vid_hdr.sqnum > ubi_dev->global_sqnum)
			ubi_dev->global_sqnum = vid_hdr.sqnum;

		ret = classify_orphan_peb(ubi_dev, pnum, &ec_hdr, &vid_hdr);

		if (ret < 0) {
			LOG_ERR("Orphan classification failed for PEB %zu: %d", pnum, ret);
			return ret;
		}

		if (ret == SCAN_PEB_HANDLED)
			continue;

		struct ubi_rbt_item *vol_entry = ubi_cache_search(&ubi_dev->vols, vid_hdr.vol_id);
		struct ubi_volume *vol = vol_entry->value.vol;

		ret = map_leb_first_occurrence(ubi_dev, pnum, &ec_hdr, &vid_hdr, vol);

		if (ret < 0) {
			LOG_ERR("LEB mapping failed for PEB %zu: %d", pnum, ret);
			return ret;
		}

		if (ret == SCAN_PEB_HANDLED)
			continue;

		struct ubi_rbt_item *existing = ubi_cache_search(&vol->eba_tbl, vid_hdr.lnum);
		ret = resolve_duplicate_leb(ubi_dev, pnum, ec_avg, &ec_hdr, &vid_hdr, vol,
					    existing);

		if (ret < 0) {
			LOG_ERR("Duplicate LEB resolution failed for PEB %zu: %d", pnum, ret);
			return ret;
		}
	}

	return 0;
}

/* Module interface function definitions -------------------------------------------------------- */

int ubi_plain_device_init(const struct ubi_flash_desc *flash,
			  const struct ubi_secure_config *secure_cfg, struct ubi_device **ubi)
{
	ARG_UNUSED(secure_cfg);

	int ret = -1;

	if (!flash || !ubi) {
		LOG_ERR("Invalid argument: flash=%p ubi=%p", (const void *)flash,
			(const void *)ubi);
		return -EINVAL;
	}

	/* Check partition availability before allocating — avoids wasting a slab
	 * block when the partition is already in use. */
	ret = ubi_partition_acquire(flash->partition_id);

	if (ret != 0) {
		LOG_ERR("Partition %u already in use by another UBI handle", flash->partition_id);
		*ubi = NULL;
		return -EBUSY;
	}

	struct ubi_device *ubi_dev = NULL;
	ret = ubi_mem_device_alloc(&ubi_dev);

	if (ret != 0) {
		LOG_ERR("Device allocation failure");
		ubi_partition_release(flash->partition_id);
		return ret;
	}
	k_mutex_init(&ubi_dev->mutex);
	ubi_dev->flash = *flash;
	ubi_dev->mode = UBI_MODE_PLAIN;
	ubi_dev->ops = ubi_plain_backend();
	ubi_dev->free_pool.tree.lessthan_fn = ubi_cache_cmp;
	ubi_dev->dirty_pool.tree.lessthan_fn = ubi_cache_cmp;
	sys_slist_init(&ubi_dev->bad_pebs);
	ubi_dev->vols.lessthan_fn = ubi_cache_cmp;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(ubi_dev->flash.partition_id, &fa);

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
	if (ubi_dev->flash.write_block_size == 0 || ubi_dev->flash.erase_block_size == 0) {
		LOG_ERR("Invalid geometry: write_block_size or erase_block_size is zero");
		flash_area_close(fa);
		ret = -EINVAL;
		goto exit;
	}

	if (fa->fa_size % ubi_dev->flash.erase_block_size != 0) {
		LOG_ERR("Partition size not a multiple of erase block size");
		flash_area_close(fa);
		ret = -EINVAL;
		goto exit;
	}

	if (ubi_dev->flash.erase_block_size % ubi_dev->flash.write_block_size != 0) {
		LOG_ERR("Erase block size not a multiple of write block size");
		flash_area_close(fa);
		ret = -EINVAL;
		goto exit;
	}

	if (ubi_dev->flash.write_block_size > WRITE_BLOCK_SIZE_ALIGNMENT) {
		LOG_ERR("write_block_size %zu exceeds max supported alignment %d",
			ubi_dev->flash.write_block_size, WRITE_BLOCK_SIZE_ALIGNMENT);
		flash_area_close(fa);
		ret = -EINVAL;
		goto exit;
	}

	const size_t nr_of_pebs = fa->fa_size / ubi_dev->flash.erase_block_size;

	if (nr_of_pebs <= UBI_DEV_HDR_NR_OF_RES_PEBS) {
		LOG_ERR("Partition too small: need > %d PEBs for reserved + data",
			UBI_DEV_HDR_NR_OF_RES_PEBS);
		flash_area_close(fa);
		ret = -EINVAL;
		goto exit;
	}

	if (ubi_dev->flash.erase_block_size < (UBI_EC_HDR_SIZE + UBI_VID_HDR_SIZE)) {
		LOG_ERR("Erase block too small for EC + VID headers");
		flash_area_close(fa);
		ret = -EINVAL;
		goto exit;
	}

	flash_area_close(fa);

	bool is_mounted = false;
	ret = ubi_dev_is_mounted(&ubi_dev->flash, &is_mounted);

	if (ret != 0) {
		LOG_ERR("Device check mount failure");
		goto exit;
	}

	/* Format device on first use. */
	if (!is_mounted) {
		ret = init_format_device(ubi_dev, nr_of_pebs);

		if (ret != 0)
			goto exit;
	}

	/* Read device header and reconstruct volume table. */
	struct ubi_dev_hdr dev_hdr = { 0 };
	ret = ubi_dev_hdr_read(&ubi_dev->flash, &dev_hdr);

	if (ret == -EROFS) {
		LOG_WRN("Device in degraded mode: reserved PEB redundancy lost");
		ubi_dev->read_only_degraded = true;
	} else if (ret != 0) {
		LOG_ERR("Device header read failure");
		goto exit;
	}

	/* Cache geometry for fast internal lookups. */
	ubi_dev->total_data_peb_count = nr_of_pebs - UBI_DEV_HDR_NR_OF_RES_PEBS;
	ubi_dev->leb_size = ubi_dev->flash.erase_block_size - UBI_EC_HDR_SIZE - UBI_VID_HDR_SIZE;

#if defined(CONFIG_UBI_MEM_BACKEND_STATIC)
	if (ubi_dev->total_data_peb_count > CONFIG_UBI_MAX_NR_OF_DATA_PEBS) {
		LOG_ERR("Flash has %zu data PEBs but CONFIG_UBI_MAX_NR_OF_DATA_PEBS=%d",
			ubi_dev->total_data_peb_count, CONFIG_UBI_MAX_NR_OF_DATA_PEBS);
		ret = -ENOMEM;
		goto exit;
	}

	if (dev_hdr.vol_count > CONFIG_UBI_MAX_NR_OF_VOLUMES) {
		LOG_ERR("Device has %u volumes but CONFIG_UBI_MAX_NR_OF_VOLUMES=%d",
			dev_hdr.vol_count, CONFIG_UBI_MAX_NR_OF_VOLUMES);
		ret = -ENOMEM;
		goto exit;
	}
#endif /* CONFIG_UBI_MEM_BACKEND_STATIC */

	ret = init_collect_volumes(ubi_dev, &dev_hdr);

	if (ret != 0)
		goto exit;

	/* Compute average erase counter and store sum/count for runtime tracking. */
	init_compute_ec_average(ubi_dev, nr_of_pebs);
	const size_t ec_avg = (ubi_dev->ec_count > 0) ? (ubi_dev->ec_sum / ubi_dev->ec_count) : 0;

	/* Scan all PEBs and classify into free, dirty, bad, or EBA entries. */
	ret = init_scan_pebs(ubi_dev, nr_of_pebs, ec_avg);

	if (ret != 0)
		goto exit;

	/* Ensure next sqnum is strictly greater than any existing one. */
	ubi_dev->global_sqnum += 1;

	*ubi = ubi_dev;
	return 0;

exit:
	ubi_device_deinit(ubi_dev);
	*ubi = NULL;
	return ret;
}

const struct ubi_backend_ops *ubi_plain_backend(void)
{
	static const struct ubi_backend_ops ops = {
		.init = ubi_plain_device_init,
		.get_info = ubi_plain_device_get_info,
		.deinit = ubi_plain_device_deinit,
		.erase_peb = ubi_plain_device_erase_peb,
		.vol_create = ubi_plain_volume_create,
		.vol_resize = ubi_plain_volume_resize,
		.vol_remove = ubi_plain_volume_remove,
		.vol_get_info = ubi_plain_volume_get_info,
		.leb_write = ubi_plain_leb_write,
		.leb_read = ubi_plain_leb_read,
		.leb_map = ubi_plain_leb_map,
		.leb_unmap = ubi_plain_leb_unmap,
		.leb_is_mapped = ubi_plain_leb_is_mapped,
		.leb_get_size = ubi_plain_leb_get_size,
	};

	return &ops;
}
