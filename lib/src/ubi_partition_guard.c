/**
 * \file    ubi_partition_guard.c
 * \brief   Static registry preventing double-init of the same flash partition.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files ------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_partition_guard.h"

/* Zephyr headers: */
#include <zephyr/kernel.h>

/* Standard library headers: */
#include <errno.h>

/* Module defines ------------------------------------------------------------------------------ */

/** One bit per partition ID in the static \c active_partitions bitfield (IDs 0 … N-1). */
#define UBI_PARTITION_GUARD_TRACKED_PARTITIONS 32

/* Static storage ------------------------------------------------------------------------------ */

static K_MUTEX_DEFINE(guard_mutex);
static uint32_t active_partitions;

/* Module interface function definitions ------------------------------------------------------- */

int ubi_partition_acquire(uint8_t partition_id)
{
	if (partition_id >= UBI_PARTITION_GUARD_TRACKED_PARTITIONS) {
		return -EINVAL;
	}

	k_mutex_lock(&guard_mutex, K_FOREVER);

	const uint32_t mask = (uint32_t)1U << partition_id;

	if (active_partitions & mask) {
		k_mutex_unlock(&guard_mutex);
		return -EBUSY;
	}

	active_partitions |= mask;
	k_mutex_unlock(&guard_mutex);
	return 0;
}

void ubi_partition_release(uint8_t partition_id)
{
	if (partition_id >= UBI_PARTITION_GUARD_TRACKED_PARTITIONS) {
		return;
	}

	k_mutex_lock(&guard_mutex, K_FOREVER);
	active_partitions &= ~((uint32_t)1U << partition_id);
	k_mutex_unlock(&guard_mutex);
}
