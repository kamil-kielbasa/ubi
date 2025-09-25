/**
 * \file    ubi.c
 * \author  Kamil Kielbasa
 * \brief   Unsorted Block Images (UBI) implementation.
 * \version 0.5
 * \date    2025-09-25
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files ------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi.h"
#include "ubi_utils.h"

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/rb.h>
#include <zephyr/sys/slist.h>
#include <zephyr/sys/util.h>
#include <zephyr/storage/flash_map.h>

/* Standard library headers: */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------ */

#define RBT_PTR(p) ((struct rbnode *)((uintptr_t)(p) & ~1))

LOG_MODULE_REGISTER(ubi, CONFIG_UBI_LOG_LEVEL);

/* Module types and type definitions ----------------------------------------------------------- */

/**
 * \brief UBI volume representation.
 *
 * This structure describes a single UBI (Unsorted Block Images) volume
 * and its associated configuration and erase block mapping.
 */
struct ubi_volume {
	size_t vol_idx; /**< Index of the volume within the UBI device. */
	size_t vol_id; /**< Unique identifier of the volume. */
	struct ubi_volume_config cfg; /**< Volume configuration parameters. */

	size_t eba_tbl_size; /**< Size of the eraseblock association (EBA) table. */
	struct rbtree eba_tbl; /**< Red-black tree mapping:
                                     - Key: Logical Erase Block (LEB) index
                                     - Value: Physical Erase Block (PEB) index */
};

BUILD_ASSERT(sizeof(struct ubi_volume) == 48);

/**
 * \brief UBI device representation.
 *
 * This structure describes a UBI device with its PEB management
 * structures and global sequencing information.
 */
struct ubi_device {
	struct k_mutex mutex;

	struct ubi_mtd mtd; /**< Underlying MTD (Memory Technology Device). */

	size_t free_pebs_size; /**< Number of free PEBs available. */
	struct rbtree free_pebs; /**< Red-black tree of free PEBs:
                                     - Key: Erase counter
                                     - Value: PEB index */

	size_t dirty_pebs_size; /**< Number of dirty PEBs (need erasure). */
	struct rbtree dirty_pebs; /**< Red-black tree of dirty PEBs:
                                     - Key: Erase counter
                                     - Value: PEB index */

	size_t bad_pebs_size; /**< Number of bad PEBs detected. */
	sys_slist_t bad_pebs; /**< Singly linked list of bad PEB indices. */

	uint64_t global_seqnr; /**< Global sequence number for updates. */

	size_t vols_seqnr; /**< Volume sequence counter. */
	size_t vols_size; /**< Number of volumes tracked. */
	struct rbtree vols; /**< Red-black tree of volumes:
			       - Key: Volume identifier
			       - Value: Volume pointer */
};

BUILD_ASSERT(sizeof(struct ubi_device) == 112);

/**
 * \brief Red-black tree item used in UBI.
 *
 * Represents a key-value pair stored in the red-black tree
 * structure, mapping either physical eraseblock numbers
 * or volume pointers.
 */
struct ubi_rbt_item {
	struct rbnode node; /**< Red-black tree node linkage. */

	uint32_t key; /**< Key for ordering the node. */

	union {
		uint32_t pnum; /**< Physical eraseblock number. */
		struct ubi_volume *vol; /**< Pointer to a UBI volume. */
	} value; /**< Associated value. */
};

BUILD_ASSERT(sizeof(struct ubi_rbt_item) == 16);

/**
 * \brief List item for UBI bad or tracked PEBs.
 *
 * Represents a node in a singly linked list of tracked PEBs
 * along with metadata such as erase counters.
 */
struct ubi_list_item {
	sys_snode_t node; /**< Linked-list node linkage. */
	uint32_t peb_index; /**< Physical eraseblock index. */
	uint32_t nr_of_erases; /**< Number of erase cycles performed on this PEB. */
};

BUILD_ASSERT(sizeof(struct ubi_list_item) == 12);

/* Module interface variables and constants ---------------------------------------------------- */
/* Static variables and constants -------------------------------------------------------------- */
/* Static function declarations ---------------------------------------------------------------- */

/**
 * \brief Compare two red-black tree nodes.
 *
 * Used as the comparator function for ordering nodes within UBI's red-black tree implementation.
 *
 * \param[in] a 	Pointer to the first node.
 * \param[in] b 	Pointer to the second node.
 *
 * \return true if \p a is less than \p b, false otherwise.
 */
static bool ubi_rbt_cmp(struct rbnode *a, struct rbnode *b);

/**
 * \brief Search for an item in a UBI red-black tree by key.
 *
 * Traverses the given red-black tree to find a node that matches the provided key.
 *
 * \param[in] tree 	Pointer to the red-black tree to search.
 * \param key  		32-bit key to search for.
 *
 * \return Pointer to the matching ubi_rbt_item, or NULL if not found.
 */
static struct ubi_rbt_item *ubi_rbt_search(struct rbtree *tree, uint32_t key);

/**
 * \brief Move a PEB to the bad blocks list.
 *
 * Transfers the given physical erase block (PEB) into the list of bad blocks, updating its
 * associated erase counter metadata.
 *
 * \param[in] ubi     	Pointer to the UBI device structure.
 * \param pnum       	Physical erase block index.
 * \param nr_of_erases 	Number of erasures performed on this PEB.
 * \param[in] bad_item	Pointer to the list item representing the bad PEB.
 */
static void move_to_bad_blocks(struct ubi_device *ubi, size_t pnum, size_t nr_of_erases,
			       struct ubi_list_item *bad_item);

/**
 * \brief Write data to a logical eraseblock (LEB).
 *
 * Writes a buffer of data into a specific logical erase block within a volume, handling volume ID
 * and logical number mapping.
 *
 * \param[in] ubi   	Pointer to the UBI device structure.
 * \param vol_id 	ID of the target volume.
 * \param lnum  	Logical eraseblock number within the volume.
 * \param[in] buf   	Pointer to the data buffer to be written.
 * \param len   	Length of the buffer in bytes.
 *
 * \return 0 on success, negative error code on failure.
 */
static int leb_write(struct ubi_device *ubi, int vol_id, size_t lnum, const void *buf, size_t len);

/* Static function definitions ----------------------------------------------------------------- */

static bool ubi_rbt_cmp(struct rbnode *a, struct rbnode *b)
{
	__ASSERT_NO_MSG(a);
	__ASSERT_NO_MSG(b);

	struct ubi_rbt_item *data_a = CONTAINER_OF(a, struct ubi_rbt_item, node);
	struct ubi_rbt_item *data_b = CONTAINER_OF(b, struct ubi_rbt_item, node);

	if (data_a->key > data_b->key)
		return false;

	return true;
}

static struct ubi_rbt_item *ubi_rbt_search(struct rbtree *tree, uint32_t key)
{
	__ASSERT_NO_MSG(tree);

	struct rbnode *node = tree->root;

	while (node) {
		struct ubi_rbt_item *item = CONTAINER_OF(node, struct ubi_rbt_item, node);

		if (key < item->key) {
			node = RBT_PTR(node->children[0]); // Go left
		} else if (key > item->key) {
			node = RBT_PTR(node->children[1]); // Go right
		} else {
			return item; // Match!
		}
	}

	return NULL;
}

static void move_to_bad_blocks(struct ubi_device *ubi, size_t pnum, size_t nr_of_erases,
			       struct ubi_list_item *bad_item)
{
	__ASSERT_NO_MSG(ubi);
	__ASSERT_NO_MSG(bad_item);

	bad_item->peb_index = pnum;
	bad_item->nr_of_erases = nr_of_erases;
	sys_slist_append(&ubi->bad_pebs, &bad_item->node);
	ubi->bad_pebs_size += 1;
}

static int leb_write(struct ubi_device *ubi, int vol_id, size_t lnum, const void *buf, size_t len)
{
	__ASSERT_NO_MSG(ubi);
	__ASSERT_NO_MSG(vol_id >= 0);
	__ASSERT_NO_MSG((buf && len > 0) || (!buf && len == 0));

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	int ret = -EIO;

	if (0 == ubi->vols_size) {
		LOG_ERR("No volumes present on device");
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_rbt_item *entry = ubi_rbt_search(&ubi->vols, vol_id);

	if (!entry) {
		LOG_ERR("Device volume not found");
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_volume *vol = entry->value.vol;

	if (lnum > vol->cfg.leb_count) {
		LOG_ERR("Volume LEB limit exceeded");
		ret = -EACCES;
		goto exit;
	}

	if (0 == ubi->free_pebs_size) {
		LOG_ERR("Lack of free PEBs");
		ret = -ENOSPC;
		goto exit;
	}

	if (len > (ubi->mtd.erase_block_size - UBI_EC_HDR_SIZE - UBI_VID_HDR_SIZE)) {
		LOG_ERR("Too big buffer to write in LEB");
		ret = -ENOSPC;
		goto exit;
	}

	entry = ubi_rbt_search(&vol->eba_tbl, lnum);

	if (entry) {
		struct ubi_ec_hdr ec_hdr = { 0 };
		ret = ubi_ec_hdr_read(&ubi->mtd, entry->value.pnum, &ec_hdr);

		if (0 != ret) {
			LOG_ERR("EC header read failure");
			goto exit;
		}

		rb_remove(&vol->eba_tbl, &entry->node);
		vol->eba_tbl_size -= 1;

		entry->key = ec_hdr.ec;
		rb_insert(&ubi->dirty_pebs, &entry->node);
		ubi->dirty_pebs_size += 1;
	}

	struct rbnode *min_rbnode = rb_get_min(&ubi->free_pebs);
	struct ubi_rbt_item *min_node = CONTAINER_OF(min_rbnode, struct ubi_rbt_item, node);

	rb_remove(&ubi->free_pebs, &min_node->node);
	ubi->free_pebs_size -= 1;

	struct ubi_vid_hdr vid_hdr = { 0 };
	vid_hdr.magic = UBI_VID_HDR_MAGIC;
	vid_hdr.version = UBI_VID_HDR_VERSION;
	vid_hdr.lnum = lnum;
	vid_hdr.vol_id = vol->vol_id;
	vid_hdr.sqnum = ubi->global_seqnr++;
	vid_hdr.data_size = len;
	vid_hdr.hdr_crc =
		crc32_ieee((const uint8_t *)&vid_hdr, sizeof(vid_hdr) - sizeof(vid_hdr.hdr_crc));

	ret = ubi_vid_hdr_write(&ubi->mtd, min_node->value.pnum, &vid_hdr);

	if (0 != ret) {
		LOG_ERR("VID header write failure");
		goto exit;
	}

	if (buf && len > 0) {
		ret = ubi_leb_data_write(&ubi->mtd, min_node->value.pnum, buf, len);

		if (0 != ret) {
			LOG_ERR("LEB data write failure");
			goto exit;
		}
	}

	struct ubi_rbt_item *alloc_node = min_node;
	alloc_node->key = lnum;
	rb_insert(&vol->eba_tbl, &alloc_node->node);
	vol->eba_tbl_size += 1;

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

/* Module interface function definitions ------------------------------------------------------- */

int ubi_device_init(const struct ubi_mtd *mtd, struct ubi_device **ubi)
{
	int ret = -1;

	if (!mtd || !ubi)
		return -EINVAL;

	struct ubi_device *ubi_dev = k_malloc(sizeof(*ubi_dev));

	if (!ubi_dev) {
		LOG_ERR("Heap allocation failure");
		return -ENOMEM;
	}

	memset(ubi_dev, 0, sizeof(*ubi_dev));
	k_mutex_init(&ubi_dev->mutex);
	ubi_dev->mtd = *mtd;
	ubi_dev->free_pebs.lessthan_fn = ubi_rbt_cmp;
	ubi_dev->dirty_pebs.lessthan_fn = ubi_rbt_cmp;
	sys_slist_init(&ubi_dev->bad_pebs);
	ubi_dev->vols.lessthan_fn = ubi_rbt_cmp;

	const struct flash_area *fa = NULL;
	ret = flash_area_open(ubi_dev->mtd.partition_id, &fa);

	if (0 != ret) {
		LOG_ERR("Flash area open failure");
		return ret;
	}

	if (!flash_area_device_is_ready(fa)) {
		LOG_ERR("Flash area is not ready");
		flash_area_close(fa);
		return -ENODEV;
	}

	const size_t nr_of_pebs = fa->fa_size / ubi_dev->mtd.erase_block_size;
	flash_area_close(fa);

	bool is_mounted = false;
	ret = ubi_dev_is_mounted(&ubi_dev->mtd, &is_mounted);

	if (0 != ret) {
		LOG_ERR("Device check mount failure");
		return ret;
	}

	/* 1. UBI device is not mounted. */
	if (false == is_mounted) {
		ret = ubi_dev_mount(&ubi_dev->mtd);

		if (0 != ret) {
			LOG_ERR("Device mount failure");
			return ret;
		}

		struct ubi_ec_hdr ec_hdr = { 0 };
		ec_hdr.magic = UBI_EC_HDR_MAGIC;
		ec_hdr.version = UBI_EC_HDR_VERSION;
		ec_hdr.ec = 0;
		ec_hdr.hdr_crc = crc32_ieee((const uint8_t *)&ec_hdr,
					    sizeof(ec_hdr) - sizeof(ec_hdr.hdr_crc));

		for (size_t peb_idx = UBI_DEV_HDR_NR_OF_RES_PEBS; peb_idx < nr_of_pebs; ++peb_idx) {
			const struct flash_area *fa = NULL;
			ret = flash_area_open(ubi_dev->mtd.partition_id, &fa);

			if (0 != ret) {
				LOG_ERR("Flash area open failure");
				return ret;
			}

			const size_t offset = peb_idx * ubi_dev->mtd.erase_block_size;
			ret = flash_area_erase(fa, offset, ubi_dev->mtd.erase_block_size);

			if (0 != ret) {
				LOG_ERR("Flash erase failure");
				flash_area_close(fa);
				goto exit;
			}

			flash_area_close(fa);

			ret = ubi_ec_hdr_write(&ubi_dev->mtd, peb_idx, &ec_hdr);

			if (0 != ret) {
				LOG_ERR("EC header write failure");
				goto exit;
			}
		}
	}

	struct ubi_dev_hdr dev_hdr = { 0 };
	ret = ubi_dev_hdr_read(&ubi_dev->mtd, &dev_hdr);

	if (0 != ret) {
		LOG_ERR("Device header read failure");
		return ret;
	}

	/* 2. Collect EBA tables for volumes. */
	for (size_t vol_idx = 0; vol_idx < dev_hdr.vol_count; ++vol_idx) {
		struct ubi_vol_hdr vol_hdr = { 0 };
		ret = ubi_vol_hdr_read(&ubi_dev->mtd, vol_idx, &vol_hdr);

		if (0 != ret) {
			LOG_ERR("Volume header read failure");
			goto exit;
		}

		struct ubi_volume *vol = k_malloc(sizeof(*vol));

		if (!vol) {
			LOG_ERR("Heap allocation failure");
			ret = -ENOMEM;
			goto exit;
		}

		memset(vol, 0, sizeof(*vol));
		vol->vol_idx = vol_idx;
		vol->vol_id = vol_hdr.vol_id;
		memcpy(vol->cfg.name, vol_hdr.name, strlen(vol_hdr.name));
		vol->cfg.type = vol_hdr.vol_type;
		vol->cfg.leb_count = vol_hdr.lebs_count;
		vol->eba_tbl_size = 0;
		vol->eba_tbl.lessthan_fn = ubi_rbt_cmp;

		struct ubi_rbt_item *item = k_malloc(sizeof(*item));

		if (!item) {
			LOG_ERR("Heap allocation failure");
			ret = -ENOMEM;
			k_free(vol);
			goto exit;
		}

		memset(item, 0, sizeof(*item));
		item->key = vol->vol_id;
		item->value.vol = vol;

		rb_insert(&ubi_dev->vols, &item->node);
		ubi_dev->vols_size += 1;

		if (vol->vol_id > ubi_dev->vols_seqnr)
			ubi_dev->vols_seqnr = vol->vol_id;
	}

	if (dev_hdr.vol_count > 0)
		ubi_dev->vols_seqnr += 1;

	size_t ec_sum = 0;
	size_t ec_count = 0;

	/* 3. Scan all PEB's with correct EC header and collect average of erases */
	for (size_t pnum = UBI_DEV_HDR_NR_OF_RES_PEBS; pnum < nr_of_pebs; ++pnum) {
		struct ubi_ec_hdr ec_hdr = { 0 };
		ret = ubi_ec_hdr_read(&ubi_dev->mtd, pnum, &ec_hdr);

		if (0 == ret) {
			ec_sum += ec_hdr.ec;
			ec_count += 1;
		}
	}

	const size_t ec_avg = ec_sum / ec_count;

	/* 4. Scan all PEB's and update volume EBA table:
   	 *    1. If EC header is incorrect, then append to bad PEBs.
   	 *    2. If EC header is correct and VID header is empty, then insert to free PEBs.
   	 *    3. If EC header is correct and VID header is incorrect, then append to bad PEBs.
   	 *    4. If EC header is correct and VID header is correct then:
   	 *       1. Collect greater sequence number from VID.
   	 *       2. Search in volume EBA table LEB with this key exist.
   	 *	 3. Volume does not exist, then insert to dirty PEBs.
         *       4. LEB does not exist, but exceed volume LEB limit, insert to dirty PEBs.
   	 *       5. LEB does not exist, then insert to volume EBA table.
   	 *       6. LEB does exist but EC or VID headers of EBA table LEB are incorrect, then append to bad PEBs.
   	 *       7. LEB does exist and EC and VID headers are correct then:
   	 *          1. If newer LEB has lower sequence number, then append to dirty PEBs.
   	 *          2. If newer LEB has greater sequence number, then remove old LEB
   	 *	       from volume EBA table and append to dirty PEBs. The newer LEB append to
   	 *	       volume EBA table.
   	 */
	for (size_t pnum = UBI_DEV_HDR_NR_OF_RES_PEBS; pnum < nr_of_pebs; ++pnum) {
		/* 4.1 */
		struct ubi_ec_hdr ec_hdr = { 0 };
		ret = ubi_ec_hdr_read(&ubi_dev->mtd, pnum, &ec_hdr);

		if (0 != ret) {
			struct ubi_list_item *item = k_malloc(sizeof(*item));

			if (!item) {
				LOG_ERR("Heap allocation failure");
				ret = -ENOMEM;
				goto exit;
			}

			move_to_bad_blocks(ubi_dev, pnum, ec_avg, item);
			continue;
		}

		/* 4.2 */
		struct ubi_vid_hdr vid_hdr = { 0 };
		ret = ubi_vid_hdr_read(&ubi_dev->mtd, pnum, &vid_hdr, false);

		if (0 != ret) {
			LOG_ERR("VID header read failure");
			goto exit;
		}

		struct ubi_vid_hdr empty_vid_hdr = { 0 };
		memset(&empty_vid_hdr, 0xff, sizeof(empty_vid_hdr));

		if (0 == memcmp(&vid_hdr, &empty_vid_hdr, sizeof(vid_hdr))) {
			struct ubi_rbt_item *item = k_malloc(sizeof(*item));

			if (!item) {
				LOG_ERR("Heap allocation failure");
				ret = -ENOMEM;
				goto exit;
			}

			item->key = ec_hdr.ec;
			item->value.pnum = pnum;
			rb_insert(&ubi_dev->free_pebs, &item->node);
			ubi_dev->free_pebs_size += 1;

			continue;
		}

		/* 4.3 */
		memset(&vid_hdr, 0, sizeof(vid_hdr));
		ret = ubi_vid_hdr_read(&ubi_dev->mtd, pnum, &vid_hdr, true);

		if (0 != ret) {
			struct ubi_list_item *item = k_malloc(sizeof(*item));

			if (!item) {
				LOG_ERR("Heap allocation failure");
				ret = -ENOMEM;
				goto exit;
			}

			move_to_bad_blocks(ubi_dev, pnum, ec_hdr.ec, item);
			continue;
		}

		/* 4.4 */

		/* 4.4.1 */
		if (vid_hdr.sqnum > ubi_dev->global_seqnr)
			ubi_dev->global_seqnr = vid_hdr.sqnum;

		/* 4.4.2 */
		struct ubi_rbt_item *tmp = ubi_rbt_search(&ubi_dev->vols, vid_hdr.vol_id);

		/* 4.4.3 */
		if (!tmp) {
			struct ubi_rbt_item *item = k_malloc(sizeof(*item));

			if (!item) {
				LOG_ERR("Heap allocation failure");
				ret = -ENOMEM;
				goto exit;
			}

			item->key = ec_hdr.ec;
			item->value.pnum = pnum;
			rb_insert(&ubi_dev->dirty_pebs, &item->node);
			ubi_dev->dirty_pebs_size += 1;

			continue;
		}

		struct ubi_volume *vol = tmp->value.vol;

		struct ubi_rbt_item *item = k_malloc(sizeof(*item));

		if (!item) {
			LOG_ERR("Heap allocation failure");
			ret = -ENOMEM;
			goto exit;
		}

		tmp = ubi_rbt_search(&vol->eba_tbl, vid_hdr.lnum);

		if (!tmp) {
			/* 4.4.4 */
			if (vid_hdr.lnum >= vol->cfg.leb_count) {
				item->key = ec_hdr.ec;
				item->value.pnum = pnum;
				rb_insert(&ubi_dev->dirty_pebs, &item->node);
				ubi_dev->dirty_pebs_size += 1;
				continue;
			}

			/* 4.4.5 */
			item->key = vid_hdr.lnum;
			item->value.pnum = pnum;
			rb_insert(&vol->eba_tbl, &item->node);
			vol->eba_tbl_size += 1;

			continue;
		} else {
			/* 4.4.6 */
			struct ubi_ec_hdr exist_ec_hdr = { 0 };
			ret = ubi_ec_hdr_read(&ubi_dev->mtd, tmp->value.pnum, &exist_ec_hdr);

			if (0 != ret) {
				struct ubi_list_item *bad_item = k_malloc(sizeof(*bad_item));

				if (!bad_item) {
					LOG_ERR("Heap allocation failure");
					k_free(item);
					ret = -ENOMEM;
					goto exit;
				}

				move_to_bad_blocks(ubi_dev, tmp->value.pnum, ec_avg, bad_item);
				continue;
			}

			struct ubi_vid_hdr exist_vid_hdr = { 0 };
			ret = ubi_vid_hdr_read(&ubi_dev->mtd, tmp->value.pnum, &exist_vid_hdr,
					       true);

			if (0 != ret) {
				struct ubi_list_item *bad_item = k_malloc(sizeof(*bad_item));

				if (!bad_item) {
					LOG_ERR("Heap allocation failure");
					k_free(item);
					ret = -ENOMEM;
					goto exit;
				}

				move_to_bad_blocks(ubi_dev, tmp->value.pnum, ec_hdr.ec, bad_item);
				continue;
			}

			/* 4.4.7.1 */
			if (vid_hdr.sqnum < exist_vid_hdr.sqnum) {
				item->key = ec_hdr.ec;
				item->value.pnum = pnum;
				rb_insert(&ubi_dev->dirty_pebs, &item->node);
				ubi_dev->dirty_pebs_size += 1;

				continue;
			} else {
				/* 4.4.7.2 */
				rb_remove(&vol->eba_tbl, &tmp->node);
				vol->eba_tbl_size -= 1;

				tmp->key = exist_ec_hdr.ec;
				rb_insert(&ubi_dev->dirty_pebs, &tmp->node);
				ubi_dev->dirty_pebs_size += 1;

				item->key = vid_hdr.lnum;
				item->key = pnum;
				rb_insert(&vol->eba_tbl, &item->node);
				vol->eba_tbl_size += 1;

				continue;
			}
		}
	}

	*ubi = ubi_dev;
	return 0;

exit:
	ubi_device_deinit(ubi_dev);
	*ubi = NULL;
	return ret;
}

int ubi_device_get_info(struct ubi_device *ubi, struct ubi_device_info *info)
{
	if (!ubi || !info)
		return -EINVAL;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	const struct flash_area *fa = NULL;
	int ret = flash_area_open(ubi->mtd.partition_id, &fa);

	if (0 != ret) {
		LOG_ERR("Flash area open failure");
		goto exit;
	}

	memset(info, 0, sizeof(*info));
	info->leb_total_count =
		(fa->fa_size / ubi->mtd.erase_block_size) - UBI_DEV_HDR_NR_OF_RES_PEBS;
	info->leb_size = ubi->mtd.erase_block_size - UBI_EC_HDR_SIZE - UBI_VID_HDR_SIZE;

	info->free_leb_count = ubi->free_pebs_size;
	info->dirty_leb_count = ubi->dirty_pebs_size;
	info->bad_leb_count = ubi->bad_pebs_size;

	flash_area_close(fa);

	if (ubi->vols_size > 0) {
		struct ubi_rbt_item *entry = NULL;
		RB_FOR_EACH_CONTAINER(&ubi->vols, entry, node)
		{
			const struct ubi_volume *vol = entry->value.vol;
			info->allocated_leb_count += vol->cfg.leb_count;
		}
		info->volumes_count = ubi->vols_size;
	} else {
		info->allocated_leb_count = 0;
		info->volumes_count = 0;
	}

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

int ubi_device_erase_peb(struct ubi_device *ubi)
{
	if (!ubi)
		return -EINVAL;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	int ret = -EIO;

	struct ubi_list_item *bad_item = k_malloc(sizeof(*bad_item));

	if (!bad_item) {
		LOG_ERR("Heap allocation failure");
		return -ENOMEM;
	}

	if (ubi->dirty_pebs_size > 0) {
		struct rbnode *node = rb_get_min(&ubi->dirty_pebs);
		struct ubi_rbt_item *entry = CONTAINER_OF(node, struct ubi_rbt_item, node);

		struct ubi_ec_hdr ec_hdr = { 0 };
		ret = ubi_ec_hdr_read(&ubi->mtd, entry->value.pnum, &ec_hdr);

		if (0 != ret) {
			LOG_ERR("EC header read failure");

			move_to_bad_blocks(ubi, entry->value.pnum, entry->key, bad_item);
			k_free(entry);

			goto bad_blocks;
		}

		const struct flash_area *fa = NULL;
		ret = flash_area_open(ubi->mtd.partition_id, &fa);

		if (0 != ret) {
			LOG_ERR("Flash area open failure");
			goto bad_blocks;
		}

		const size_t offset = entry->value.pnum * ubi->mtd.erase_block_size;
		ret = flash_area_erase(fa, offset, ubi->mtd.erase_block_size);
		flash_area_close(fa);

		if (0 != ret) {
			LOG_ERR("Flash erase failure");

			move_to_bad_blocks(ubi, entry->value.pnum, entry->key, bad_item);
			k_free(entry);

			goto bad_blocks;
		}

		ec_hdr.ec += 1;
		ec_hdr.hdr_crc = crc32_ieee((const uint8_t *)&ec_hdr,
					    sizeof(ec_hdr) - sizeof(ec_hdr.hdr_crc));
		ret = ubi_ec_hdr_write(&ubi->mtd, entry->value.pnum, &ec_hdr);

		if (0 != ret) {
			LOG_ERR("EC header write failure");

			move_to_bad_blocks(ubi, entry->value.pnum, entry->key, bad_item);
			k_free(entry);

			goto bad_blocks;
		}

		rb_remove(&ubi->dirty_pebs, &entry->node);
		ubi->dirty_pebs_size -= 1;

		entry->key = ec_hdr.ec;
		rb_insert(&ubi->free_pebs, &entry->node);
		ubi->free_pebs_size += 1;
	}

	k_free(bad_item);

bad_blocks:
	if (ubi->bad_pebs_size > 0) {
		/** TODO: Torture bad blocks. */
	}

	k_mutex_unlock(&ubi->mutex);
	return 0;
}

int ubi_device_deinit(struct ubi_device *ubi)
{
	if (!ubi)
		return -EINVAL;

	struct rbnode *node = NULL;
	struct ubi_rbt_item *rbt_item = NULL;
	struct ubi_rbt_item *vol_item = NULL;

	struct ubi_list_item *list_item = NULL;
	struct ubi_list_item *list_next = NULL;

	while ((node = rb_get_min(&ubi->free_pebs))) {
		rbt_item = CONTAINER_OF(node, struct ubi_rbt_item, node);
		rb_remove(&ubi->free_pebs, &rbt_item->node);
		k_free(rbt_item);
		ubi->free_pebs_size -= 1;
	}

	while ((node = rb_get_min(&ubi->dirty_pebs))) {
		rbt_item = CONTAINER_OF(node, struct ubi_rbt_item, node);
		rb_remove(&ubi->dirty_pebs, &rbt_item->node);
		k_free(rbt_item);
		ubi->dirty_pebs_size -= 1;
	}

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&ubi->bad_pebs, list_item, list_next, node)
	{
		sys_slist_remove(&ubi->bad_pebs, NULL, &list_item->node);
		k_free(list_item);
		ubi->bad_pebs_size -= 1;
	}

	while ((node = rb_get_min(&ubi->vols))) {
		rbt_item = CONTAINER_OF(node, struct ubi_rbt_item, node);
		rb_remove(&ubi->vols, &rbt_item->node);

		struct ubi_volume *vol = rbt_item->value.vol;
		while ((node = rb_get_min(&vol->eba_tbl))) {
			vol_item = CONTAINER_OF(node, struct ubi_rbt_item, node);
			rb_remove(&vol->eba_tbl, &vol_item->node);
			k_free(vol_item);
			vol->eba_tbl_size -= 1;
		}

		k_free(rbt_item->value.vol);
		k_free(rbt_item);
		ubi->vols_size -= 1;
	}

	k_free(ubi);
	return 0;
}

#if defined(CONFIG_UBI_TEST_API_ENABLE)

int ubi_device_get_peb_ec(struct ubi_device *ubi, size_t **peb_ec, size_t *len)
{
	int ret = -EIO;

	if (!ubi || !peb_ec || !len)
		return -EINVAL;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	const struct flash_area *fa = NULL;

	ret = flash_area_open(ubi->mtd.partition_id, &fa);

	if (0 != ret) {
		LOG_ERR("Flash area open failure");
		goto exit;
	}

	const size_t nr_of_pebs =
		(fa->fa_size / ubi->mtd.erase_block_size) - UBI_DEV_HDR_NR_OF_RES_PEBS;

	flash_area_close(fa);

	size_t *_peb_ec = k_malloc(nr_of_pebs * sizeof(*_peb_ec));

	if (!_peb_ec) {
		LOG_ERR("Heap allocation failure");
		ret = -ENOMEM;
		goto exit;
	}

	for (size_t pnum = 0; pnum < nr_of_pebs; ++pnum) {
		struct ubi_ec_hdr ec_hdr = { 0 };
		ret = ubi_ec_hdr_read(&ubi->mtd, pnum + UBI_DEV_HDR_NR_OF_RES_PEBS, &ec_hdr);

		if (0 != ret) {
			LOG_ERR("EC header read failure");
			k_free(_peb_ec);
			goto exit;
		}

		_peb_ec[pnum] = ec_hdr.ec;
	}

	*len = nr_of_pebs;
	*peb_ec = _peb_ec;

exit:
	k_mutex_unlock(&ubi->mutex);
	return 0;
}

#endif /* CONFIG_UBI_TEST_API_ENABLE */

int ubi_volume_create(struct ubi_device *ubi, const struct ubi_volume_config *vol_cfg, int *vol_id)
{
	int ret = -EIO;

	if (!ubi || !vol_cfg || !vol_id)
		return -EINVAL;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	/* 1. Check if volume exist */
	const size_t name_len = strnlen(vol_cfg->name, UBI_VOLUME_NAME_MAX_LEN);

	struct ubi_rbt_item *entry = NULL;
	RB_FOR_EACH_CONTAINER(&ubi->vols, entry, node)
	{
		const struct ubi_volume *vol = entry->value.vol;
		const size_t len = strnlen(vol->cfg.name, UBI_VOLUME_NAME_MAX_LEN);

		if (name_len == len && 0 == memcmp(vol_cfg->name, vol->cfg.name, name_len)) {
			*vol_id = vol->vol_id;
			ret = 0;
			goto exit;
		}
	}

	/* 2. Create volume */
	struct ubi_device_info info = { 0 };
	ret = ubi_device_get_info(ubi, &info);

	if (0 != ret) {
		LOG_ERR("UBI device get info failure");
		goto exit;
	}

	const size_t total_free_pebs = info.leb_total_count - info.allocated_leb_count;
	if (vol_cfg->leb_count > total_free_pebs) {
		LOG_ERR("Failed to allocate PEBs for volume");
		ret = -ENOSPC;
		goto exit;
	}

	struct ubi_dev_hdr dev_hdr = { 0 };
	ret = ubi_dev_hdr_read(&ubi->mtd, &dev_hdr);

	if (0 != ret) {
		LOG_ERR("Device header read failure");
		goto exit;
	}

	struct ubi_dev_hdr new_dev_hdr = dev_hdr;
	new_dev_hdr.revision += 1;
	new_dev_hdr.vol_count += 1;
	new_dev_hdr.hdr_crc = crc32_ieee((const uint8_t *)&new_dev_hdr,
					 sizeof(new_dev_hdr) - sizeof(new_dev_hdr.hdr_crc));

	struct ubi_vol_hdr new_vol_hdr = { 0 };
	new_vol_hdr.magic = UBI_VOL_HDR_MAGIC;
	new_vol_hdr.version = UBI_VOL_HDR_VERSION;
	new_vol_hdr.vol_type = vol_cfg->type;
	new_vol_hdr.vol_id = ubi->vols_seqnr++;
	new_vol_hdr.lebs_count = vol_cfg->leb_count;
	strncpy(new_vol_hdr.name, vol_cfg->name, UBI_VOLUME_NAME_MAX_LEN);
	new_vol_hdr.hdr_crc = crc32_ieee((const uint8_t *)&new_vol_hdr,
					 sizeof(new_vol_hdr) - sizeof(new_vol_hdr.hdr_crc));

	ret = ubi_vol_hdr_append(&ubi->mtd, &new_dev_hdr, &new_vol_hdr);

	if (0 != ret) {
		LOG_ERR("Volume header append failure");
		goto exit;
	}

	struct ubi_volume *vol = k_malloc(sizeof(*vol));
	if (!vol) {
		LOG_ERR("Heap allocation failure");
		ret = -ENOMEM;
		goto exit;
	}

	memset(vol, 0, sizeof(*vol));
	vol->vol_idx = new_dev_hdr.vol_count - 1;
	vol->vol_id = new_vol_hdr.vol_id;
	memcpy(vol->cfg.name, new_vol_hdr.name, strlen(new_vol_hdr.name));
	vol->cfg.type = new_vol_hdr.vol_type;
	vol->cfg.leb_count = new_vol_hdr.lebs_count;
	vol->eba_tbl_size = 0;
	vol->eba_tbl.lessthan_fn = ubi_rbt_cmp;

	struct ubi_rbt_item *item = k_malloc(sizeof(*item));
	if (!item) {
		LOG_ERR("Heap allocation failure");
		k_free(vol);
		ret = -ENOMEM;
		goto exit;
	}

	item->key = vol->vol_id;
	item->value.vol = vol;
	rb_insert(&ubi->vols, &item->node);
	ubi->vols_size += 1;

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

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	if (0 == ubi->vols_size) {
		LOG_ERR("No volumes present on device");
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_rbt_item *entry = ubi_rbt_search(&ubi->vols, vol_id);

	if (!entry) {
		LOG_ERR("Device volume not found");
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_volume *vol = entry->value.vol;

	if (UBI_VOLUME_TYPE_DYNAMIC != vol->cfg.type) {
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
		struct ubi_device_info info = { 0 };
		ret = ubi_device_get_info(ubi, &info);

		if (0 != ret) {
			LOG_ERR("Device get info failure");
			goto exit;
		}

		const size_t avail = info.leb_total_count - info.allocated_leb_count;
		const size_t diff = vol_cfg->leb_count - vol->cfg.leb_count;

		if (diff > avail) {
			LOG_ERR("Lack of available for allocation LEBs");
			ret = -ENOSPC;
			goto exit;
		}
	} else {
		const size_t diff = vol->cfg.leb_count - vol_cfg->leb_count;

		if (0 == diff) {
			LOG_ERR("Cannot resize volume to zero LEBs");
			ret = -ECANCELED;
			goto exit;
		}

		for (size_t lnum = (vol->cfg.leb_count - diff); lnum < vol->cfg.leb_count; ++lnum) {
			struct ubi_rbt_item *item = ubi_rbt_search(&vol->eba_tbl, lnum);

			if (item) {
				rb_remove(&vol->eba_tbl, &item->node);
				vol->eba_tbl_size -= 1;

				struct ubi_ec_hdr ec_hdr = { 0 };
				ret = ubi_ec_hdr_read(&ubi->mtd, item->value.pnum, &ec_hdr);

				if (0 != ret) {
					LOG_ERR("EC header read failure");
					goto exit;
				}

				item->key = ec_hdr.ec;
				rb_insert(&ubi->dirty_pebs, &item->node);
				ubi->dirty_pebs_size += 1;
			}
		}
	}

	struct ubi_dev_hdr dev_hdr = { 0 };
	ret = ubi_dev_hdr_read(&ubi->mtd, &dev_hdr);

	if (0 != ret) {
		LOG_ERR("Device header read failure");
		goto exit;
	}

	dev_hdr.revision += 1;
	dev_hdr.hdr_crc =
		crc32_ieee((const uint8_t *)&dev_hdr, sizeof(dev_hdr) - sizeof(dev_hdr.hdr_crc));

	struct ubi_vol_hdr vol_hdr = { 0 };
	ret = ubi_vol_hdr_read(&ubi->mtd, vol->vol_idx, &vol_hdr);

	if (0 != ret) {
		LOG_ERR("Volume header read failure");
		goto exit;
	}

	vol_hdr.lebs_count = vol_cfg->leb_count;
	vol_hdr.hdr_crc =
		crc32_ieee((const uint8_t *)&vol_hdr, sizeof(vol_hdr) - sizeof(vol_hdr.hdr_crc));

	ret = ubi_vol_hdr_update(&ubi->mtd, &dev_hdr, vol->vol_idx, &vol_hdr);

	if (0 != ret) {
		LOG_ERR("Volume header update failure");
		goto exit;
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

	if (0 == ubi->vols_size) {
		LOG_ERR("No volumes present on device");
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_rbt_item *entry = ubi_rbt_search(&ubi->vols, vol_id);

	if (!entry) {
		LOG_ERR("Device volume not found");
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_dev_hdr dev_hdr = { 0 };
	ret = ubi_dev_hdr_read(&ubi->mtd, &dev_hdr);

	if (0 != ret) {
		LOG_ERR("Device header read failure");
		goto exit;
	}

	dev_hdr.vol_count -= 1;
	dev_hdr.revision += 1;
	dev_hdr.hdr_crc =
		crc32_ieee((const uint8_t *)&dev_hdr, sizeof(dev_hdr) - sizeof(dev_hdr.hdr_crc));

	struct ubi_volume *vol = entry->value.vol;
	ret = ubi_vol_hdr_remove(&ubi->mtd, &dev_hdr, vol->vol_idx);

	if (0 != ret) {
		LOG_ERR("Volume header remove failure");
		goto exit;
	}

	struct ubi_rbt_item *item = NULL;
	RB_FOR_EACH_CONTAINER(&vol->eba_tbl, item, node)
	{
		rb_remove(&vol->eba_tbl, &item->node);
		vol->eba_tbl_size -= 1;

		struct ubi_ec_hdr ec_hdr = { 0 };
		ret = ubi_ec_hdr_read(&ubi->mtd, item->value.pnum, &ec_hdr);

		if (0 != ret) {
			LOG_ERR("EC header read failure");
			goto exit;
		}

		item->key = ec_hdr.ec;
		rb_insert(&ubi->dirty_pebs, &item->node);
		ubi->dirty_pebs_size += 1;
	}

	rb_remove(&ubi->vols, &entry->node);
	ubi->vols_size -= 1;

	k_free(entry->value.vol);
	k_free(entry);

	for (size_t vol_idx = 0; vol_idx < dev_hdr.vol_count; ++vol_idx) {
		struct ubi_vol_hdr vol_hdr = { 0 };
		ret = ubi_vol_hdr_read(&ubi->mtd, vol_idx, &vol_hdr);

		if (0 != ret) {
			LOG_ERR("Volume header readd failure");
			goto exit;
		}

		entry = ubi_rbt_search(&ubi->vols, vol_hdr.vol_id);

		if (!entry) {
			LOG_ERR("Inconsistency between cache and nvm");
			ret = -EIO;
			goto exit;
		}

		vol = entry->value.vol;
		vol->vol_idx = vol_idx;
	}

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

	if (0 == ubi->vols_size) {
		LOG_ERR("No volumes present on device");
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_rbt_item *entry = ubi_rbt_search(&ubi->vols, vol_id);

	if (!entry) {
		LOG_ERR("Device volume not found");
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_volume *vol = entry->value.vol;
	*vol_cfg = vol->cfg;
	*alloc_lebs = vol->eba_tbl_size;
	ret = 0;

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}

int ubi_leb_write(struct ubi_device *ubi, int vol_id, size_t lnum, const void *buf, size_t len)
{
	if (!ubi || vol_id < 0 || !buf || 0 == len)
		return -EINVAL;

	return leb_write(ubi, vol_id, lnum, buf, len);
}

int ubi_leb_read(struct ubi_device *ubi, int vol_id, size_t lnum, size_t offset, void *buf,
		 size_t size)
{
	int ret = -EIO;

	if (!ubi || vol_id < 0 || !buf || 0 == size)
		return -EINVAL;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	if (0 == ubi->vols_size) {
		LOG_ERR("No volumes present on device");
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_rbt_item *entry = ubi_rbt_search(&ubi->vols, vol_id);

	if (!entry) {
		LOG_ERR("Device volume not found");
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_volume *vol = entry->value.vol;

	if (lnum > vol->cfg.leb_count) {
		LOG_ERR("Volume LEB limit exceeded");
		ret = -EACCES;
		goto exit;
	}

	entry = ubi_rbt_search(&vol->eba_tbl, lnum);

	if (!entry) {
		LOG_ERR("LEB not found");
		ret = -ENOENT;
		goto exit;
	}

	ret = ubi_leb_data_read(&ubi->mtd, entry->value.pnum, offset, buf, size);

	if (0 != ret) {
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

	return leb_write(ubi, vol_id, lnum, NULL, 0);
}

int ubi_leb_unmap(struct ubi_device *ubi, int vol_id, size_t lnum)
{
	int ret = -EIO;

	if (!ubi || vol_id < 0)
		return -EINVAL;

	k_mutex_lock(&ubi->mutex, K_FOREVER);

	if (0 == ubi->vols_size) {
		LOG_ERR("No volumes present on device");
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_rbt_item *entry = ubi_rbt_search(&ubi->vols, vol_id);

	if (!entry) {
		LOG_ERR("Device volume not found");
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_volume *vol = entry->value.vol;

	if (lnum > vol->cfg.leb_count) {
		LOG_ERR("Volume LEB limit exceeded");
		ret = -EACCES;
		goto exit;
	}

	entry = ubi_rbt_search(&vol->eba_tbl, lnum);

	if (!entry) {
		LOG_ERR("Cannot unmap an unmapped LEB");
		ret = -EACCES;
		goto exit;
	}

	struct ubi_ec_hdr ec_hdr = { 0 };
	ret = ubi_ec_hdr_read(&ubi->mtd, entry->value.pnum, &ec_hdr);

	if (0 != ret) {
		LOG_ERR("EC header read failure");
		goto exit;
	}

	rb_remove(&vol->eba_tbl, &entry->node);
	vol->eba_tbl_size -= 1;

	entry->key = ec_hdr.ec;
	rb_insert(&ubi->dirty_pebs, &entry->node);
	ubi->dirty_pebs_size += 1;

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

	if (0 == ubi->vols_size) {
		LOG_ERR("No volumes present on device");
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_rbt_item *entry = ubi_rbt_search(&ubi->vols, vol_id);

	if (!entry) {
		LOG_ERR("Device volume not found");
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_volume *vol = entry->value.vol;

	if (lnum > vol->cfg.leb_count) {
		LOG_ERR("Volume LEB limit exceeded");
		ret = -EACCES;
		goto exit;
	}

	entry = ubi_rbt_search(&vol->eba_tbl, lnum);

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

	if (0 == ubi->vols_size) {
		LOG_ERR("No volumes present on device");
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_rbt_item *entry = ubi_rbt_search(&ubi->vols, vol_id);

	if (!entry) {
		LOG_ERR("Device volume not found");
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_volume *vol = entry->value.vol;

	if (lnum > vol->cfg.leb_count) {
		LOG_ERR("Volume LEB limit exceeded");
		ret = -EACCES;
		goto exit;
	}

	entry = ubi_rbt_search(&vol->eba_tbl, lnum);

	if (!entry) {
		LOG_ERR("LEB %zu in volume %d is not mapped", lnum, vol_id);
		ret = -ENOENT;
		goto exit;
	}

	struct ubi_vid_hdr vid_hdr = { 0 };
	ret = ubi_vid_hdr_read(&ubi->mtd, entry->value.pnum, &vid_hdr, true);

	if (0 != ret) {
		LOG_ERR("VID header read failure");
		goto exit;
	}

	*size = vid_hdr.data_size;

exit:
	k_mutex_unlock(&ubi->mutex);
	return ret;
}
