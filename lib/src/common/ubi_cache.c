/**
 * \file    ubi_cache.c
 *
 * \brief   UBI red-black tree and linked-list cache helpers.
 *
 * \author  Kamil Kielbasa
 *
 * \copyright Copyright (c) 2025
 */

/* Include files -------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_cache.h"

/* Zephyr headers: */
#include <zephyr/sys/__assert.h>

/* Module interface function definitions -------------------------------------------------------- */

bool ubi_cache_cmp(struct rbnode *a, struct rbnode *b)
{
	__ASSERT_NO_MSG(a);
	__ASSERT_NO_MSG(b);

	struct ubi_rbt_item *data_a = CONTAINER_OF(a, struct ubi_rbt_item, node);
	struct ubi_rbt_item *data_b = CONTAINER_OF(b, struct ubi_rbt_item, node);

	if (data_a->key > data_b->key)
		return false;

	return true;
}

struct ubi_rbt_item *ubi_cache_search(struct rbtree *tree, uint32_t key)
{
	__ASSERT_NO_MSG(tree);

	struct rbnode *node = tree->root;

	while (node) {
		struct ubi_rbt_item *item = CONTAINER_OF(node, struct ubi_rbt_item, node);

		if (key < item->key) {
			node = RBT_PTR(node->children[0]);
		} else if (key > item->key) {
			node = RBT_PTR(node->children[1]);
		} else {
			return item;
		}
	}

	return NULL;
}
