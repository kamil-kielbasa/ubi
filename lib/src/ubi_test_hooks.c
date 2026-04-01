/**
 * \file    ubi_test_hooks.c
 * \brief   Fault injection implementation for UBI test builds.
 *
 * \copyright Copyright (c) 2026
 */

#include "ubi_test_hooks.h"

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)

#include <zephyr/kernel.h>

static int malloc_remaining = -1;

void ubi_test_fault_reset(void)
{
	malloc_remaining = -1;
}

void ubi_test_fault_set_malloc_fail_after(int n)
{
	malloc_remaining = n;
}

void *ubi_test_malloc(size_t size)
{
	if (malloc_remaining == 0) {
		return NULL;
	}
	if (malloc_remaining > 0) {
		malloc_remaining--;
	}
	return k_malloc(size);
}

#endif /* CONFIG_UBI_TEST_FAULT_INJECTION */
