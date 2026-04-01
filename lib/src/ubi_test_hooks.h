/**
 * \file    ubi_test_hooks.h
 * \brief   Fault injection hooks for testing transactional safety.
 *
 * When CONFIG_UBI_TEST_FAULT_INJECTION is enabled, ubi_test_malloc()
 * wraps k_malloc with a counter that returns NULL after N successful
 * allocations. Tests set the counter via ubi_test_fault_set_malloc_fail_after().
 *
 * \copyright Copyright (c) 2026
 */

#ifndef UBI_TEST_HOOKS_H
#define UBI_TEST_HOOKS_H

#include <zephyr/kernel.h>
#include <stddef.h>
#include <stdint.h>

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)

/**
 * \brief Reset all fault injection state.
 *
 * Must be called between test cases to ensure a clean starting state.
 */
void ubi_test_fault_reset(void);

/**
 * \brief Configure malloc to fail after \p n successful calls.
 *
 * Pass 0 to fail on the very next call. Pass -1 (or a very large value)
 * to disable injection.
 */
void ubi_test_fault_set_malloc_fail_after(int n);

/**
 * \brief Allocate memory, subject to fault injection.
 *
 * If the remaining success counter is > 0, decrements and delegates
 * to k_malloc. Otherwise returns NULL to simulate ENOMEM.
 */
void *ubi_test_malloc(size_t size);

#else /* !CONFIG_UBI_TEST_FAULT_INJECTION */

static inline void ubi_test_fault_reset(void)
{
}
static inline void ubi_test_fault_set_malloc_fail_after(int n)
{
	(void)n;
}
static inline void *ubi_test_malloc(size_t size)
{
	return k_malloc(size);
}

#endif /* CONFIG_UBI_TEST_FAULT_INJECTION */

#endif /* UBI_TEST_HOOKS_H */
