/**
 * \file    ubi.c
 * \author  Kamil Kielbasa
 * \brief   Unsorted Block Images (UBI) implementation.
 * \version 0.1
 * \date    2025-07-25
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files ----------------------------------------------------------- */

#include "ubi.h"

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h> /* ARRAY_SIZE */
#include <zephyr/toolchain.h> /* packed for struct's */
#include <zephyr/sys/crc.h> /* CRC32 */
#include <zephyr/drivers/flash.h> /* Flash API */
#include <zephyr/sys/slist.h> /* For sys_snode_t, which is used internally by k_queue */
#include <zephyr/sys/rb.h> /* Red-black tree */
#include <zephyr/logging/log.h>

/* Standard library headers: */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>

/* Module defines ---------------------------------------------------------- */

#define RBT_PTR(p) ((struct rbnode *)((uintptr_t)(p) & ~1))

/* Module types and type definitiones -------------------------------------- */

struct ubi_device {
	struct mem_tech_device mtd;

	size_t eba_tbl_size;
	struct rbtree eba_tbl; /* Key: LEB index, Value: PEB index. */

	size_t free_pebs_size;
	struct rbtree free_pebs; /* Key: erase counter, Value: PEB index. */

	size_t dirty_pebs_size;
	struct rbtree dirty_pebs; /* Key: erase counter, Value: PEB index. */

	size_t bad_pebs_size;
	sys_slist_t bad_pebs; /* Bad PEBS indexes linked-list. */

	uint64_t global_seqnr;
};

#define UBI_DEV_HDR_NR_OF_RES_PEBS (1)
#define UBI_DEV_HDR_RES_PEB_0 (0)
#define UBI_DEV_HDR_MAGIC (0x55424925) // "UBI%"
#define UBI_DEV_HDR_SIZE (32)
#define UBI_DEV_HDR_VERSION (1)

struct ubi_dev_hdr {
	uint32_t magic;
	uint8_t version;
	uint8_t padding_1[3];
	uint32_t offset;
	uint32_t size;
	uint32_t padding_2[3];
	uint32_t vol_tbl_crc;
};

BUILD_ASSERT(sizeof(struct ubi_dev_hdr) == UBI_DEV_HDR_SIZE);

#define UBI_EC_HDR_MAGIC (0x55424923) // "UBI#"
#define UBI_EC_HDR_SIZE (16)
#define UBI_EC_HDR_VERSION (1)

struct ubi_ec_hdr {
	uint32_t magic;
	uint8_t version;
	uint8_t padding[3];
	uint32_t ec;
	uint32_t hdr_crc;
};

BUILD_ASSERT(sizeof(struct ubi_ec_hdr) == UBI_EC_HDR_SIZE);

#define UBI_VID_HDR_MAGIC (0x55424921) // "UBI!"
#define UBI_VID_HDR_SIZE (32)
#define UBI_VID_HDR_VERSION (1)

struct ubi_vid_hdr {
	uint32_t magic;
	uint8_t version;
	uint8_t padding_1[3];
	uint32_t lnum;
	uint32_t data_size;
	uint64_t sqnum;
	uint32_t padding_2;
	uint32_t hdr_crc;
};

BUILD_ASSERT(sizeof(struct ubi_vid_hdr) == UBI_VID_HDR_SIZE);

struct ubi_rbt_item {
	struct rbnode node;
	uint32_t key;
	uint32_t value;
};

BUILD_ASSERT(sizeof(struct ubi_rbt_item) == 16);

struct ubi_list_item {
	sys_snode_t node;
	uint32_t peb_index;
	uint32_t nr_of_erases;
};

BUILD_ASSERT(sizeof(struct ubi_list_item) == 12);

/* Module interface variables and constants -------------------------------- */
/* Static variables and constants ------------------------------------------ */

static struct ubi_device *ubi = NULL;

/* Static function declarations -------------------------------------------- */

static bool ubi_rbt_cmp(struct rbnode *a, struct rbnode *b);
static struct ubi_rbt_item *ubi_rbt_search(struct rbtree *tree, uint32_t key);
static void ubi_rbt_cleanup(struct rbtree *tree);

static void ubi_list_cleanup(sys_slist_t *list);

static int ubi_ec_hdr_read(const struct ubi_device *ubi, const size_t pnum,
			   struct ubi_ec_hdr *ec_hdr);
static int ubi_ec_hdr_write(const struct ubi_device *ubi, const size_t pnum,
			    struct ubi_ec_hdr *ec_hdr);

static int ubi_vid_hdr_read(const struct ubi_device *ubi, const size_t pnum,
			    struct ubi_vid_hdr *vid_hdr, bool check);
static int ubi_vid_hdr_write(struct ubi_device *ubi, const size_t pnum,
			     struct ubi_vid_hdr *vid_hdr);

/* Static function definitions --------------------------------------------- */

static bool ubi_rbt_cmp(struct rbnode *a, struct rbnode *b)
{
	struct ubi_rbt_item *data_a = CONTAINER_OF(a, struct ubi_rbt_item, node);
	struct ubi_rbt_item *data_b = CONTAINER_OF(b, struct ubi_rbt_item, node);

	if (data_a->key > data_b->key)
		return false;

	return true;
}

static struct ubi_rbt_item *ubi_rbt_search(struct rbtree *tree, uint32_t key)
{
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

static void ubi_rbt_cleanup(struct rbtree *tree)
{
	struct rbnode *node;

	while ((node = rb_get_min(tree)) != NULL) {
		struct ubi_rbt_item *entry = CONTAINER_OF(node, struct ubi_rbt_item, node);
		rb_remove(tree, node);
		k_free(entry);
	}
}

static void ubi_list_cleanup(sys_slist_t *list)
{
	struct ubi_list_item *entry, *tmp;

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(list, entry, tmp, node)
	{
		sys_slist_remove(list, NULL, &entry->node);
		k_free(entry); // Free dynamically allocated node
	}
}

static int ubi_ec_hdr_read(const struct ubi_device *ubi, const size_t pnum,
			   struct ubi_ec_hdr *ec_hdr)
{
	if (!ubi)
		return -1;

	const size_t total_nr_of_pebs = ubi->mtd.p_size / ubi->mtd.eb_size;

	if (pnum > total_nr_of_pebs || UBI_DEV_HDR_RES_PEB_0 == pnum)
		return -1;

	struct ubi_ec_hdr hdr = { 0 };
	int ret = flash_read(ubi->mtd.dev, ubi->mtd.p_off + (pnum * ubi->mtd.eb_size), &hdr,
			     sizeof(hdr));
	if (ret != 0) {
		printk("Flash read error: %d\n", ret);
		return -1;
	}

	if (UBI_EC_HDR_MAGIC != hdr.magic ||
	    hdr.hdr_crc != crc32_ieee((const uint8_t *)&hdr, sizeof(hdr) - sizeof(hdr.hdr_crc))) {
		printk("EC header incorrect!\n");
		return -1;
	}

	if (ec_hdr)
		*ec_hdr = hdr;

	return 0;
}

static int ubi_ec_hdr_write(const struct ubi_device *ubi, const size_t pnum,
			    struct ubi_ec_hdr *ec_hdr)
{
	if (!ubi || !ec_hdr)
		return -1;

	const size_t total_nr_of_pebs = ubi->mtd.p_size / ubi->mtd.eb_size;

	if (pnum > total_nr_of_pebs || UBI_DEV_HDR_RES_PEB_0 == pnum)
		return -1;

	ec_hdr->magic = UBI_EC_HDR_MAGIC;
	ec_hdr->version = UBI_EC_HDR_VERSION;
	ec_hdr->hdr_crc =
		crc32_ieee((const uint8_t *)ec_hdr, sizeof(*ec_hdr) - sizeof(ec_hdr->hdr_crc));

	int ret = flash_write(ubi->mtd.dev, ubi->mtd.p_off + (pnum * ubi->mtd.eb_size), ec_hdr,
			      sizeof(*ec_hdr));
	if (ret != 0) {
		printk("Flash write error: %d\n", ret);
		return -1;
	}

	return 0;
}

static int ubi_vid_hdr_read(const struct ubi_device *ubi, const size_t pnum,
			    struct ubi_vid_hdr *vid_hdr, bool check)
{
	if (!ubi)
		return -1;

	const size_t total_nr_of_pebs = ubi->mtd.p_size / ubi->mtd.eb_size;

	if (pnum > total_nr_of_pebs || UBI_DEV_HDR_RES_PEB_0 == pnum)
		return -1;

	struct ubi_vid_hdr hdr = { 0 };
	int ret = flash_read(ubi->mtd.dev,
			     ubi->mtd.p_off + (pnum * ubi->mtd.eb_size) + UBI_EC_HDR_SIZE, &hdr,
			     sizeof(hdr));
	if (ret != 0) {
		printk("Flash read error: %d\n", ret);
		return -1;
	}

	if (vid_hdr)
		*vid_hdr = hdr;

	if (check) {
		if (UBI_VID_HDR_MAGIC != hdr.magic ||
		    hdr.hdr_crc !=
			    crc32_ieee((const uint8_t *)&hdr, sizeof(hdr) - sizeof(hdr.hdr_crc))) {
			printk("VID header incorrect!\n");
			return -1;
		}
	}

	return 0;
}

static int ubi_vid_hdr_write(struct ubi_device *ubi, const size_t pnum, struct ubi_vid_hdr *vid_hdr)
{
	if (!ubi || !vid_hdr)
		return -1;

	const size_t total_nr_of_pebs = ubi->mtd.p_size / ubi->mtd.eb_size;

	if (pnum > total_nr_of_pebs || UBI_DEV_HDR_RES_PEB_0 == pnum)
		return -1;

	vid_hdr->magic = UBI_VID_HDR_MAGIC;
	vid_hdr->version = UBI_VID_HDR_VERSION;
	vid_hdr->sqnum = ubi->global_seqnr++;
	vid_hdr->hdr_crc =
		crc32_ieee((const uint8_t *)vid_hdr, sizeof(*vid_hdr) - sizeof(vid_hdr->hdr_crc));

	int ret = flash_write(ubi->mtd.dev,
			      ubi->mtd.p_off + (pnum * ubi->mtd.eb_size) + UBI_EC_HDR_SIZE, vid_hdr,
			      sizeof(*vid_hdr));
	if (ret != 0) {
		printk("Flash write error: %d\n", ret);
		return -1;
	}

	return 0;
}

/* Module interface function definitions ----------------------------------- */

int ubi_init(const struct mem_tech_device *mtd)
{
	int ret = -1;

	/* 1. Preconditions: */
	if (!mtd)
		return -1;

	if (ubi)
		return 0;

	/* 2. Allocate and initialize UBI device: */
	ubi = k_malloc(sizeof(*ubi));

	if (!ubi)
		return -1;

	memset(ubi, 0, sizeof(*ubi));
	ubi->mtd = *mtd;
	ubi->eba_tbl.lessthan_fn = ubi_rbt_cmp;
	ubi->free_pebs.lessthan_fn = ubi_rbt_cmp;
	ubi->dirty_pebs.lessthan_fn = ubi_rbt_cmp;
	sys_slist_init(&ubi->bad_pebs);

	/* 3. Read UBI volume table: */
	struct ubi_dev_hdr vol_tbl = { 0 };
	ret = flash_read(ubi->mtd.dev, ubi->mtd.p_off, &vol_tbl, sizeof(vol_tbl));
	if (ret != 0) {
		printk("Flash read error: %d\n", ret);
		return -1;
	}

	/* 4. If fresh flash then initialize all PEB's with EC headers only: */
	/* 5. Else scan PEB's and fill UBI device: */
	const size_t total_nr_of_pebs = ubi->mtd.p_size / ubi->mtd.eb_size;

	if (UBI_DEV_HDR_MAGIC != vol_tbl.magic ||
	    crc32_ieee((const uint8_t *)&vol_tbl, sizeof(vol_tbl) - sizeof(vol_tbl.vol_tbl_crc)) !=
		    vol_tbl.vol_tbl_crc) {
		/* 4a. Write fresh UBI volume table: */
		vol_tbl.magic = UBI_DEV_HDR_MAGIC;
		vol_tbl.version = UBI_DEV_HDR_VERSION;
		vol_tbl.offset = ubi->mtd.p_off;
		vol_tbl.size = ubi->mtd.p_size;
		vol_tbl.vol_tbl_crc = crc32_ieee((const uint8_t *)&vol_tbl,
						 sizeof(vol_tbl) - sizeof(vol_tbl.vol_tbl_crc));

		ret = flash_erase(ubi->mtd.dev, ubi->mtd.p_off, ubi->mtd.eb_size);
		if (ret != 0) {
			printk("Flash erase error: %d\n", ret);
			return -1;
		}

		ret = flash_write(ubi->mtd.dev, ubi->mtd.p_off, &vol_tbl, sizeof(vol_tbl));
		if (ret != 0) {
			printk("Flash write error: %d\n", ret);
			return -1;
		}

		for (size_t peb_idx = UBI_DEV_HDR_NR_OF_RES_PEBS; peb_idx < total_nr_of_pebs;
		     ++peb_idx) {
			ret = flash_erase(ubi->mtd.dev,
					  ubi->mtd.p_off + (peb_idx * ubi->mtd.eb_size),
					  ubi->mtd.eb_size);
			if (ret != 0) {
				printk("Flash erase error: %d\n", ret);
				return -1;
			}

			struct ubi_ec_hdr ec_hdr = { .ec = 0 };
			ret = ubi_ec_hdr_write(ubi, peb_idx, &ec_hdr);

			if (ret != 0) {
				printk("ubi_ec_hdr_write error: %d\n", ret);
				return -1;
			}

			struct ubi_rbt_item *new_node = k_malloc(sizeof(*new_node));
			if (NULL == new_node) {
				printk("Malloc error\n");
				return -1;
			}

			new_node->key = ec_hdr.ec;
			new_node->value = peb_idx;
			rb_insert(&ubi->free_pebs, &new_node->node);
			ubi->free_pebs_size += 1;
		}
	} else {
		for (size_t peb_idx = UBI_DEV_HDR_NR_OF_RES_PEBS; peb_idx < total_nr_of_pebs;
		     ++peb_idx) {
			struct ubi_ec_hdr ec_hdr = { 0 };
			ret = ubi_ec_hdr_read(ubi, peb_idx, &ec_hdr);

			if (ret != 0) {
				struct ubi_list_item *new_item = k_malloc(sizeof(*new_item));
				if (NULL == new_item) {
					printk("Malloc error\n");
					return -1;
				}

				new_item->peb_index = peb_idx;
				new_item->nr_of_erases = 0;
				sys_slist_append(&ubi->bad_pebs, &new_item->node);
				ubi->bad_pebs_size += 1;
				continue;
			}

			struct ubi_vid_hdr vid_hdr = { 0 };
			ret = ubi_vid_hdr_read(ubi, peb_idx, &vid_hdr, false);
			if (ret != 0) {
				printk("ubi_vid_hdr_read error: %d\n", ret);
				return -1;
			}

			struct ubi_vid_hdr empty_vid_hdr = { 0 };
			memset(&empty_vid_hdr, 0xff, sizeof(empty_vid_hdr));
			if (0 == memcmp(&vid_hdr, &empty_vid_hdr, sizeof(vid_hdr))) {
				struct ubi_rbt_item *new_node = k_malloc(sizeof(*new_node));
				if (NULL == new_node) {
					printk("Malloc error\n");
					return -1;
				}

				new_node->key = ec_hdr.ec;
				new_node->value = peb_idx;
				rb_insert(&ubi->free_pebs, &new_node->node);
				ubi->free_pebs_size += 1;
				continue;
			}

			ret = ubi_vid_hdr_read(ubi, peb_idx, &vid_hdr, true);

			if (ret != 0) {
				struct ubi_list_item *new_item = k_malloc(sizeof(*new_item));
				if (NULL == new_item) {
					printk("Malloc error\n");
					return -1;
				}

				new_item->peb_index = peb_idx;
				new_item->nr_of_erases = 0;
				sys_slist_append(&ubi->bad_pebs, &new_item->node);
				ubi->bad_pebs_size += 1;
				continue;
			}

			if (vid_hdr.sqnum > ubi->global_seqnr) {
				ubi->global_seqnr = vid_hdr.sqnum;
			}

			struct ubi_rbt_item *exist_node =
				ubi_rbt_search(&ubi->eba_tbl, vid_hdr.lnum);
			struct ubi_rbt_item *new_node = k_malloc(sizeof(*new_node));
			if (NULL == new_node) {
				printk("Malloc error\n");
				return -1;
			}

			if (NULL == exist_node) {
				new_node->key = vid_hdr.lnum;
				new_node->value = peb_idx;
				rb_insert(&ubi->eba_tbl, &new_node->node);
				ubi->eba_tbl_size += 1;
				continue;
			} else {
				struct ubi_vid_hdr exist_vid_hdr = { 0 };
				ret = ubi_vid_hdr_read(ubi, exist_node->value, &exist_vid_hdr,
						       true);

				if (ret != 0) {
					printk("ubi_vid_hdr_read error: %d\n", ret);
					return -1;
				}

				if (exist_vid_hdr.sqnum > vid_hdr.sqnum) {
					new_node->key = ec_hdr.ec;
					new_node->value = peb_idx;
					rb_insert(&ubi->free_pebs, &new_node->node);
					ubi->free_pebs_size += 1;
					continue;
				} else {
					struct ubi_ec_hdr exist_ec_hdr = { 0 };
					ret = ubi_ec_hdr_read(ubi, peb_idx, &exist_ec_hdr);

					rb_remove(&ubi->eba_tbl, &exist_node->node);
					ubi->eba_tbl_size -= 1;
					exist_node->key = exist_ec_hdr.ec;
					rb_insert(&ubi->free_pebs, &exist_node->node);
					ubi->free_pebs_size += 1;

					new_node->key = vid_hdr.lnum;
					new_node->value = peb_idx;
					rb_insert(&ubi->eba_tbl, &new_node->node);
					ubi->eba_tbl_size += 1;
					continue;
				}
			}

			printk("Unhandled scanning scenario!\n");
			return -1;
		}
	}

	return 0;
}

int ubi_deinit(void)
{
	if (!ubi)
		return -1;

	ubi_rbt_cleanup(&ubi->eba_tbl);
	ubi_rbt_cleanup(&ubi->free_pebs);
	ubi_rbt_cleanup(&ubi->dirty_pebs);
	ubi_list_cleanup(&ubi->bad_pebs);

	memset(ubi, 0, sizeof(*ubi));
	k_free(ubi);
	ubi = NULL;

	return 0;
}

int ubi_info(struct ubi_device_info *dev_info, struct ubi_flash_info *flash_info)
{
	if (!ubi)
		return -1;

	if (!dev_info && !flash_info)
		return -1;

	if (dev_info) {
		dev_info->alloc_pebs = ubi->eba_tbl_size;
		dev_info->free_pebs = ubi->free_pebs_size;
		dev_info->dirty_pebs = ubi->dirty_pebs_size;
		dev_info->bad_pebs = ubi->bad_pebs_size;

		const size_t total_nr_of_pebs = ubi->mtd.p_size / ubi->mtd.eb_size;
		dev_info->leb_count = total_nr_of_pebs - UBI_DEV_HDR_NR_OF_RES_PEBS;
		dev_info->leb_size = ubi->mtd.eb_size - UBI_EC_HDR_SIZE - UBI_VID_HDR_SIZE;
	}

	if (flash_info) {
		flash_info->ec_average = 0;
		flash_info->peb_count = ubi->mtd.p_size / ubi->mtd.eb_size;

		flash_info->peb_init =
			k_malloc(sizeof(*flash_info->peb_init) * flash_info->peb_count);
		if (!flash_info->peb_init)
			return -1;

		flash_info->peb_ec = k_malloc(sizeof(*flash_info->peb_ec) * flash_info->peb_count);
		if (!flash_info->peb_ec)
			return -1;

		for (size_t pnum = 0; pnum < flash_info->peb_count; ++pnum) {
			if (UBI_DEV_HDR_RES_PEB_0 == pnum) {
				flash_info->peb_init[pnum] = false;
			} else {
				flash_info->peb_init[pnum] = true;

				struct ubi_ec_hdr ec_hdr = { 0 };
				int ret = ubi_ec_hdr_read(ubi, pnum, &ec_hdr);

				if (0 != ret)
					return -1;

				flash_info->peb_ec[pnum] = ec_hdr.ec;
				flash_info->ec_average += ec_hdr.ec;
			}
		}

		flash_info->ec_average /= (flash_info->peb_count - UBI_DEV_HDR_NR_OF_RES_PEBS);
	}

	return 0;
}

int ubi_leb_write(size_t lnum, const void *buf, size_t len)
{
	if (!ubi || !buf || 0 == len)
		return -1;

	const size_t total_nr_of_pebs = ubi->mtd.p_size / ubi->mtd.eb_size;
	const size_t total_nr_of_lebs = total_nr_of_pebs - UBI_DEV_HDR_NR_OF_RES_PEBS;

	if (lnum > total_nr_of_lebs)
		return -1;

	const size_t max_leb_size = ubi->mtd.eb_size - UBI_EC_HDR_SIZE - UBI_VID_HDR_SIZE;

	if (len > max_leb_size)
		return -1;

	struct ubi_rbt_item *old_item = ubi_rbt_search(&ubi->eba_tbl, lnum);

	if (old_item != NULL) {
		rb_remove(&ubi->eba_tbl, &old_item->node);
		ubi->eba_tbl_size -= 1;

		struct ubi_ec_hdr ec_hdr = { 0 };
		int ret = ubi_ec_hdr_read(ubi, old_item->value, &ec_hdr);

		if (ret != 0) {
			printk("ubi_ec_hdr_read error: %d\n", ret);
			return -1;
		}

		struct ubi_rbt_item *dirty_item = old_item;
		dirty_item->key = ec_hdr.ec;
		rb_insert(&ubi->dirty_pebs, &dirty_item->node);
		ubi->dirty_pebs_size += 1;
	}

	if (ubi->free_pebs_size == 0)
		return -1;

	struct rbnode *min_rbnode = rb_get_min(&ubi->free_pebs);
	struct ubi_rbt_item *min_node = CONTAINER_OF(min_rbnode, struct ubi_rbt_item, node);

	rb_remove(&ubi->free_pebs, &min_node->node);
	ubi->free_pebs_size -= 1;

	min_node->key = lnum;
	rb_insert(&ubi->eba_tbl, &min_node->node);
	ubi->eba_tbl_size += 1;

	struct ubi_vid_hdr vid_hdr = { .lnum = lnum, .data_size = len };
	int ret = ubi_vid_hdr_write(ubi, min_node->value, &vid_hdr);

	if (ret != 0) {
		printk("ubi_vid_hdr_write error: %d\n", ret);
		return -1;
	}

	ret = flash_write(ubi->mtd.dev,
			  ubi->mtd.p_off + (min_node->value * ubi->mtd.eb_size) +
				  sizeof(struct ubi_ec_hdr) + sizeof(struct ubi_vid_hdr),
			  buf, len);

	if (ret != 0) {
		printk("Flash write error: %d\n", ret);
		return -1;
	}

	return 0;
}

int ubi_leb_read(size_t lnum, void *buf, size_t offset, size_t *len)
{
	if (!ubi || !buf || !len)
		return -1;

	const size_t total_nr_of_pebs = ubi->mtd.p_size / ubi->mtd.eb_size;
	const size_t total_nr_of_lebs = total_nr_of_pebs - UBI_DEV_HDR_NR_OF_RES_PEBS;

	if (lnum > total_nr_of_lebs)
		return -1;

	const size_t max_leb_size = ubi->mtd.eb_size - UBI_EC_HDR_SIZE - UBI_VID_HDR_SIZE;

	if (offset > max_leb_size)
		return -1;

	if (ubi->eba_tbl_size == 0)
		return -1;

	struct ubi_rbt_item *found_item = ubi_rbt_search(&ubi->eba_tbl, lnum);

	if (!found_item)
		return -1;

	struct ubi_ec_hdr ec_hdr = { 0 };
	int ret = ubi_ec_hdr_read(ubi, found_item->value, &ec_hdr);

	if (ret != 0) {
		printk("ubi_ec_hdr_read error: %d\n", ret);
		return -1;
	}

	struct ubi_vid_hdr vid_hdr = { 0 };
	ret = ubi_vid_hdr_read(ubi, found_item->value, &vid_hdr, true);

	if (ret != 0) {
		printk("ubi_vid_hdr_read error: %d\n", ret);
		return -1;
	}

	const size_t bytes_to_read = vid_hdr.data_size - offset;
	ret = flash_read(ubi->mtd.dev,
			 ubi->mtd.p_off + (found_item->value * ubi->mtd.eb_size) +
				 sizeof(struct ubi_ec_hdr) + sizeof(struct ubi_vid_hdr),
			 buf, bytes_to_read);

	if (ret != 0) {
		printk("Flash read error: %d\n", ret);
		return -1;
	}

	*len = bytes_to_read;

	return 0;
}

int ubi_leb_map(size_t lnum)
{
	if (!ubi)
		return -1;

	const size_t total_nr_of_pebs = ubi->mtd.p_size / ubi->mtd.eb_size;
	const size_t total_nr_of_lebs = total_nr_of_pebs - UBI_DEV_HDR_NR_OF_RES_PEBS;

	if (lnum > total_nr_of_lebs)
		return -1;

	if (ubi_rbt_search(&ubi->eba_tbl, lnum)) {
		printk("LEB is already mapped\n");
		return -1;
	}

	int ret = -1;

	struct rbnode *min_rbnode = rb_get_min(&ubi->free_pebs);
	struct ubi_rbt_item *min_node = CONTAINER_OF(min_rbnode, struct ubi_rbt_item, node);

	rb_remove(&ubi->free_pebs, &min_node->node);
	ubi->free_pebs_size -= 1;

	struct ubi_vid_hdr vid_hdr = { .lnum = lnum, .data_size = 0 };
	ret = ubi_vid_hdr_write(ubi, min_node->value, &vid_hdr);

	if (ret != 0) {
		printk("ubi_vid_hdr_write error: %d\n", ret);
		return -1;
	}

	min_node->key = lnum;
	rb_insert(&ubi->eba_tbl, &min_node->node);
	ubi->eba_tbl_size += 1;

	return 0;
}

int ubi_leb_is_mapped(size_t lnum, bool *leb_is_mapped)
{
	if (!ubi)
		return -1;

	*leb_is_mapped = (NULL != ubi_rbt_search(&ubi->eba_tbl, lnum));
	return 0;
}

int ubi_leb_unmap(size_t lnum)
{
	if (!ubi)
		return -1;

	struct ubi_rbt_item *eba_item = ubi_rbt_search(&ubi->eba_tbl, lnum);

	if (!eba_item) {
		printk("LEB is not mapped!\n");
		return -1;
	}

	rb_remove(&ubi->eba_tbl, &eba_item->node);
	ubi->eba_tbl_size -= 1;

	struct ubi_ec_hdr ec_hdr = { 0 };
	int ret = ubi_ec_hdr_read(ubi, eba_item->value, &ec_hdr);

	if (ret != 0) {
		printk("ubi_ec_hdr_read error: %d\n", ret);
		return -1;
	}

	eba_item->key = ec_hdr.ec;
	rb_insert(&ubi->dirty_pebs, &eba_item->node);
	ubi->dirty_pebs_size += 1;

	return 0;
}

int ubi_peb_erase(void)
{
	if (!ubi)
		return -1;

	int ret = -1;

	if (ubi->dirty_pebs_size > 0) {
		struct rbnode *dirty_rbnode = rb_get_min(&ubi->dirty_pebs);
		struct ubi_rbt_item *dirty_node =
			CONTAINER_OF(dirty_rbnode, struct ubi_rbt_item, node);

		struct ubi_ec_hdr ec_hdr = { 0 };
		ret = ubi_ec_hdr_read(ubi, dirty_node->value, &ec_hdr);

		if (ret != 0) {
			printk("ubi_ec_hdr_read error: %d\n", ret);
			return -1;
		}

		ret = flash_erase(ubi->mtd.dev,
				  ubi->mtd.p_off + (dirty_node->value * ubi->mtd.eb_size),
				  ubi->mtd.eb_size);
		if (ret != 0) {
			printk("Flash erase error: %d\n", ret);
			return -1;
		}

		ec_hdr.ec += 1;
		ret = ubi_ec_hdr_write(ubi, dirty_node->value, &ec_hdr);

		if (ret != 0) {
			printk("ubi_ec_hdr_write error: %d\n", ret);
			return -1;
		}

		rb_remove(&ubi->dirty_pebs, &dirty_node->node);
		ubi->dirty_pebs_size -= 1;

		struct ubi_rbt_item *free_node = dirty_node;
		free_node->key = ec_hdr.ec;
		rb_insert(&ubi->free_pebs, &free_node->node);
		ubi->free_pebs_size += 1;
	}

	if (ubi->bad_pebs_size > 0) {
		/* TODO: Figure out how to deal with bad pebs as permanent. */
	}

	return 0;
}
