/**
 * \file    tests_ubi_secure_anchor.c
 * \author  Kamil Kielbasa
 *
 * \brief   Secure-specific tests: hidden anchor PEB plus the per-volume
 *          AEAD counter floor cache.
 *
 * \details Closes the AEAD nonce-reuse vulnerability where the previous
 *          implementation only consulted the currently-mapped PEB for a
 *          given lnum when recovering counter state for the next write.
 *          After write, unmap, erase-all-dirty and rewrite, the new
 *          mapping would restart leb_write_counter at 0 under the same
 *          HKDF child key, violating AEAD nonce uniqueness for that
 *          {key_version, volume_id} pair.  These tests pin the new
 *          contract: the per-volume RAM cache, mirrored on flash by the
 *          hidden anchor, is the strict upper bound across all on-flash
 *          evidence and is the floor for every subsequent write.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>

#include "ubi_test_fixture.h"
#include "ubi_test_secure_fixture.h"

#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)
#include "ubi_secure_test_hooks.h"
#include "ubi_secure_types.h"
#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */

#include <psa/crypto.h>

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>

#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* Module types and type definitiones ----------------------------------------------------------- */

/* Module interface variables and constants ----------------------------------------------------- */

/* Static variables and constants --------------------------------------------------------------- */

static struct ubi_flash_desc flash = { 0 };

/* Static function declarations ----------------------------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_before(void *ctx);

#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)
static void drain_dirty_pebs(struct ubi_device *ubi);
#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */

/* Static function definitions ------------------------------------------------------------------ */

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
	(void)ctx;
	ubi_test_partition_force_release_all();
	zassert_ok(flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE));
#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)
	ubi_secure_test_set_leb_write_counter_floor(0);
#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */
}

#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)
static void drain_dirty_pebs(struct ubi_device *ubi)
{
	struct ubi_device_info info = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info));
	while (info.dirty_peb_count > 0) {
		zassert_ok(ubi_device_erase_peb(ubi));
		memset(&info, 0, sizeof(info));
		zassert_ok(ubi_device_get_info(ubi, &info));
	}
}
#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */

/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_secure_anchor, NULL, ztest_suite_setup, ztest_suite_before, NULL, NULL);

#if defined(CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION)

/**
 * \brief Single LEB write, unmap and erase preserves and strictly advances
 *        the cached counter through the sole-witness anchor rewrite.
 *
 * \scenario Init, create a dynamic volume, write one LEB so the per-volume
 *           cache is strictly above the anchor's initial floor of 1, unmap
 *           that LEB and drain the resulting dirty PEB.  Because the dirty
 *           PEB is the sole on-flash witness of the cached counter,
 *           erasing it triggers a hidden-anchor rewrite which bumps the
 *           cache by one before the erase completes.  Finally write the
 *           LEB again to confirm the new write inherits the bumped floor.
 *
 * \expect cache_after_write > 1 (write advances above anchor seed);
 *           cache_after_drain > cache_after_write (anchor rewrite bumps);
 *           cache_after_second_write > cache_after_drain (next write
 *           strictly advances).
 */
ZTEST(ubi_secure_anchor, test_single_leb_unmap_erase_write_inherits_counter)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	const struct ubi_volume_config vol_cfg = {
		.name = { 'a', 'n', 'c', 'h', '1' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	struct ubi_device *ubi = NULL;
	int vol_id = -1;
	const uint8_t data[] = { 0xA5, 0x5A };

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint64_t cached_after_write = 0;

	zassert_ok(
		ubi_secure_test_get_volume_cached_counter(ubi, vol_id, &cached_after_write, NULL));
	zassert_true(cached_after_write > 1, "cache must advance after first write, got %llu",
		     (unsigned long long)cached_after_write);

	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
	drain_dirty_pebs(ubi);

	uint64_t cached_after_drain = 0;

	zassert_ok(
		ubi_secure_test_get_volume_cached_counter(ubi, vol_id, &cached_after_drain, NULL));
	zassert_true(cached_after_drain > cached_after_write,
		     "sole-witness erase must bump cache via anchor rewrite: "
		     "before=%llu after=%llu",
		     (unsigned long long)cached_after_write,
		     (unsigned long long)cached_after_drain);

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint64_t cached_after_second_write = 0;

	zassert_ok(ubi_secure_test_get_volume_cached_counter(ubi, vol_id,
							     &cached_after_second_write, NULL));
	zassert_true(cached_after_second_write > cached_after_drain,
		     "second write must strictly advance cache: prev=%llu new=%llu",
		     (unsigned long long)cached_after_drain,
		     (unsigned long long)cached_after_second_write);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Multi-LEB churn followed by unmap, erase and a fresh write
 *        strictly exceeds the peak counter observed during the churn.
 *
 * \scenario Create a four-LEB volume, write to each LEB to peak the
 *           cached counter, then unmap every LEB and drain the entire
 *           dirty pool.  Write LEB 0 again.
 *
 * \expect cache_after_post_churn_write > cache_peak observed across the
 *           four initial writes.
 */
ZTEST(ubi_secure_anchor, test_multi_leb_churn_cache_strict_monotonic)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	const struct ubi_volume_config vol_cfg = {
		.name = { 'a', 'n', 'c', 'h', '2' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 4,
	};
	struct ubi_device *ubi = NULL;
	int vol_id = -1;
	const uint8_t data[] = { 0xDE, 0xAD, 0xBE, 0xEF };

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	for (size_t lnum = 0; lnum < vol_cfg.leb_count; lnum++) {
		zassert_ok(ubi_leb_write(ubi, vol_id, lnum, data, sizeof(data)));
	}

	uint64_t cached_peak = 0;

	zassert_ok(ubi_secure_test_get_volume_cached_counter(ubi, vol_id, &cached_peak, NULL));

	for (size_t lnum = 0; lnum < vol_cfg.leb_count; lnum++) {
		zassert_ok(ubi_leb_unmap(ubi, vol_id, lnum));
	}
	drain_dirty_pebs(ubi);

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint64_t cached_after = 0;

	zassert_ok(ubi_secure_test_get_volume_cached_counter(ubi, vol_id, &cached_after, NULL));
	zassert_true(cached_after > cached_peak,
		     "post-churn write must exceed prior peak: peak=%llu after=%llu",
		     (unsigned long long)cached_peak, (unsigned long long)cached_after);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Cold attach reseeds the cache from the hidden anchor and the
 *        post-attach value strictly exceeds the pre-drain snapshot.
 *
 * \scenario Create a volume, write one LEB and unmap it.  Sample the
 *           cache BEFORE draining the dirty pool.  Drain the dirty pool,
 *           which triggers the sole-witness anchor rewrite, then verify
 *           dirty_peb_count is zero.  Deinit, then init again so the
 *           only surviving carrier of the volume's counter floor is the
 *           anchor on flash.
 *
 * \expect dirty_peb_count == 0 after the drain;
 *           cache_post_attach > cache_pre_drain (anchor rewrite that
 *           happened during the drain is observed through cold attach).
 */
ZTEST(ubi_secure_anchor, test_cold_attach_reseeds_cache_from_anchor)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	const struct ubi_volume_config vol_cfg = {
		.name = { 'a', 'n', 'c', 'h', '3' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	struct ubi_device *ubi = NULL;
	int vol_id = -1;
	const uint8_t data[] = { 0x11, 0x22, 0x33, 0x44 };

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));

	/* Sample the cache BEFORE draining so the post-attach assertion can
	 * prove that the anchor rewrite performed during the drain is the
	 * source of the strict increase observed across cold attach. */
	uint64_t cached_pre_drain = 0;

	zassert_ok(ubi_secure_test_get_volume_cached_counter(ubi, vol_id, &cached_pre_drain, NULL));

	drain_dirty_pebs(ubi);

	struct ubi_device_info info_after_drain = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info_after_drain));
	zassert_equal(0, info_after_drain.dirty_peb_count,
		      "drain must leave no dirty PEBs, got %zu", info_after_drain.dirty_peb_count);

	zassert_ok(ubi_device_deinit(ubi));
	ubi = NULL;

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));

	uint64_t cached_post_attach = 0;

	zassert_ok(
		ubi_secure_test_get_volume_cached_counter(ubi, vol_id, &cached_post_attach, NULL));
	zassert_true(cached_post_attach > cached_pre_drain,
		     "cold attach must observe the anchor rewrite: pre_drain=%llu post=%llu",
		     (unsigned long long)cached_pre_drain, (unsigned long long)cached_post_attach);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Erasing a non-witness dirty PEB leaves the cache unchanged and
 *        does not consume an extra free PEB for an anchor rewrite.
 *
 * \scenario Create a volume, write LEB 0 and LEB 1 (LEB 1 receives the
 *           higher counter because it is written last).  Overwrite
 *           LEB 0; the new mapping carries an even higher counter and
 *           the previous LEB 0 PEB enters the dirty pool with a strictly
 *           lower counter than the cache.  Erase exactly one dirty PEB.
 *
 * \expect The cache is unchanged across the erase;
 *           free_peb_count grows by exactly one (the erased PEB returns
 *           to the free pool with no extra PEB consumed by an anchor
 *           rewrite).
 */
ZTEST(ubi_secure_anchor, test_non_witness_erase_does_not_rewrite_anchor)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	const struct ubi_volume_config vol_cfg = {
		.name = { 'a', 'n', 'c', 'h', '4' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	struct ubi_device *ubi = NULL;
	int vol_id = -1;
	const uint8_t data[] = { 0xAB };

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 1, data, sizeof(data)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint64_t cached_before_erase = 0;

	zassert_ok(
		ubi_secure_test_get_volume_cached_counter(ubi, vol_id, &cached_before_erase, NULL));

	struct ubi_device_info info_before = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info_before));
	zassert_true(info_before.dirty_peb_count >= 1);

	const size_t free_before = info_before.free_peb_count;

	zassert_ok(ubi_device_erase_peb(ubi));

	struct ubi_device_info info_after = { 0 };

	zassert_ok(ubi_device_get_info(ubi, &info_after));

	uint64_t cached_after_erase = 0;

	zassert_ok(
		ubi_secure_test_get_volume_cached_counter(ubi, vol_id, &cached_after_erase, NULL));
	zassert_equal(cached_before_erase, cached_after_erase,
		      "non-witness erase must not change cache");
	zassert_equal(free_before + 1, info_after.free_peb_count,
		      "non-witness erase must not consume an extra free PEB for anchor rewrite");

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Counter saturation rejects the next write with -EOVERFLOW.
 *
 * \scenario Create a volume and arm the per-LEB write-counter floor
 *           test hook at UBI_SECURE_COUNTER_MAX so the next mutation
 *           would exceed the 48-bit counter space.  Attempt a single
 *           LEB write.
 *
 * \expect ubi_leb_write() returns -EOVERFLOW.
 */
ZTEST(ubi_secure_anchor, test_counter_saturation_rejects_with_overflow)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	const struct ubi_volume_config vol_cfg = {
		.name = { 'a', 'n', 'c', 'h', '5' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	struct ubi_device *ubi = NULL;
	int vol_id = -1;
	const uint8_t data[] = { 0xEE };

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	ubi_secure_test_set_leb_write_counter_floor(UBI_SECURE_COUNTER_MAX);

	const int ret = ubi_leb_write(ubi, vol_id, 0, data, sizeof(data));

	zassert_equal(-EOVERFLOW, ret, "expected -EOVERFLOW on saturated counter, got %d", ret);

	ubi_secure_test_set_leb_write_counter_floor(0);

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Repeated write, unmap and erase loop preserves strict
 *        monotonicity of the cached counter across every iteration.
 *
 * \scenario Create a volume and iterate sixteen times: write LEB 0,
 *           unmap LEB 0, drain the dirty pool.  After each iteration
 *           sample the cache.
 *
 * \expect The cache value strictly increases between every consecutive
 *           pair of iterations.
 */
ZTEST(ubi_secure_anchor, test_write_unmap_erase_loop_strict_monotonic)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	const struct ubi_volume_config vol_cfg = {
		.name = { 'a', 'n', 'c', 'h', '6' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	struct ubi_device *ubi = NULL;
	int vol_id = -1;
	const uint8_t data[] = { 0x77 };

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));

	uint64_t prev_cached = 0;

	zassert_ok(ubi_secure_test_get_volume_cached_counter(ubi, vol_id, &prev_cached, NULL));

	for (size_t i = 0; i < 16; i++) {
		zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));
		zassert_ok(ubi_leb_unmap(ubi, vol_id, 0));
		drain_dirty_pebs(ubi);

		uint64_t cur_cached = 0;

		zassert_ok(
			ubi_secure_test_get_volume_cached_counter(ubi, vol_id, &cur_cached, NULL));
		zassert_true(cur_cached > prev_cached,
			     "iteration %zu: cache must strictly advance: prev=%llu cur=%llu", i,
			     (unsigned long long)prev_cached, (unsigned long long)cur_cached);
		prev_cached = cur_cached;
	}

	zassert_ok(ubi_device_deinit(ubi));
}

/**
 * \brief Authenticated VID-meta read hook agrees with the RAM cache after
 *        a single LEB write, and the same hook also surfaces the hidden
 *        anchor PEB through the SIZE_MAX lnum sentinel.
 *
 * \scenario Create a dynamic volume, write one LEB, then for that LEB
 *           and for the anchor PEB (resolved via
 *           \ref ubi_secure_test_get_peb_for_lnum with
 *           \c SIZE_MAX) read the authenticated VID secure metadata
 *           via \ref ubi_secure_test_read_vid_meta_from_peb.
 *
 * \expect Resolution and authenticated reads succeed; the LEB's
 *           authenticated leb_write_counter equals the cached value;
 *           the anchor's authenticated leb_write_counter is non-zero
 *           and not greater than the cache (the cache is the strict
 *           upper bound).
 */
ZTEST(ubi_secure_anchor, test_read_vid_meta_hook_matches_cache_and_anchor)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();
	const struct ubi_volume_config vol_cfg = {
		.name = { 'p', 'r', 'a', 'h' },
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	struct ubi_device *ubi = NULL;
	int vol_id = -1;
	const uint8_t data[] = { 0xA5, 0x5A };

	zassert_ok(ubi_device_init(&flash, &cfg, &ubi));
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, &vol_id));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint64_t cached_wc = 0;
	uint64_t cached_tab = 0;

	zassert_ok(ubi_secure_test_get_volume_cached_counter(ubi, vol_id, &cached_wc, &cached_tab));

	size_t leb_pnum = SIZE_MAX;

	zassert_ok(ubi_secure_test_get_peb_for_lnum(ubi, vol_id, 0, &leb_pnum));
	zassert_not_equal(SIZE_MAX, leb_pnum);

	uint64_t leb_wc = 0;
	uint64_t leb_tab = 0;
	uint64_t leb_sqnum = 0;

	zassert_ok(ubi_secure_test_read_vid_meta_from_peb(ubi, leb_pnum, &leb_wc, &leb_tab,
							  &leb_sqnum));
	zassert_equal(cached_wc, leb_wc,
		      "LEB authenticated wc must match cache: cache=%llu peb=%llu",
		      (unsigned long long)cached_wc, (unsigned long long)leb_wc);
	zassert_equal(cached_tab, leb_tab);
	zassert_true(leb_sqnum > 0);

	size_t anchor_pnum = 0;

	zassert_ok(ubi_secure_test_get_peb_for_lnum(ubi, vol_id, SIZE_MAX, &anchor_pnum));
	zassert_not_equal(anchor_pnum, leb_pnum);

	uint64_t anchor_wc = 0;
	uint64_t anchor_tab = 0;

	zassert_ok(ubi_secure_test_read_vid_meta_from_peb(ubi, anchor_pnum, &anchor_wc, &anchor_tab,
							  NULL));
	zassert_true(anchor_wc > 0);
	zassert_true(anchor_wc <= cached_wc,
		     "cache is the strict upper bound: cache=%llu anchor=%llu",
		     (unsigned long long)cached_wc, (unsigned long long)anchor_wc);
	zassert_true(anchor_tab <= cached_tab);

	/* Error paths: unknown vol_id and unmapped lnum both report -ENOENT. */
	size_t scratch = 0;

	zassert_equal(-ENOENT, ubi_secure_test_get_peb_for_lnum(ubi, 999, 0, &scratch));
	zassert_equal(-ENOENT, ubi_secure_test_get_peb_for_lnum(ubi, vol_id, 1, &scratch));

	zassert_ok(ubi_device_deinit(ubi));
}

#endif /* CONFIG_UBI_CRYPTO_TEST_FAULT_INJECTION */
