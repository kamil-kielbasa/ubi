/**
 * \file    ubi_test_memory.h
 * \brief   Shared memory-tracking helpers for leak detection.
 *
 * Under the heap backend, tracks `sys_heap` runtime stats.
 * Under the static backend, the heap is not used for UBI allocations,
 * so memory checking is a no-op.
 *
 * \copyright Copyright (c) 2026
 */

#ifndef UBI_TEST_MEMORY_H
#define UBI_TEST_MEMORY_H

#include <zephyr/ztest.h>
#include <zephyr/sys/sys_heap.h>

#include <string.h>

#if defined(CONFIG_UBI_MEM_BACKEND_HEAP) && defined(CONFIG_SYS_HEAP_RUNTIME_STATS)

extern struct sys_heap _system_heap;

static inline void ubi_test_memory_snapshot(struct sys_memory_stats *stats)
{
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, stats));
}

static inline void ubi_test_memory_check_no_leak(const struct sys_memory_stats *before,
						 const struct sys_memory_stats *after)
{
	zassert_equal(before->free_bytes, after->free_bytes, "Heap leak: before=%zu after=%zu",
		      before->free_bytes, after->free_bytes);
}

#else

static inline void ubi_test_memory_snapshot(struct sys_memory_stats *stats)
{
	(void)stats;
}

static inline void ubi_test_memory_check_no_leak(const struct sys_memory_stats *before,
						 const struct sys_memory_stats *after)
{
	(void)before;
	(void)after;
}

#endif

/**
 * \brief   Verify a full init/deinit cycle leaves the heap unchanged.
 *
 * \details Compares the pre-init snapshot \p bi against the post-deinit
 *          snapshot \p ad, asserting bytewise equality.  Under the heap
 *          backend, additionally asserts that init actually allocated
 *          something (\p ai differs from \p ad).  All three snapshots
 *          are zeroed on exit so the same buffers can be reused for the
 *          next cycle.
 *
 * \param[in,out] bi  Pre-init snapshot.
 * \param[in,out] ai  Post-init snapshot.
 * \param[in,out] ad  Post-deinit snapshot.
 */
static inline void ubi_test_memory_check(struct sys_memory_stats *bi, struct sys_memory_stats *ai,
					 struct sys_memory_stats *ad)
{
	zassert_not_null(bi);
	zassert_not_null(ai);
	zassert_not_null(ad);

	zassert_equal(bi->free_bytes, ad->free_bytes);
	zassert_equal(bi->allocated_bytes, ad->allocated_bytes);

#if defined(CONFIG_UBI_MEM_BACKEND_HEAP)
	zassert_true(ai->free_bytes < ad->free_bytes,
		     "init must allocate from heap: ai_free=%zu ad_free=%zu", ai->free_bytes,
		     ad->free_bytes);
	zassert_true(ai->allocated_bytes > ad->allocated_bytes,
		     "deinit must release heap: ai_alloc=%zu ad_alloc=%zu", ai->allocated_bytes,
		     ad->allocated_bytes);
#endif

	memset(bi, 0, sizeof(*bi));
	memset(ai, 0, sizeof(*ai));
	memset(ad, 0, sizeof(*ad));
}

#endif /* UBI_TEST_MEMORY_H */
