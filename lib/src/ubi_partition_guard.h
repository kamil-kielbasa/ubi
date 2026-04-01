/**
 * \file    ubi_partition_guard.h
 * \brief   Prevents multiple UBI device handles for the same flash partition.
 *
 * A lightweight static registry keyed by partition_id (uint8_t).
 * Thread-safe: uses a dedicated mutex independent of per-device locks.
 *
 * \copyright Copyright (c) 2026
 */

#ifndef UBI_PARTITION_GUARD_H
#define UBI_PARTITION_GUARD_H

#include <stdint.h>

/**
 * \brief Mark a partition as active (in use by a UBI device handle).
 *
 * \param partition_id  Flash partition identifier.
 *
 * \retval 0       Success — partition was not active and is now claimed.
 * \retval -EBUSY  Partition is already claimed by another handle.
 */
int ubi_partition_acquire(uint8_t partition_id);

/**
 * \brief Release a previously acquired partition.
 *
 * \param partition_id  Flash partition identifier.
 */
void ubi_partition_release(uint8_t partition_id);

#endif /* UBI_PARTITION_GUARD_H */
