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

#if defined(CONFIG_UBI_MEM_BACKEND_HEAP) && defined(CONFIG_SYS_HEAP_RUNTIME_STATS)

extern struct sys_heap _system_heap;

static inline void ubi_test_memory_snapshot(struct sys_memory_stats *stats)
{
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, stats));
}

static inline void ubi_test_memory_check_no_leak(const struct sys_memory_stats *before,
						 const struct sys_memory_stats *after)
{
	zassert_equal(before->free_bytes, after->free_bytes,
		      "Heap leak: before=%zu after=%zu", before->free_bytes, after->free_bytes);
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

#endif /* UBI_TEST_MEMORY_H */
