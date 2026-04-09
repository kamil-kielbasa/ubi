/**
 * \file    ubi_core_init.c
 * \author  Kamil Kielbasa
 * \brief   UBI device initialization: format, scan, mount.
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files ------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_internal.h"
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

/* Module defines ------------------------------------------------------------------------------ */

LOG_MODULE_REGISTER(ubi, CONFIG_UBI_LOG_LEVEL);

/* Static function declarations ---------------------------------------------------------------- */

static int init_format_device(struct ubi_device *ubi_dev, size_t nr_of_pebs);
static int init_collect_volumes(struct ubi_device *ubi_dev, const struct ubi_dev_hdr *dev_hdr);
static void init_compute_ec_average(struct ubi_device *ubi_dev, size_t nr_of_pebs);
static int init_scan_pebs(struct ubi_device *ubi_dev, size_t nr_of_pebs, size_t ec_avg);

/* Internal helper definitions ----------------------------------------------------------------- */

int ubi_get_erased_val(const struct ubi_mtd *mtd, uint8_t *erased_val)
{
	__ASSERT_NO_MSG(mtd);
	__ASSERT_NO_MSG(erased_val);

	const struct flash_area *fa = NULL;
	int ret = flash_area_open(mtd->partition_id, &fa);

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
	__ASSERT_NO_MSG(ubi);
	__ASSERT_NO_MSG(bad_item);

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

/* Init sub-functions ------------------------------------------------------------------ */

/**
 * \brief Format the UBI device by mounting and initializing all PEBs.
 *
 * Called when a UBI device is not yet mounted. Mounts the device header
 * and erases + writes EC headers for all data PEBs.
 */
static int init_format_device(struct ubi_device *ubi_dev, size_t nr_of_pebs)
{
	int ret = ubi_dev_mount(&ubi_dev->mtd);

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
	ret = flash_area_open(ubi_dev->mtd.partition_id, &fa);

	if (ret != 0) {
		LOG_ERR("Flash area open failure");
		return ret;
	}

	for (size_t peb_idx = UBI_DEV_HDR_NR_OF_RES_PEBS; peb_idx < nr_of_pebs; ++peb_idx) {
		const size_t offset = peb_idx * ubi_dev->mtd.erase_block_size;
		ret = flash_area_erase(fa, offset, ubi_dev->mtd.erase_block_size);

		if (ret != 0) {
			LOG_ERR("Flash erase failure");
			flash_area_close(fa);
			return ret;
		}
	}

	flash_area_close(fa);

	for (size_t peb_idx = UBI_DEV_HDR_NR_OF_RES_PEBS; peb_idx < nr_of_pebs; ++peb_idx) {
		ret = ubi_ec_hdr_write(&ubi_dev->mtd, peb_idx, &ec_hdr);

		if (ret != 0) {
			LOG_ERR("EC header write failure");
			return ret;
		}
	}

	return 0;
}

/**
 * \brief Collect volumes from device headers into the in-memory volume tree.
 */
static int init_collect_volumes(struct ubi_device *ubi_dev, const struct ubi_dev_hdr *dev_hdr)
{
	for (size_t vol_idx = 0; vol_idx < dev_hdr->vol_count; ++vol_idx) {
		struct ubi_vol_hdr vol_hdr = { 0 };
		int ret = ubi_vol_hdr_read(&ubi_dev->mtd, vol_idx, &vol_hdr);

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
		vol->vol_idx = vol_idx;
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

		if (vol->vol_id > ubi_dev->vol_next_id)
			ubi_dev->vol_next_id = vol->vol_id;
	}

	if (dev_hdr->vol_count > 0)
		ubi_dev->vol_next_id += 1;

	return 0;
}

/**
 * \brief Compute the average erase counter across all valid PEBs.
 *
 * Stores ec_sum and ec_count in the device structure for runtime tracking.
 */
static void init_compute_ec_average(struct ubi_device *ubi_dev, size_t nr_of_pebs)
{
	size_t ec_sum = 0;
	size_t ec_count = 0;

	for (size_t pnum = UBI_DEV_HDR_NR_OF_RES_PEBS; pnum < nr_of_pebs; ++pnum) {
		struct ubi_ec_hdr ec_hdr = { 0 };
		int ret = ubi_ec_hdr_read(&ubi_dev->mtd, pnum, &ec_hdr);

		if (ret == 0) {
			ec_sum += ec_hdr.ec;
			ec_count += 1;
		}
	}

	ubi_dev->ec_sum = ec_sum;
	ubi_dev->ec_count = ec_count;
}

/**
 * \brief Return codes for PEB scan helpers.
 *
 * Each helper returns SCAN_NEXT_STEP to continue processing, SCAN_PEB_HANDLED
 * when the PEB is fully classified, or a negative errno on fatal errors.
 */
enum scan_result {
	SCAN_NEXT_STEP = 0,
	SCAN_PEB_HANDLED = 1,
};

/**
 * \brief Validate the EC header; mark PEB as bad if the read fails.
 */
static int validate_ec_header(struct ubi_device *dev, size_t pnum, size_t ec_avg,
			      struct ubi_ec_hdr *ec_hdr)
{
	int ret = ubi_ec_hdr_read(&dev->mtd, pnum, ec_hdr);

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

/**
 * \brief Read and validate the VID header. Classify PEB as free or bad when appropriate.
 *
 * On return with SCAN_NEXT_STEP, \p vid_hdr contains a CRC-validated VID header.
 *
 * \param erased_val  Hardware-reported erased byte value for the flash partition.
 */
static int validate_vid_header(struct ubi_device *dev, size_t pnum, const struct ubi_ec_hdr *ec_hdr,
			       struct ubi_vid_hdr *vid_hdr, uint8_t erased_val)
{
	/* First read without CRC — detect empty (free) PEBs. */
	int ret = ubi_vid_hdr_read(&dev->mtd, pnum, vid_hdr, false);

	if (ret != 0) {
		struct ubi_list_item *item = NULL;
		ret = ubi_mem_leaf_alloc((void **)&item);

		if (ret != 0) {
			LOG_ERR("Leaf item allocation failure");
			return ret;
		}

		ubi_move_to_bad_blocks(dev, pnum, ec_hdr->ec, item);
		return SCAN_PEB_HANDLED;
	}

	if (ubi_buf_is_erased(vid_hdr, sizeof(*vid_hdr), erased_val)) {
		struct ubi_rbt_item *item = NULL;
		ret = ubi_mem_leaf_alloc((void **)&item);

		if (ret != 0) {
			LOG_ERR("Leaf item allocation failure");
			return ret;
		}

		item->key = ec_hdr->ec;
		item->value.pnum = pnum;
		rb_insert(&dev->free_pebs, &item->node);
		dev->free_peb_count += 1;

		return SCAN_PEB_HANDLED;
	}

	/* Re-read with CRC validation; corrupt header means bad PEB. */
	memset(vid_hdr, 0, sizeof(*vid_hdr));
	ret = ubi_vid_hdr_read(&dev->mtd, pnum, vid_hdr, true);

	if (ret != 0) {
		struct ubi_list_item *item = NULL;
		ret = ubi_mem_leaf_alloc((void **)&item);

		if (ret != 0) {
			LOG_ERR("Leaf item allocation failure");
			return ret;
		}

		ubi_move_to_bad_blocks(dev, pnum, ec_hdr->ec, item);
		return SCAN_PEB_HANDLED;
	}

	return SCAN_NEXT_STEP;
}

/**
 * \brief Classify an orphan PEB (volume deleted) by moving it to the dirty pool.
 */
static int classify_orphan_peb(struct ubi_device *dev, size_t pnum, const struct ubi_ec_hdr *ec_hdr,
			       const struct ubi_vid_hdr *vid_hdr)
{
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
	rb_insert(&dev->dirty_pebs, &item->node);
	dev->dirty_peb_count += 1;

	return SCAN_PEB_HANDLED;
}

/**
 * \brief Map a LEB that appears for the first time into the volume EBA table.
 *
 * If the LEB index exceeds the volume capacity, the PEB is moved to the dirty pool.
 */
static int map_leb_first_occurrence(struct ubi_device *dev, size_t pnum,
				    const struct ubi_ec_hdr *ec_hdr,
				    const struct ubi_vid_hdr *vid_hdr, struct ubi_volume *vol)
{
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
		rb_insert(&dev->dirty_pebs, &item->node);
		dev->dirty_peb_count += 1;
		return SCAN_PEB_HANDLED;
	}

	item->key = vid_hdr->lnum;
	item->value.pnum = pnum;
	rb_insert(&vol->eba_tbl, &item->node);
	vol->eba_tbl_count += 1;

	return SCAN_PEB_HANDLED;
}

/**
 * \brief Resolve a duplicate LEB by comparing sequence numbers.
 *
 * The PEB with the higher sequence number wins the EBA table slot;
 * the loser is moved to the dirty pool. If the existing PEB's headers
 * cannot be read, it is moved to the bad blocks list.
 */
static int resolve_duplicate_leb(struct ubi_device *dev, size_t pnum, size_t ec_avg,
				 const struct ubi_ec_hdr *ec_hdr, const struct ubi_vid_hdr *vid_hdr,
				 struct ubi_volume *vol, struct ubi_rbt_item *existing)
{
	struct ubi_rbt_item *item = NULL;
	int ret = ubi_mem_leaf_alloc((void **)&item);

	if (ret != 0) {
		LOG_ERR("Leaf item allocation failure");
		return ret;
	}

	struct ubi_ec_hdr exist_ec = { 0 };
	ret = ubi_ec_hdr_read(&dev->mtd, existing->value.pnum, &exist_ec);

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
	ret = ubi_vid_hdr_read(&dev->mtd, existing->value.pnum, &exist_vid, true);

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
		rb_insert(&dev->dirty_pebs, &item->node);
		dev->dirty_peb_count += 1;
	} else {
		/* Current PEB is newer — replace the existing mapping. */
		rb_remove(&vol->eba_tbl, &existing->node);
		vol->eba_tbl_count -= 1;

		existing->key = exist_ec.ec;
		rb_insert(&dev->dirty_pebs, &existing->node);
		dev->dirty_peb_count += 1;

		item->key = vid_hdr->lnum;
		item->value.pnum = pnum;
		rb_insert(&vol->eba_tbl, &item->node);
		vol->eba_tbl_count += 1;
	}

	return SCAN_PEB_HANDLED;
}

/**
 * \brief Scan all PEBs and classify into free, dirty, bad, or EBA table entries.
 */
static int init_scan_pebs(struct ubi_device *ubi_dev, size_t nr_of_pebs, size_t ec_avg)
{
	uint8_t erased_val = 0xFF;
	int ev_ret = ubi_get_erased_val(&ubi_dev->mtd, &erased_val);

	if (ev_ret != 0) {
		LOG_ERR("Failed to query erased value");
		return ev_ret;
	}

	for (size_t pnum = UBI_DEV_HDR_NR_OF_RES_PEBS; pnum < nr_of_pebs; ++pnum) {
		struct ubi_ec_hdr ec_hdr = { 0 };
		int ret = validate_ec_header(ubi_dev, pnum, ec_avg, &ec_hdr);

		if (ret < 0)
			return ret;
		if (ret == SCAN_PEB_HANDLED)
			continue;

		struct ubi_vid_hdr vid_hdr = { 0 };
		ret = validate_vid_header(ubi_dev, pnum, &ec_hdr, &vid_hdr, erased_val);

		if (ret < 0)
			return ret;
		if (ret == SCAN_PEB_HANDLED)
			continue;

		if (vid_hdr.sqnum > ubi_dev->global_sqnum)
			ubi_dev->global_sqnum = vid_hdr.sqnum;

		ret = classify_orphan_peb(ubi_dev, pnum, &ec_hdr, &vid_hdr);

		if (ret < 0)
			return ret;
		if (ret == SCAN_PEB_HANDLED)
			continue;

		struct ubi_rbt_item *vol_entry = ubi_cache_search(&ubi_dev->vols, vid_hdr.vol_id);
		struct ubi_volume *vol = vol_entry->value.vol;

		ret = map_leb_first_occurrence(ubi_dev, pnum, &ec_hdr, &vid_hdr, vol);

		if (ret < 0)
			return ret;
		if (ret == SCAN_PEB_HANDLED)
			continue;

		struct ubi_rbt_item *existing = ubi_cache_search(&vol->eba_tbl, vid_hdr.lnum);
		ret = resolve_duplicate_leb(ubi_dev, pnum, ec_avg, &ec_hdr, &vid_hdr, vol,
					    existing);

		if (ret < 0)
			return ret;
	}

	return 0;
}

/* Module interface function definitions ------------------------------------------------------- */

int ubi_device_init(const struct ubi_mtd *mtd, struct ubi_device **ubi)
{
	int ret = -1;

	if (!mtd || !ubi)
		return -EINVAL;

	/* Check partition availability before allocating — avoids wasting a slab
	 * block when the partition is already in use. */
	ret = ubi_partition_acquire(mtd->partition_id);

	if (ret != 0) {
		LOG_ERR("Partition %u already in use by another UBI handle", mtd->partition_id);
		*ubi = NULL;
		return -EBUSY;
	}

	struct ubi_device *ubi_dev = NULL;
	ret = ubi_mem_device_alloc(&ubi_dev);

	if (ret != 0) {
		LOG_ERR("Device allocation failure");
		ubi_partition_release(mtd->partition_id);
		return ret;
	}
	k_mutex_init(&ubi_dev->mutex);
	ubi_dev->mtd = *mtd;
	ubi_dev->free_pebs.lessthan_fn = ubi_cache_cmp;
	ubi_dev->dirty_pebs.lessthan_fn = ubi_cache_cmp;
	sys_slist_init(&ubi_dev->bad_pebs);
	ubi_dev->vols.lessthan_fn = ubi_cache_cmp;

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
		LOG_ERR("Partition too small: need > %d PEBs for reserved + data",
			UBI_DEV_HDR_NR_OF_RES_PEBS);
		flash_area_close(fa);
		ret = -EINVAL;
		goto exit;
	}

	if (ubi_dev->mtd.erase_block_size < (UBI_EC_HDR_SIZE + UBI_VID_HDR_SIZE)) {
		LOG_ERR("Erase block too small for EC + VID headers");
		flash_area_close(fa);
		ret = -EINVAL;
		goto exit;
	}

	flash_area_close(fa);

	bool is_mounted = false;
	ret = ubi_dev_is_mounted(&ubi_dev->mtd, &is_mounted);

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
	ret = ubi_dev_hdr_read(&ubi_dev->mtd, &dev_hdr);

	if (ret == -EROFS) {
		LOG_WRN("Device in degraded mode: reserved PEB redundancy lost");
		ubi_dev->read_only_degraded = true;
	} else if (ret != 0) {
		LOG_ERR("Device header read failure");
		goto exit;
	}

	/* Cache geometry for fast internal lookups. */
	ubi_dev->total_data_peb_count = nr_of_pebs - UBI_DEV_HDR_NR_OF_RES_PEBS;
	ubi_dev->leb_size = ubi_dev->mtd.erase_block_size - UBI_EC_HDR_SIZE - UBI_VID_HDR_SIZE;

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
#endif

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
