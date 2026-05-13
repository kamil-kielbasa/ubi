/**
 * \file    ubi_mem.c
 * \author  Kamil Kielbasa
 * \brief   UBI memory abstraction — heap and static backends.
 *
 * \copyright Copyright (c) 2025
 */

/* Include files -------------------------------------------------------------------------------- */

/* Internal headers: */
#include "ubi_mem.h"
#include "ubi_internal.h"
#include "ubi_plain_io.h"
#include "ubi_cache.h"

/* Public headers: */
#include <ubi_test.h>

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>

/* Standard library headers: */
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

LOG_MODULE_DECLARE(ubi, CONFIG_UBI_LOG_LEVEL);

BUILD_ASSERT(sizeof(struct ubi_rbt_item) == 16, "ubi_rbt_item must be 16 bytes");
BUILD_ASSERT(sizeof(struct ubi_list_item) == 12, "ubi_list_item must be 12 bytes");
BUILD_ASSERT(sizeof(union ubi_leaf_item) == 16, "ubi_leaf_item must be 16 bytes");
#if defined(CONFIG_UBI_TEST_API_ENABLE) && (defined(CONFIG_X86) || defined(CONFIG_ARCH_POSIX))
/* On x86 / native_sim the test-only `bool test_write_shutdown` extends the
 * struct by 4 bytes after tail padding.  On ARM the same field fits inside
 * the existing tail padding, so it does not change `sizeof`. */
#define UBI_DEVICE_TEST_API_BYTES 4
#else
#define UBI_DEVICE_TEST_API_BYTES 0
#endif
#if defined(CONFIG_UBI_CRYPTO)
BUILD_ASSERT(sizeof(struct ubi_volume) == 64, "ubi_volume must be 64 bytes (secure)");
#if defined(CONFIG_X86) || defined(CONFIG_ARCH_POSIX)
BUILD_ASSERT(sizeof(struct ubi_device) == 224 + UBI_DEVICE_TEST_API_BYTES,
	     "ubi_device must be 224 bytes (secure, x86) or 228 bytes with TEST_API");
#elif defined(CONFIG_ARM)
/* ARM size varies with CONFIG_UBI_CRYPTO_MAX_KEY_VERSIONS, k_mutex layout
 * (Cortex-M vs Cortex-R, SMP, etc.) and tail padding, so use a generous
 * upper bound rather than an exact match.  The bound is here to catch
 * accidental growth, not to pin the exact byte count. */
BUILD_ASSERT(sizeof(struct ubi_device) <= 256, "ubi_device unexpectedly grew (secure, ARM)");
#endif
#else /* !CONFIG_UBI_CRYPTO */
BUILD_ASSERT(sizeof(struct ubi_volume) == 44, "ubi_volume must be 44 bytes");
#if defined(CONFIG_X86) || defined(CONFIG_ARCH_POSIX)
BUILD_ASSERT(sizeof(struct ubi_device) == 132 + UBI_DEVICE_TEST_API_BYTES,
	     "ubi_device must be 132 bytes (plain, x86) or 136 bytes with TEST_API");
#elif defined(CONFIG_ARM)
BUILD_ASSERT(sizeof(struct ubi_device) <= 144, "ubi_device unexpectedly grew (plain, ARM)");
#endif
#endif /* CONFIG_UBI_CRYPTO */

/* Fault injection support ---------------------------------------------------------------------- */

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)

static int alloc_remaining = -1;
static int alloc_remaining_kind[UBI_TEST_ALLOC_KIND_COUNT] = { -1, -1, -1, -1, -1 };

void ubi_test_fault_reset(void)
{
	alloc_remaining = -1;
	for (int i = 0; i < UBI_TEST_ALLOC_KIND_COUNT; ++i) {
		alloc_remaining_kind[i] = -1;
	}
	ubi_test_fault_set_flash_write_fail_after(-1);
	ubi_test_fault_set_flash_erase_fail_after(-1);
}

void ubi_test_fault_set_alloc_fail_after(int n)
{
	alloc_remaining = n;
}

void ubi_test_fault_set_kind_alloc_fail_after(enum ubi_test_alloc_kind kind, int n)
{
	if ((unsigned int)kind >= (unsigned int)UBI_TEST_ALLOC_KIND_COUNT) {
		return;
	}
	alloc_remaining_kind[kind] = n;
}

/**
 * \brief Check whether the next allocation of \p kind should be faulted.
 *
 * The per-kind counter is consulted first; if it is armed (>=0) it consumes
 * this allocation and either fires or decrements.  Only when the per-kind
 * counter is disabled (-1) does the shared `alloc_remaining` counter act.
 *
 * \retval true   The allocation should fail (return -ENOMEM).
 * \retval false  The allocation may proceed.
 */
static inline bool fault_should_fail(enum ubi_test_alloc_kind kind)
{
	if (alloc_remaining_kind[kind] == 0) {
		return true;
	}
	if (alloc_remaining_kind[kind] > 0) {
		alloc_remaining_kind[kind]--;
		return false;
	}
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
#define UBI_MEM_SCRATCH_BASE_SIZE \
	(UBI_DEV_HDR_SIZE + (CONFIG_UBI_MAX_NR_OF_VOLUMES * UBI_VOL_HDR_SIZE))

#if defined(CONFIG_UBI_CRYPTO_LEB_CHUNKED)
/* Chunked LEB read needs: ct(chunk_size + 16 tag) + pt(chunk_size). */
#define UBI_MEM_SCRATCH_CHUNKED_SIZE (2 * CONFIG_UBI_CRYPTO_LEB_CHUNK_SIZE + 16)
#define UBI_MEM_SCRATCH_SIZE                                          \
	((UBI_MEM_SCRATCH_BASE_SIZE > UBI_MEM_SCRATCH_CHUNKED_SIZE) ? \
		 UBI_MEM_SCRATCH_BASE_SIZE :                          \
		 UBI_MEM_SCRATCH_CHUNKED_SIZE)
#else /* !CONFIG_UBI_CRYPTO_LEB_CHUNKED */
#define UBI_MEM_SCRATCH_SIZE UBI_MEM_SCRATCH_BASE_SIZE
#endif /* CONFIG_UBI_CRYPTO_LEB_CHUNKED */
#define UBI_MEM_SLAB_ALIGN 4

K_MEM_SLAB_DEFINE_STATIC(device_slab, sizeof(struct ubi_device), UBI_MEM_DEVICE_POOL_COUNT,
			 UBI_MEM_SLAB_ALIGN);
K_MEM_SLAB_DEFINE_STATIC(volume_slab, sizeof(struct ubi_volume), UBI_MEM_VOLUME_POOL_COUNT,
			 UBI_MEM_SLAB_ALIGN);
K_MEM_SLAB_DEFINE_STATIC(leaf_slab, sizeof(union ubi_leaf_item), UBI_MEM_LEAF_POOL_COUNT,
			 UBI_MEM_SLAB_ALIGN);
K_MEM_SLAB_DEFINE_STATIC(scratch_slab, UBI_MEM_SCRATCH_SIZE, UBI_MEM_SCRATCH_POOL_COUNT,
			 UBI_MEM_SLAB_ALIGN);

/* Device --------------------------------------------------------------------------------------- */

int ubi_mem_device_alloc(struct ubi_device **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail(UBI_TEST_ALLOC_DEVICE)) {
		LOG_ERR("Device allocation fault injected");
		return -ENOMEM;
	}
#endif /* CONFIG_UBI_TEST_FAULT_INJECTION */

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

/* Volume --------------------------------------------------------------------------------------- */

int ubi_mem_volume_alloc(struct ubi_volume **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail(UBI_TEST_ALLOC_VOLUME)) {
		LOG_ERR("Volume allocation fault injected");
		return -ENOMEM;
	}
#endif /* CONFIG_UBI_TEST_FAULT_INJECTION */

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

/* Leaf (16 B) ---------------------------------------------------------------------------------- */

int ubi_mem_leaf_alloc(void **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail(UBI_TEST_ALLOC_LEAF)) {
		LOG_ERR("Leaf allocation fault injected");
		return -ENOMEM;
	}
#endif /* CONFIG_UBI_TEST_FAULT_INJECTION */

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

/* Scratch -------------------------------------------------------------------------------------- */

int ubi_mem_scratch_alloc(size_t len, uint8_t **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail(UBI_TEST_ALLOC_SCRATCH)) {
		LOG_ERR("Scratch allocation fault injected");
		return -ENOMEM;
	}
#endif /* CONFIG_UBI_TEST_FAULT_INJECTION */

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

/* Diagnostic (test API) ------------------------------------------------------------------------ */

#if defined(CONFIG_UBI_TEST_API_ENABLE)

int ubi_mem_diag_alloc(size_t size, void **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail(UBI_TEST_ALLOC_DIAG)) {
		LOG_ERR("Diagnostic allocation fault injected");
		return -ENOMEM;
	}
#endif /* CONFIG_UBI_TEST_FAULT_INJECTION */

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

void ubi_mem_force_reset_all_slabs(void)
{
	/* Re-initialize each slab's free-list. The buffers were declared by
	 * K_MEM_SLAB_DEFINE_STATIC as `_k_mem_slab_buf_<name>` in this same
	 * translation unit, so they are accessible by name here. */
	(void)k_mem_slab_init(&device_slab, _k_mem_slab_buf_device_slab,
			      WB_UP(sizeof(struct ubi_device)), UBI_MEM_DEVICE_POOL_COUNT);
	(void)k_mem_slab_init(&volume_slab, _k_mem_slab_buf_volume_slab,
			      WB_UP(sizeof(struct ubi_volume)), UBI_MEM_VOLUME_POOL_COUNT);
	(void)k_mem_slab_init(&leaf_slab, _k_mem_slab_buf_leaf_slab,
			      WB_UP(sizeof(union ubi_leaf_item)), UBI_MEM_LEAF_POOL_COUNT);
	(void)k_mem_slab_init(&scratch_slab, _k_mem_slab_buf_scratch_slab,
			      WB_UP(UBI_MEM_SCRATCH_SIZE), UBI_MEM_SCRATCH_POOL_COUNT);
}

#endif /* CONFIG_UBI_TEST_API_ENABLE */

#endif /* CONFIG_UBI_MEM_BACKEND_STATIC */

/* ========================================================================= */
/* Heap backend (k_malloc / k_free)                                          */
/* ========================================================================= */

#if defined(CONFIG_UBI_MEM_BACKEND_HEAP)

/* Device --------------------------------------------------------------------------------------- */

int ubi_mem_device_alloc(struct ubi_device **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail(UBI_TEST_ALLOC_DEVICE)) {
		LOG_ERR("Device allocation fault injected");
		return -ENOMEM;
	}
#endif /* CONFIG_UBI_TEST_FAULT_INJECTION */

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

/* Volume --------------------------------------------------------------------------------------- */

int ubi_mem_volume_alloc(struct ubi_volume **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail(UBI_TEST_ALLOC_VOLUME)) {
		LOG_ERR("Volume allocation fault injected");
		return -ENOMEM;
	}
#endif /* CONFIG_UBI_TEST_FAULT_INJECTION */

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

/* Leaf (16 B) ---------------------------------------------------------------------------------- */

int ubi_mem_leaf_alloc(void **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail(UBI_TEST_ALLOC_LEAF)) {
		LOG_ERR("Leaf allocation fault injected");
		return -ENOMEM;
	}
#endif /* CONFIG_UBI_TEST_FAULT_INJECTION */

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

/* Scratch -------------------------------------------------------------------------------------- */

int ubi_mem_scratch_alloc(size_t len, uint8_t **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail(UBI_TEST_ALLOC_SCRATCH)) {
		LOG_ERR("Scratch allocation fault injected");
		return -ENOMEM;
	}
#endif /* CONFIG_UBI_TEST_FAULT_INJECTION */

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

/* Diagnostic (test API) ------------------------------------------------------------------------ */

#if defined(CONFIG_UBI_TEST_API_ENABLE)

int ubi_mem_diag_alloc(size_t size, void **out)
{
	if (!out) {
		return -EINVAL;
	}

#if defined(CONFIG_UBI_TEST_FAULT_INJECTION)
	if (fault_should_fail(UBI_TEST_ALLOC_DIAG)) {
		LOG_ERR("Diagnostic allocation fault injected");
		return -ENOMEM;
	}
#endif /* CONFIG_UBI_TEST_FAULT_INJECTION */

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

void ubi_mem_force_reset_all_slabs(void)
{
	/* No-op on heap backend: leaked allocations remain leaked. The heap
	 * normally has enough headroom that this is not catastrophic across a
	 * single test run. */
}

#endif /* CONFIG_UBI_TEST_API_ENABLE */

#endif /* CONFIG_UBI_MEM_BACKEND_HEAP */
