/**
 * \file    ubi_test_memory.h
 * \brief   Shared heap-tracking helpers for leak detection.
 *
 * \copyright Copyright (c) 2026
 */

#ifndef UBI_TEST_MEMORY_H
#define UBI_TEST_MEMORY_H

#include <zephyr/ztest.h>
#include <zephyr/sys/sys_heap.h>

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)

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

struct sys_memory_stats { size_t free_bytes; };

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

#endif /* CONFIG_SYS_HEAP_RUNTIME_STATS */

#endif /* UBI_TEST_MEMORY_H */
