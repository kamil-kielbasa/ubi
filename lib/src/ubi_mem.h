/**
 * \file    ubi_mem.h
 * \author  Kamil Kielbasa
 * \brief   UBI memory abstraction layer.
 *
 * All UBI runtime memory operations route through this module. Two backends
 * are available, selected at compile time via Kconfig:
 *
 * - **Static** (`CONFIG_UBI_MEM_BACKEND_STATIC`, default): uses `k_mem_slab`
 *   pools sized at compile time. Provides deterministic RAM budget and full
 *   isolation from the application heap.
 *
 * - **Heap** (`CONFIG_UBI_MEM_BACKEND_HEAP`): delegates to `k_malloc` /
 *   `k_free` (legacy behaviour).
 *
 * \copyright Copyright (c) 2025
 */

/* Include guard ------------------------------------------------------------------------------- */
#ifndef UBI_MEM_H
#define UBI_MEM_H

/* Include files ------------------------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>

/* Forward declarations ------------------------------------------------------------------------ */
struct ubi_device;
struct ubi_volume;
struct ubi_rbt_item;
struct ubi_list_item;

/* Module interface function declarations ------------------------------------------------------ */

/* ----- Device allocation ----- */

/**
 * \brief Allocate a zeroed `struct ubi_device`.
 *
 * \param[out] out  On success, points to the allocated device.
 *
 * \retval 0        Success.
 * \retval -ENOMEM  Out of memory (heap) or pool exhausted (static).
 */
int ubi_mem_device_alloc(struct ubi_device **out);

/**
 * \brief Free a previously allocated device struct.
 */
void ubi_mem_device_free(struct ubi_device *dev);

/* ----- Volume allocation ----- */

/**
 * \brief Allocate a zeroed `struct ubi_volume`.
 *
 * \param[out] out  On success, points to the allocated volume.
 *
 * \retval 0        Success.
 * \retval -ENOMEM  Out of memory.
 */
int ubi_mem_volume_alloc(struct ubi_volume **out);

/**
 * \brief Free a previously allocated volume struct.
 */
void ubi_mem_volume_free(struct ubi_volume *vol);

/* ----- Leaf item allocation (rbt_item / list_item — 16 B block) ----- */

/**
 * \brief Allocate a zeroed 16-byte leaf item block.
 *
 * The returned pointer may be cast to `struct ubi_rbt_item *` or
 * `struct ubi_list_item *` depending on context.
 *
 * \param[out] out  On success, points to the allocated block.
 *
 * \retval 0        Success.
 * \retval -ENOMEM  Out of memory.
 */
int ubi_mem_leaf_alloc(void **out);

/**
 * \brief Free a previously allocated leaf item block.
 */
void ubi_mem_leaf_free(void *ptr);

/* ----- Scratch buffer (variable-size, transient) ----- */

/**
 * \brief Acquire a scratch buffer of at least \p len bytes.
 *
 * Under the static backend this returns a pointer into a pre-allocated
 * buffer. Under the heap backend it delegates to `k_malloc`.
 * The caller must release the buffer via `ubi_mem_scratch_free`.
 *
 * \param[in]  len  Required buffer size in bytes.
 * \param[out] out  On success, points to the allocated buffer.
 *
 * \retval 0        Success.
 * \retval -ENOMEM  Requested size exceeds the scratch budget (static)
 *                  or heap exhausted (heap).
 */
int ubi_mem_scratch_alloc(size_t len, uint8_t **out);

/**
 * \brief Release a scratch buffer obtained from `ubi_mem_scratch_alloc`.
 */
void ubi_mem_scratch_free(uint8_t *ptr);

/* ----- Diagnostic allocation (test API only) ----- */

#if defined(CONFIG_UBI_TEST_API_ENABLE)

/**
 * \brief Allocate an arbitrary-sized buffer via the heap.
 *
 * Used only by diagnostic test APIs (`ubi_device_get_peb_ec`) where the
 * caller is responsible for freeing the result. Not backed by slab pools.
 *
 * \param[in]  size  Requested size in bytes.
 * \param[out] out   On success, points to the allocated buffer.
 *
 * \retval 0        Success.
 * \retval -ENOMEM  Out of memory.
 */
int ubi_mem_diag_alloc(size_t size, void **out);

/**
 * \brief Free a buffer obtained from `ubi_mem_diag_alloc`.
 */
void ubi_mem_diag_free(void *ptr);

#endif /* CONFIG_UBI_TEST_API_ENABLE */

#endif /* UBI_MEM_H */
