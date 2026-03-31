/**
 * \file    ubi_cache.h
 *
 * \brief   UBI red-black tree and linked-list cache types and helpers.
 *
 * \author  Kamil Kielbasa
 *
 * \copyright Copyright (c) 2025
 */

/* Include guard ------------------------------------------------------------------------------- */
#ifndef UBI_CACHE_H
#define UBI_CACHE_H

/* Include files ------------------------------------------------------------------------------- */
#include <zephyr/sys/rb.h>
#include <zephyr/sys/slist.h>
#include <zephyr/sys/util.h>

#include <stddef.h>
#include <stdint.h>

/* Defines ------------------------------------------------------------------------------------- */

#define RBT_PTR(p) ((struct rbnode *)((uintptr_t)(p) & ~1))

/* Forward declarations ------------------------------------------------------------------------ */

struct ubi_volume;

/* Types and type definitions ------------------------------------------------------------------ */

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
	uint32_t pnum; /**< Physical eraseblock number. */
	uint32_t erase_count; /**< Number of erase cycles performed on this PEB. */
};

BUILD_ASSERT(sizeof(struct ubi_list_item) == 12);

/* Module interface function declarations ------------------------------------------------------ */

/**
 * \brief Compare two red-black tree nodes.
 *
 * Used as the comparator function for ordering nodes within UBI's red-black tree implementation.
 *
 * \param[in] a 	Pointer to the first node.
 * \param[in] b 	Pointer to the second node.
 *
 * \return true if \p a is less than or equal to \p b, false otherwise.
 */
bool ubi_cache_cmp(struct rbnode *a, struct rbnode *b);

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
struct ubi_rbt_item *ubi_cache_search(struct rbtree *tree, uint32_t key);

#endif /* UBI_CACHE_H */
