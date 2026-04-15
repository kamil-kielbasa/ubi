/**
 * \file    ubi_mem.c
 * \author  Kamil Kielbasa
 * \brief   UBI memory abstraction — heap and static backends.
 *
 * \copyright Copyright (c) 2025
 */

/* Include files ------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_mem.h"
#include "ubi_internal.h"
#include "ubi_io.h"
#include "ubi_cache.h"

/* Public headers: */
#include <ubi_test.h>

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>

/* Standard library headers: */
#include <string.h>

/* Module defines ------------------------------------------------------------------------------ */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

BUILD_ASSERT(sizeof(struct ubi_rbt_item) == 16, "ubi_rbt_item must be 16 bytes");
BUILD_ASSERT(sizeof(struct ubi_list_item) == 12, "ubi_list_item must be 12 bytes");
BUILD_ASSERT(sizeof(union ubi_leaf_item) == 16, "ubi_leaf_item must be 16 bytes");
#if defined(CONFIG_UBI_CRYPTO)
BUILD_ASSERT(sizeof(struct ubi_volume) == 48, "ubi_volume must be 48 bytes (secure)");
BUILD_ASSERT(sizeof(struct ubi_device) == 148, "ubi_device must be 148 bytes (secure)");
#else
BUILD_ASSERT(sizeof(struct ubi_volume) == 44, "ubi_volume must be 44 bytes");
BUILD_ASSERT(sizeof(struct ubi_device) == 136, "ubi_device must be 136 bytes");
#endif

/* Fault injection support --------------------------------------------------------------------- */

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)

static int alloc_remaining = -1;

void ubi_test_fault_reset(void)
{
	alloc_remaining = -1;
	ubi_test_fault_set_flash_write_fail_after(-1);
	ubi_test_fault_set_flash_erase_fail_after(-1);
}

void ubi_test_fault_set_alloc_fail_after(int n)
{
	alloc_remaining = n;
}

/**
 * \brief Check whether the next allocation should be faulted.
 *
 * \retval true   The allocation should fail (return -ENOMEM).
 * \retval false  The allocation may proceed.
 */
static inline bool fault_should_fail(void)
{
	if (alloc_remaining == 0) {
		return true;
	}
	if (alloc_remaining > 0) {
		alloc_remaining--;
	}
	return false;
}

#endif /* CONFIG_UBI_TEST_FAULT_INJECTION */

/* ========================================================================= */
/* Static backend (k_mem_slab)                                               */
/* ========================================================================= */

#if defined(CONFIG_UBI_MEM_BACKEND_STATIC)

#include <zephyr/sys/util.h>

/* Pool sizes derived from Kconfig. */
#define UBI_MEM_DEVICE_POOL_COUNT CONFIG_UBI_MAX_NR_OF_DEVICES
#define UBI_MEM_VOLUME_POOL_COUNT (CONFIG_UBI_MAX_NR_OF_DEVICES * CONFIG_UBI_MAX_NR_OF_VOLUMES)
#define UBI_MEM_LEAF_POOL_COUNT         \
	(CONFIG_UBI_MAX_NR_OF_DEVICES * \
	 (CONFIG_UBI_MAX_NR_OF_DATA_PEBS + CONFIG_UBI_MAX_NR_OF_VOLUMES))
#define UBI_MEM_SCRATCH_POOL_COUNT 1U
#define UBI_MEM_SCRATCH_SIZE (UBI_DEV_HDR_SIZE + (CONFIG_UBI_MAX_NR_OF_VOLUMES * UBI_VOL_HDR_SIZE))
#define UBI_MEM_SLAB_ALIGN 4

K_MEM_SLAB_DEFINE_STATIC(device_slab, sizeof(struct ubi_device), UBI_MEM_DEVICE_POOL_COUNT,
			 UBI_MEM_SLAB_ALIGN);
K_MEM_SLAB_DEFINE_STATIC(volume_slab, sizeof(struct ubi_volume), UBI_MEM_VOLUME_POOL_COUNT,
			 UBI_MEM_SLAB_ALIGN);
K_MEM_SLAB_DEFINE_STATIC(leaf_slab, sizeof(union ubi_leaf_item), UBI_MEM_LEAF_POOL_COUNT,
			 UBI_MEM_SLAB_ALIGN);
K_MEM_SLAB_DEFINE_STATIC(scratch_slab, UBI_MEM_SCRATCH_SIZE, UBI_MEM_SCRATCH_POOL_COUNT,
			 UBI_MEM_SLAB_ALIGN);

/* ----- Device ----- */

int ubi_mem_device_alloc(struct ubi_device **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail()) {
		LOG_ERR("Device allocation fault injected");
		return -ENOMEM;
	}
#endif

	void *block = NULL;
	int ret = k_mem_slab_alloc(&device_slab, &block, K_NO_WAIT);

	if (ret != 0) {
		LOG_ERR("Device slab allocation failed");
		return -ENOMEM;
	}

	memset(block, 0, sizeof(struct ubi_device));
	*out = (struct ubi_device *)block;
	return 0;
}

void ubi_mem_device_free(struct ubi_device *dev)
{
	if (dev) {
		k_mem_slab_free(&device_slab, dev);
	}
}

/* ----- Volume ----- */

int ubi_mem_volume_alloc(struct ubi_volume **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail()) {
		LOG_ERR("Volume allocation fault injected");
		return -ENOMEM;
	}
#endif

	void *block = NULL;
	int ret = k_mem_slab_alloc(&volume_slab, &block, K_NO_WAIT);

	if (ret != 0) {
		LOG_ERR("Volume slab allocation failed");
		return -ENOMEM;
	}

	memset(block, 0, sizeof(struct ubi_volume));
	*out = (struct ubi_volume *)block;
	return 0;
}

void ubi_mem_volume_free(struct ubi_volume *vol)
{
	if (vol) {
		k_mem_slab_free(&volume_slab, vol);
	}
}

/* ----- Leaf (16 B) ----- */

int ubi_mem_leaf_alloc(void **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail()) {
		LOG_ERR("Leaf allocation fault injected");
		return -ENOMEM;
	}
#endif

	void *block = NULL;
	int ret = k_mem_slab_alloc(&leaf_slab, &block, K_NO_WAIT);

	if (ret != 0) {
		LOG_ERR("Leaf slab allocation failed");
		return -ENOMEM;
	}

	memset(block, 0, sizeof(union ubi_leaf_item));
	*out = block;
	return 0;
}

void ubi_mem_leaf_free(void *ptr)
{
	if (ptr) {
		k_mem_slab_free(&leaf_slab, ptr);
	}
}

/* ----- Scratch ----- */

int ubi_mem_scratch_alloc(size_t len, uint8_t **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail()) {
		LOG_ERR("Scratch allocation fault injected");
		return -ENOMEM;
	}
#endif

	if (len > UBI_MEM_SCRATCH_SIZE) {
		LOG_ERR("Scratch request %zu exceeds budget %d", len, UBI_MEM_SCRATCH_SIZE);
		return -ENOMEM;
	}

	void *block = NULL;
	int ret = k_mem_slab_alloc(&scratch_slab, &block, K_NO_WAIT);

	if (ret != 0) {
		LOG_ERR("Scratch slab allocation failed");
		return -ENOMEM;
	}

	memset(block, 0, len);
	*out = (uint8_t *)block;
	return 0;
}

void ubi_mem_scratch_free(uint8_t *ptr)
{
	if (ptr) {
		k_mem_slab_free(&scratch_slab, ptr);
	}
}

/* ----- Diagnostic (test API) ----- */

#if defined(CONFIG_UBI_TEST_API_ENABLE)

int ubi_mem_diag_alloc(size_t size, void **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail()) {
		LOG_ERR("Diagnostic allocation fault injected");
		return -ENOMEM;
	}
#endif

	void *ptr = k_malloc(size);

	if (!ptr) {
		LOG_ERR("Diagnostic heap allocation failed (%zu bytes)", size);
		return -ENOMEM;
	}

	*out = ptr;
	return 0;
}

void ubi_mem_diag_free(void *ptr)
{
	if (ptr) {
		k_free(ptr);
	}
}

#endif /* CONFIG_UBI_TEST_API_ENABLE */

#endif /* CONFIG_UBI_MEM_BACKEND_STATIC */

/* ========================================================================= */
/* Heap backend (k_malloc / k_free)                                          */
/* ========================================================================= */

#if defined(CONFIG_UBI_MEM_BACKEND_HEAP)

/* ----- Device ----- */

int ubi_mem_device_alloc(struct ubi_device **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail()) {
		LOG_ERR("Device allocation fault injected");
		return -ENOMEM;
	}
#endif

	struct ubi_device *dev = k_malloc(sizeof(*dev));

	if (!dev) {
		LOG_ERR("Device heap allocation failed");
		return -ENOMEM;
	}

	memset(dev, 0, sizeof(*dev));
	*out = dev;
	return 0;
}

void ubi_mem_device_free(struct ubi_device *dev)
{
	if (dev) {
		k_free(dev);
	}
}

/* ----- Volume ----- */

int ubi_mem_volume_alloc(struct ubi_volume **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail()) {
		LOG_ERR("Volume allocation fault injected");
		return -ENOMEM;
	}
#endif

	struct ubi_volume *vol = k_malloc(sizeof(*vol));

	if (!vol) {
		LOG_ERR("Volume heap allocation failed");
		return -ENOMEM;
	}

	memset(vol, 0, sizeof(*vol));
	*out = vol;
	return 0;
}

void ubi_mem_volume_free(struct ubi_volume *vol)
{
	if (vol) {
		k_free(vol);
	}
}

/* ----- Leaf (16 B) ----- */

int ubi_mem_leaf_alloc(void **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail()) {
		LOG_ERR("Leaf allocation fault injected");
		return -ENOMEM;
	}
#endif

	void *ptr = k_malloc(sizeof(union ubi_leaf_item));

	if (!ptr) {
		LOG_ERR("Leaf heap allocation failed");
		return -ENOMEM;
	}

	memset(ptr, 0, sizeof(union ubi_leaf_item));
	*out = ptr;
	return 0;
}

void ubi_mem_leaf_free(void *ptr)
{
	if (ptr) {
		k_free(ptr);
	}
}

/* ----- Scratch ----- */

int ubi_mem_scratch_alloc(size_t len, uint8_t **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail()) {
		LOG_ERR("Scratch allocation fault injected");
		return -ENOMEM;
	}
#endif

	uint8_t *ptr = k_malloc(len);

	if (!ptr) {
		LOG_ERR("Scratch heap allocation failed (%zu bytes)", len);
		return -ENOMEM;
	}

	memset(ptr, 0, len);
	*out = ptr;
	return 0;
}

void ubi_mem_scratch_free(uint8_t *ptr)
{
	if (ptr) {
		k_free(ptr);
	}
}

/* ----- Diagnostic (test API) ----- */

#if defined(CONFIG_UBI_TEST_API_ENABLE)

int ubi_mem_diag_alloc(size_t size, void **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail()) {
		LOG_ERR("Diagnostic allocation fault injected");
		return -ENOMEM;
	}
#endif

	void *ptr = k_malloc(size);

	if (!ptr) {
		LOG_ERR("Diagnostic heap allocation failed (%zu bytes)", size);
		return -ENOMEM;
	}

	*out = ptr;
	return 0;
}

void ubi_mem_diag_free(void *ptr)
{
	if (ptr) {
		k_free(ptr);
	}
}

#endif /* CONFIG_UBI_TEST_API_ENABLE */

#endif /* CONFIG_UBI_MEM_BACKEND_HEAP */
