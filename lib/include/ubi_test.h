/**
 * \file    ubi_test.h
 *
 * \brief   UBI test-only API — fault injection, invariant checks, diagnostics.
 *
 * This header is only available when CONFIG_UBI_TEST_API_ENABLE is set.
 * Do not include in production builds.
 *
 * \author  Kamil Kielbasa
 *
 * \copyright Copyright (c) 2026
 */

/* Include guard ------------------------------------------------------------------------------- */
#ifndef UBI_TEST_H
#define UBI_TEST_H

/* Include files ------------------------------------------------------------------------------- */
#include <ubi.h>

/* Test API ------------------------------------------------------------------------------------ */

#if defined(CONFIG_UBI_TEST_API_ENABLE)

/**
 * \brief Verify internal consistency invariants (test API).
 *
 * Checks that the sum of all tracked PEBs (free + dirty + bad + mapped)
 * equals total_data_peb_count, that tree sizes match their counters,
 * and that reserved_peb_count matches the sum of vol->cfg.leb_count.
 *
 * \param[in] ubi  UBI device handle.
 *
 * \retval 0       All invariants hold.
 * \retval -EINVAL  NULL pointer.
 * \retval -EIO     An invariant was violated (details logged).
 */
int ubi_device_check_invariants(struct ubi_device *ubi);

/**
 * \brief Retrieve per-PEB erase counters (test API).
 *
 * Allocates an array of erase counters, one per data PEB. The caller
 * must free the array with k_free() when done.
 *
 * \param[in] ubi 		UBI device handle.
 * \param[out] peb_ec		Pointer to receive the erase-counter array.
 * \param[out] len		Number of entries in the array.
 *
 * \retval 0       Success.
 * \retval -EINVAL  Invalid parameters.
 * \retval -ENOMEM  Allocation failure.
 */
int ubi_device_get_peb_ec(struct ubi_device *ubi, size_t **peb_ec, size_t *len);

/**
 * \brief Force-release all partition guard locks (test API).
 *
 * Clears the internal active-partitions bitfield so that subsequent
 * ubi_device_init() calls will not return -EBUSY.  Must be called in
 * the test-case \c before callback to prevent cascading failures when
 * a prior assertion aborts before reaching ubi_device_deinit().
 */
void ubi_test_partition_force_release_all(void);

/**
 * \brief Get the erased byte value for a flash partition (test API).
 *
 * Thin wrapper around the internal ubi_get_erased_val() helper, exposed
 * for unit testing. See ubi_internal.h for details.
 *
 * \param[in] mtd          UBI MTD descriptor.
 * \param[out] erased_val  Erased byte value for the partition.
 *
 * \return 0 on success, or negative errno on failure.
 */
int ubi_test_get_erased_val(const struct ubi_mtd *mtd, uint8_t *erased_val);

/**
 * \brief Check whether a buffer is entirely erased (test API).
 *
 * \param[in] buf        Buffer to check.
 * \param len            Length of the buffer in bytes.
 * \param erased_val     Expected erased byte value.
 *
 * \retval true   Every byte in \p buf equals \p erased_val.
 * \retval false  At least one byte differs.
 */
bool ubi_test_buf_is_erased(const void *buf, size_t len, uint8_t erased_val);

#endif /* CONFIG_UBI_TEST_API_ENABLE */

/* Fault injection API ------------------------------------------------------------------------- */

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)

/**
 * \brief Reset all fault injection state.
 *
 * Must be called between test cases to ensure a clean starting state.
 */
void ubi_test_fault_reset(void);

/**
 * \brief Configure allocations to fail after \p n successful calls.
 *
 * Pass 0 to fail on the very next call. Pass -1 (or a very large value)
 * to disable injection. The counter is checked inside ubi_mem_*_alloc().
 */
void ubi_test_fault_set_alloc_fail_after(int n);

/**
 * \brief Configure flash writes to fail after \p n successful calls.
 *
 * Pass 0 to fail on the very next write. Pass -1 to disable injection.
 * The counter is checked inside flash_write_with_retry().
 */
void ubi_test_fault_set_flash_write_fail_after(int n);

/**
 * \brief Configure flash erases to fail after \p n successful calls.
 *
 * Pass 0 to fail on the very next erase. Pass -1 to disable injection.
 * The counter is checked inside flash I/O wrappers.
 */
void ubi_test_fault_set_flash_erase_fail_after(int n);

#else /* !CONFIG_UBI_TEST_FAULT_INJECTION */

static inline void ubi_test_fault_reset(void)
{
}

static inline void ubi_test_fault_set_alloc_fail_after(int n)
{
	(void)n;
}

static inline void ubi_test_fault_set_flash_write_fail_after(int n)
{
	(void)n;
}

static inline void ubi_test_fault_set_flash_erase_fail_after(int n)
{
	(void)n;
}

#endif /* CONFIG_UBI_TEST_FAULT_INJECTION */

#endif /* UBI_TEST_H */
