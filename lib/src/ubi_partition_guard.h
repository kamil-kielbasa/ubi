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

/* Include files ------------------------------------------------------------------------------- */
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

/**
 * \brief Force-release all partitions (test-only).
 *
 * Unconditionally clears the entire active-partitions bitfield.
 * Intended for test teardown to prevent cascading EBUSY failures
 * when a prior test assertion aborts before reaching ubi_device_deinit.
 */
void ubi_partition_force_release_all(void);

#endif /* UBI_PARTITION_GUARD_H */
