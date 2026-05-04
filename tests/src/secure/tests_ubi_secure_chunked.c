/**
 * \file    tests_ubi_secure_chunked.c
 * \author  Kamil Kielbasa
 *
 * \brief   Tests for chunked secure LEB mode (§7.8, §8.3, §12.3, §15.3).
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>
#include "arrays.h"

#include "ubi_test_secure_fixture.h"

#include <psa/crypto.h>

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/sys_heap.h>

#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* Static variables ----------------------------------------------------------------------------- */

static struct ubi_flash_desc flash = { 0 };

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
extern struct sys_heap _system_heap;
#endif

static struct sys_memory_stats before_init = { 0 };
static struct sys_memory_stats after_init = { 0 };
static struct sys_memory_stats after_deinit = { 0 };

/* Static helpers ------------------------------------------------------------------------------- */

static void memory_check(struct sys_memory_stats *bi, struct sys_memory_stats *ai,
			 struct sys_memory_stats *ad)
{
	zassert_not_null(bi);
	zassert_not_null(ai);
	zassert_not_null(ad);

	zassert_equal(bi->free_bytes, ad->free_bytes);
	zassert_equal(bi->allocated_bytes, ad->allocated_bytes);

#if defined(CONFIG_UBI_MEM_BACKEND_HEAP)
	zassert_not_equal(ai->free_bytes, ad->free_bytes);
	zassert_not_equal(ai->allocated_bytes, ad->allocated_bytes);
#endif

	memset(bi, 0, sizeof(*bi));
	memset(ai, 0, sizeof(*ai));
	memset(ad, 0, sizeof(*ad));
}

/* Suite setup / teardown ----------------------------------------------------------------------- */

static void *ztest_suite_setup(void)
{
	const struct device *flash_dev = UBI_PARTITION_DEVICE;
	zassert_true(device_is_ready(flash_dev));

	struct flash_pages_info page_info = { 0 };
	zassert_ok(flash_get_page_info_by_offs(flash_dev, 0, &page_info));

	flash.partition_id = FIXED_PARTITION_ID(UBI_PARTITION_NAME);
	flash.erase_block_size = page_info.size;
	flash.write_block_size = flash_get_write_block_size(flash_dev);

	zassert_equal(psa_crypto_init(), PSA_SUCCESS);
	ubi_test_import_root_key();

	return NULL;
}

static void ztest_suite_before(void *ctx)
{
	ARG_UNUSED(ctx);
	ubi_test_partition_force_release_all();
	zassert_ok(flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE));
}

/* Tests ---------------------------------------------------------------------------------------- */

/**
 * \brief Chunked geometry produces valid leb_size.
 *
 * \details Verify that leb_size is smaller than single-tag mode (more per-chunk
 *          tag overhead) and that at least one chunk fits.
 *
 * \expected leb_size > 0 and leb_size < (erase_block_size - 208).
 */
ZTEST(ubi_secure_chunked, test_geometry_leb_size)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));

	/* Chunked leb_size must be positive. */
	zassert_true(info.leb_size > 0, "leb_size is zero with chunked geometry");

	/* Chunked leb_size should be <= single-tag leb_size because of extra tags. */
	const size_t single_tag_leb_size = flash.erase_block_size - 160 - 48;

	zassert_true(info.leb_size <= single_tag_leb_size, "Chunked leb_size %zu > single-tag %zu",
		     info.leb_size, single_tag_leb_size);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Single-chunk write and read.
 *
 * \details Write a payload smaller than chunk_size, read back and verify.
 *          This exercises the chunked path with exactly one chunk.
 *
 * \expected Data matches after read; leb_get_size returns correct size.
 */
ZTEST(ubi_secure_chunked, test_single_chunk_write_read)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'c', 'k', '0' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	/* 128 bytes < chunk_size=256 → 1 chunk. */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_128, ARRAY_SIZE(array_128)));

	uint8_t rdata[ARRAY_SIZE(array_128)] = { 0 };
	size_t rdata_size = 0;

	zassert_ok(ubi_leb_get_size(ubi, vol_id, 0, &rdata_size));
	zassert_equal(ARRAY_SIZE(array_128), rdata_size);

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, rdata_size));
	zassert_mem_equal(rdata, array_128, ARRAY_SIZE(array_128));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	memory_check(&before_init, &after_init, &after_deinit);
}

/**
 * \brief Multi-chunk write and read.
 *
 * \details Write 1024 bytes (4 chunks of 256), read back and verify the full
 *          payload matches.
 *
 * \expected All 1024 bytes match after read.
 */
ZTEST(ubi_secure_chunked, test_multi_chunk_write_read)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'c', 'k', '1' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	/* 1024 bytes / 256-byte chunks = 4 chunks. */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_1024, ARRAY_SIZE(array_1024)));

	uint8_t rdata[ARRAY_SIZE(array_1024)] = { 0 };
	size_t rdata_size = 0;

	zassert_ok(ubi_leb_get_size(ubi, vol_id, 0, &rdata_size));
	zassert_equal(ARRAY_SIZE(array_1024), rdata_size);

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, rdata_size));
	zassert_mem_equal(rdata, array_1024, ARRAY_SIZE(array_1024));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	memory_check(&before_init, &after_init, &after_deinit);
}

/**
 * \brief Partial read spanning a chunk boundary.
 *
 * \details Write 1024 bytes (4 chunks), then read a 64-byte slice starting
 *          at offset 240 — this spans the boundary between chunk 0 (bytes
 *          0–255) and chunk 1 (bytes 256–511). Only chunks 0 and 1 need
 *          authentication (§12.3).
 *
 * \expected The 64-byte slice at offset 240 matches the original data.
 */
ZTEST(ubi_secure_chunked, test_partial_read_cross_chunk)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'c', 'k', '2' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_1024, ARRAY_SIZE(array_1024)));

	/* Read 64 bytes starting at offset 240 — spans chunk 0/1 boundary. */
	uint8_t rdata[64] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 240, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, &array_1024[240], sizeof(rdata));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Partial read within a single chunk.
 *
 * \details Write 1024 bytes, then read 32 bytes from the middle of chunk 2
 *          (offset 560, inside bytes 512–767). Only chunk 2 needs auth.
 *
 * \expected The 32-byte slice matches.
 */
ZTEST(ubi_secure_chunked, test_partial_read_within_chunk)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'c', 'k', '3' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_1024, ARRAY_SIZE(array_1024)));

	/* Read 32 bytes from offset 560 — within chunk 2. */
	uint8_t rdata[32] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 560, rdata, sizeof(rdata)));
	zassert_mem_equal(rdata, &array_1024[560], sizeof(rdata));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Chunked write with non-aligned payload (last chunk partial).
 *
 * \details Write 271 bytes (chunk 0 = 256 bytes, chunk 1 = 15 bytes).
 *          Read back and verify the full payload.
 *
 * \expected All 271 bytes match.
 */
ZTEST(ubi_secure_chunked, test_partial_last_chunk)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'c', 'k', '4' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	/* 271 bytes: first chunk full (256), second chunk partial (15). */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_271, ARRAY_SIZE(array_271)));

	uint8_t rdata[ARRAY_SIZE(array_271)] = { 0 };
	size_t rdata_size = 0;

	zassert_ok(ubi_leb_get_size(ubi, vol_id, 0, &rdata_size));
	zassert_equal(ARRAY_SIZE(array_271), rdata_size);

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, rdata_size));
	zassert_mem_equal(rdata, array_271, ARRAY_SIZE(array_271));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Multi-chunk data persists across reboot.
 *
 * \details Write 1024 bytes, deinit, re-init and verify data is recovered
 *          from flash with chunked authentication.
 *
 * \expected Data matches after reboot; heap fully reclaimed.
 */
ZTEST(ubi_secure_chunked, test_multi_chunk_with_reboot)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'c', 'k', '5' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	/* 1. Init, create, write. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_1024, ARRAY_SIZE(array_1024)));

	/* 2. Verify before reboot. */
	uint8_t rdata[ARRAY_SIZE(array_1024)] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, ARRAY_SIZE(array_1024)));
	zassert_mem_equal(rdata, array_1024, ARRAY_SIZE(array_1024));

	/* 3. Deinit (simulated reboot). */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	memory_check(&before_init, &after_init, &after_deinit);

	/* 4. Re-init and verify persistence. */
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &before_init));
	ubi = NULL;
	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	memset(rdata, 0, sizeof(rdata));
	size_t rdata_size = 0;

	zassert_ok(ubi_leb_get_size(ubi, vol_id, 0, &rdata_size));
	zassert_equal(ARRAY_SIZE(array_1024), rdata_size);

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, rdata_size));
	zassert_mem_equal(rdata, array_1024, ARRAY_SIZE(array_1024));

	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_init));
	zassert_ok(ubi_device_deinit(ubi));
	zassert_ok(sys_heap_runtime_stats_get(&_system_heap, &after_deinit));
	memory_check(&before_init, &after_init, &after_deinit);
}

/**
 * \brief Overwrite a chunked LEB with different data.
 *
 * \details Write 512 bytes, then overwrite with 1024 bytes, verify the
 *          new data is returned. Old PEB becomes dirty.
 *
 * \expected Overwritten data matches; old data is gone.
 */
ZTEST(ubi_secure_chunked, test_overwrite_chunked)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'c', 'k', '6' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	/* First write: 512 bytes (2 full chunks). */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_512, ARRAY_SIZE(array_512)));

	/* Overwrite: 1024 bytes (4 full chunks). */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_1024, ARRAY_SIZE(array_1024)));

	uint8_t rdata[ARRAY_SIZE(array_1024)] = { 0 };
	size_t rdata_size = 0;

	zassert_ok(ubi_leb_get_size(ubi, vol_id, 0, &rdata_size));
	zassert_equal(ARRAY_SIZE(array_1024), rdata_size);

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, rdata, rdata_size));
	zassert_mem_equal(rdata, array_1024, ARRAY_SIZE(array_1024));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Tamper one chunk and verify authentication failure.
 *
 * \details Write 1024 bytes (4 chunks), then corrupt one byte in chunk 2's
 *          ciphertext on flash. A full read should fail with auth error.
 *          A partial read of only chunk 0 should succeed (§12.3).
 *
 * \expected Full read returns error; partial read of untampered chunk succeeds.
 */
ZTEST(ubi_secure_chunked, test_tamper_one_chunk)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'c', 'k', '7' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, array_1024, ARRAY_SIZE(array_1024)));

	/* Find the PEB for lnum=0. We need to tamper its flash content. */
	size_t rdata_size = 0;

	zassert_ok(ubi_leb_get_size(ubi, vol_id, 0, &rdata_size));
	zassert_equal(ARRAY_SIZE(array_1024), rdata_size);

	/* Tamper one byte in chunk 2's ciphertext region on flash.
	 * Data PEB layout: EC(64) + VID(96) + prefix32 + chunk0(256+16) + chunk1(256+16) + chunk2...
	 * Chunk 2 ciphertext starts at: 160 + 32 + 2*(256+16) = 160+32+544 = 736. */
	const struct flash_area *fa = NULL;

	zassert_ok(flash_area_open(flash.partition_id, &fa));

	/* Read the PEB index from the EBA (simplified: find the data region).
	 * We'll tamper at a fixed offset within the data PEB that holds lnum=0.
	 * To find it, we read volume info to know the mapping exists, then
	 * directly corrupt the flash at the known layout offset.
	 *
	 * For simplicity, scan reserved+data PEBs to find the data PEB for vol_id/lnum=0.
	 * Actually, we can just deinit and re-init to verify the tamper,
	 * but we need to know which PEB to tamper.
	 *
	 * Alternative: use the simulated flash interface directly.
	 * The test partitions start at UBI_PARTITION_OFFSET.
	 * Reserved PEBs are at PEB index 0 and 1.
	 * Data PEBs start at index 2+.
	 * The PEB for lnum=0 is allocated from the free pool — we can find it
	 * by scanning data PEBs for a valid VID with vol_id and lnum=0.
	 *
	 * For a simpler approach: tamper ALL data PEB regions at the chunk 2 offset.
	 * Since only one PEB carries this data, only that one will be affected. */

	const size_t nr_res_pebs = 2;
	const size_t nr_pebs = UBI_PARTITION_SIZE / flash.erase_block_size;
	const size_t chunk_size = CONFIG_UBI_CRYPTO_LEB_CHUNK_SIZE;
	const size_t tag_size = 16;
	/* Chunk 2 ct starts at: LEB_OFFSET(160) + prefix(32) + 2*(chunk_size+tag) */
	const size_t chunk2_ct_off = 160 + 32 + 2 * (chunk_size + tag_size);

	for (size_t peb = nr_res_pebs; peb < nr_pebs; peb++) {
		const size_t peb_off = peb * flash.erase_block_size;
		uint8_t byte_val = 0;

		(void)flash_area_read(fa, peb_off + chunk2_ct_off, &byte_val, 1);

		/* Flip one bit. */
		byte_val ^= 0x01;

		/* Flash simulator may not allow direct overwrite without erase.
		 * Use the DOUBLE_WRITES config which allows re-writing. */
		(void)flash_area_write(fa, peb_off + chunk2_ct_off, &byte_val, 1);
	}

	flash_area_close(fa);

	/* Full read touching chunk 2 should fail auth. */
	uint8_t rdata[ARRAY_SIZE(array_1024)] = { 0 };
	int ret = ubi_leb_read(ubi, vol_id, 0, 0, rdata, ARRAY_SIZE(array_1024));

	zassert_not_equal(ret, 0, "Full read should fail after chunk tamper");

	/* Partial read of only chunk 0 (bytes 0–127) should succeed because
	 * chunk 0 is untampered. */
	uint8_t partial[128] = { 0 };

	ret = ubi_leb_read(ubi, vol_id, 0, 0, partial, sizeof(partial));
	zassert_ok(ret, "Partial read of untampered chunk should succeed");
	zassert_mem_equal(partial, array_1024, sizeof(partial));

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Zero-length LEB map in chunked mode.
 *
 * \details Map a LEB (zero-length write), verify it is mapped and has
 *          size 0. This exercises the single-tag zero-length fallback
 *          path in chunked mode (§7.8).
 *
 * \expected leb_is_mapped returns true; leb_get_size returns 0.
 */
ZTEST(ubi_secure_chunked, test_zero_length_map)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	const struct ubi_volume_config vol_cfg = {
		.name = { '/', 'c', 'k', '8' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 4,
	};

	struct ubi_device *ubi = NULL;
	int vol_id = -1;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	/* Map LEB 1 (zero-length write). */
	zassert_ok(ubi_leb_map(ubi, vol_id, 1));

	bool is_mapped = false;

	zassert_ok(ubi_leb_is_mapped(ubi, vol_id, 1, &is_mapped));
	zassert_true(is_mapped);

	size_t size = 99;

	zassert_ok(ubi_leb_get_size(ubi, vol_id, 1, &size));
	zassert_equal(0, size);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Reject initialization when erase_block_size is too small for chunks.
 *
 * \details Create a flash descriptor with a tiny erase_block_size that cannot
 *          fit even one chunk. ubi_device_init() must fail — the flash
 *          driver rejects the invalid erase block size before the geometry
 *          check runs.
 *
 * \expected ubi_device_init returns a negative error code.
 */
ZTEST(ubi_secure_chunked, test_geometry_reject_tiny_erase_block)
{
	const struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	/* Create a flash descriptor with a very small erase block that cannot fit
	 * the secure headers + even one chunk. */
	struct ubi_flash_desc tiny_flash = flash;

	tiny_flash.erase_block_size = 256;

	struct ubi_device *ubi = NULL;
	const int ret = ubi_device_init(&tiny_flash, &cfg, &ubi);

	zassert_true(ret < 0, "Init must fail when erase block is too small for chunks (ret=%d)",
		     ret);
}

/* Suite declaration ---------------------------------------------------------------------------- */

ZTEST_SUITE(ubi_secure_chunked, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);
