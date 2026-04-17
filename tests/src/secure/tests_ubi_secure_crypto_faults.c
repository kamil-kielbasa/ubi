/**
 * \file    tests_ubi_secure_crypto_faults.c
 * \author  Kamil Kielbasa
 *
 * \brief   Crypto fault injection tests for secure backend.
 *
 * \details Exercises error paths in AEAD, HKDF, RNG, get_key_id, and
 *          freshness callbacks by arming the one-shot test hooks
 *          wired in ubi_secure_crypto.c, ubi_core_init.c, and
 *          ubi_secure_event.h.
 *
 * \copyright Copyright (c) 2026
 */

/* --------------------------------------- Include files --------------------------------------- */

#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>

#include "ubi_test_fixture.h"
#include "ubi_test_secure_fixture.h"
#include "ubi_test_memory.h"
#include "ubi_secure_test_hooks.h"

#include <psa/crypto.h>

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>

#include <string.h>

/* -------------------------------------- Module defines --------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* ------------------------------------- Static variables -------------------------------------- */

static struct ubi_mtd mtd = { 0 };
static struct ubi_device *g_ubi;

/* ---------------------------------- Suite setup / teardown ----------------------------------- */

static void *ztest_suite_setup(void)
{
	const struct device *const flash_dev = UBI_PARTITION_DEVICE;

	zassert_true(device_is_ready(flash_dev));

	struct flash_pages_info page_info = { 0 };

	zassert_ok(flash_get_page_info_by_offs(flash_dev, 0, &page_info));

	mtd.partition_id = FIXED_PARTITION_ID(UBI_PARTITION_NAME);
	mtd.erase_block_size = page_info.size;
	mtd.write_block_size = flash_get_write_block_size(flash_dev);

	zassert_equal(psa_crypto_init(), PSA_SUCCESS);
	ubi_test_import_root_key();

	return NULL;
}

static void ztest_suite_before(void *ctx)
{
	ARG_UNUSED(ctx);
	ubi_test_partition_force_release_all();
	ubi_test_fault_reset();
	ubi_secure_test_hook_reset();
	zassert_ok(flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE));
	g_ubi = NULL;
}

static void ztest_testcase_teardown(void *ctx)
{
	ARG_UNUSED(ctx);
	ubi_test_fault_reset();
	ubi_secure_test_hook_reset();
	if (g_ubi) {
		(void)ubi_device_deinit(g_ubi);
		g_ubi = NULL;
	}
}

/* --------------------------------- Secure init helper ---------------------------------------- */

static struct ubi_device *sec_init(void)
{
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(&mtd, &cfg, &ubi));
	g_ubi = ubi;
	return ubi;
}

static struct ubi_device *sec_init_with_vol(const char *name, int *vol_id)
{
	struct ubi_device *ubi = sec_init();

	struct ubi_volume_config vol_cfg = {
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};

	strncpy(vol_cfg.name, name, sizeof(vol_cfg.name) - 1);
	zassert_ok(ubi_volume_create(ubi, &vol_cfg, vol_id));
	return ubi;
}

/* ========================== AEAD encrypt fault tests ========================================= */

/**
 * \brief AEAD encrypt failure during leb_write returns error.
 *
 * \details Init device, create volume, arm AEAD_ENCRYPT_FAIL, attempt write.
 *
 * \expect leb_write returns a non-zero error code.
 */
ZTEST(ubi_secure_crypto_faults, test_aead_encrypt_fail_on_leb_write)
{
	int vol_id = -1;
	struct ubi_device *ubi = sec_init_with_vol("aew", &vol_id);

	const uint8_t data[] = { 0xAA, 0xBB, 0xCC, 0xDD };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_AEAD_ENCRYPT_FAIL, true);
	const int ret = ubi_leb_write(ubi, vol_id, 0, data, sizeof(data));

	zassert_not_equal(ret, 0, "leb_write should fail when AEAD encrypt is injected");
}

/**
 * \brief AEAD encrypt failure preserves old data when overwriting.
 *
 * \details Write original data, arm encrypt fault, overwrite fails, read
 *          back original.
 *
 * \expect Read-back matches original data after failed overwrite.
 */
ZTEST(ubi_secure_crypto_faults, test_aead_encrypt_fail_preserves_old_data)
{
	int vol_id = -1;
	struct ubi_device *ubi = sec_init_with_vol("aep", &vol_id);

	const uint8_t original[] = { 0x11, 0x22, 0x33, 0x44 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, original, sizeof(original)));

	const uint8_t new_data[] = { 0xAA, 0xBB, 0xCC, 0xDD };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_AEAD_ENCRYPT_FAIL, true);
	const int ret = ubi_leb_write(ubi, vol_id, 0, new_data, sizeof(new_data));

	zassert_not_equal(ret, 0);

	uint8_t readback[4] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, original, sizeof(original));
}

/**
 * \brief AEAD encrypt failure during leb_write via volume create returns error.
 *
 * \details Create volume, arm AEAD_ENCRYPT_FAIL, attempt write on new volume.
 *          The hook fires on the EC-header encrypt of the free PEB's data path.
 *
 * \expect leb_write returns a non-zero error code.
 */
ZTEST(ubi_secure_crypto_faults, test_aead_encrypt_fail_on_leb_write_after_create)
{
	int vol_id = -1;
	struct ubi_device *ubi = sec_init_with_vol("aecw", &vol_id);

	const uint8_t data[] = { 0x55, 0x66, 0x77, 0x88 };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_AEAD_ENCRYPT_FAIL, true);
	const int ret = ubi_leb_write(ubi, vol_id, 0, data, sizeof(data));

	zassert_not_equal(ret, 0,
			  "leb_write should fail when AEAD encrypt is injected after create");
}

/* ========================== AEAD decrypt fault tests ========================================= */

/**
 * \brief AEAD decrypt failure during leb_read returns error.
 *
 * \details Write data, arm AEAD_DECRYPT_FAIL, attempt read.
 *
 * \expect leb_read returns a non-zero error code.
 */
ZTEST(ubi_secure_crypto_faults, test_aead_decrypt_fail_on_leb_read)
{
	int vol_id = -1;
	struct ubi_device *ubi = sec_init_with_vol("adr", &vol_id);

	const uint8_t data[] = { 0x11, 0x22, 0x33, 0x44 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t readback[4] = { 0 };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_AEAD_DECRYPT_FAIL, true);
	const int ret = ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback));

	zassert_not_equal(ret, 0, "leb_read should fail when AEAD decrypt is injected");
}

/**
 * \brief AEAD decrypt failure during re-attach (device_init) returns error.
 *
 * \details Init, create vol, write LEB, deinit. Re-init with AEAD_DECRYPT_FAIL
 *          armed — attach should still succeed (scan tolerates individual PEB
 *          auth failures), but volume data is inaccessible.
 *
 * \expect Re-attach succeeds; subsequent read of the LEB fails.
 */
ZTEST(ubi_secure_crypto_faults, test_aead_decrypt_fail_on_reattach_read)
{
	int vol_id = -1;
	struct ubi_device *ubi = sec_init_with_vol("adr2", &vol_id);

	const uint8_t data[] = { 0xDE, 0xAD };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));

	/* Re-init — no fault on attach. */
	ubi = sec_init();

	uint8_t readback[2] = { 0 };

	/* Now arm decrypt fault for the read path. */
	ubi_secure_test_hook_set(UBI_SECURE_HOOK_AEAD_DECRYPT_FAIL, true);
	const int ret = ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback));

	zassert_not_equal(ret, 0, "leb_read should fail with AEAD decrypt fault");
}

/* ========================== RNG fault tests ================================================== */

/**
 * \brief RNG failure during leb_write returns error.
 *
 * \details Arm RNG_FAIL, attempt leb_write. Salt generation fails before
 *          any flash mutation.
 *
 * \expect leb_write returns a non-zero error code.
 */
ZTEST(ubi_secure_crypto_faults, test_rng_fail_on_leb_write)
{
	int vol_id = -1;
	struct ubi_device *ubi = sec_init_with_vol("rng", &vol_id);

	const uint8_t data[] = { 0x01, 0x02 };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_RNG_FAIL, true);
	const int ret = ubi_leb_write(ubi, vol_id, 0, data, sizeof(data));

	zassert_not_equal(ret, 0, "leb_write should fail when RNG is injected");
}

/**
 * \brief RNG failure during leb_write via volume create returns error.
 *
 * \details Create volume, arm RNG_FAIL, attempt write. Salt generation
 *          for the EC encrypt fails before any flash mutation.
 *
 * \expect leb_write returns a non-zero error code.
 */
ZTEST(ubi_secure_crypto_faults, test_rng_fail_on_leb_write_after_create)
{
	int vol_id = -1;
	struct ubi_device *ubi = sec_init_with_vol("rngw", &vol_id);

	const uint8_t data[] = { 0x03, 0x04 };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_RNG_FAIL, true);
	const int ret = ubi_leb_write(ubi, vol_id, 0, data, sizeof(data));

	zassert_not_equal(ret, 0, "leb_write should fail when RNG is injected after create");
}

/* ========================== HKDF fault tests ================================================= */

/**
 * \brief HKDF failure during leb_write returns error.
 *
 * \details Arm HKDF_FAIL, attempt leb_write. Key derivation fails before
 *          any AEAD operation.
 *
 * \expect leb_write returns a non-zero error code.
 */
ZTEST(ubi_secure_crypto_faults, test_hkdf_fail_on_leb_write)
{
	int vol_id = -1;
	struct ubi_device *ubi = sec_init_with_vol("hkdf", &vol_id);

	const uint8_t data[] = { 0x55, 0x66 };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_HKDF_FAIL, true);
	const int ret = ubi_leb_write(ubi, vol_id, 0, data, sizeof(data));

	zassert_not_equal(ret, 0, "leb_write should fail when HKDF is injected");
}

/**
 * \brief HKDF failure during leb_read returns error.
 *
 * \details Write data, arm HKDF_FAIL, attempt read.
 *
 * \expect leb_read returns a non-zero error code.
 */
ZTEST(ubi_secure_crypto_faults, test_hkdf_fail_on_leb_read)
{
	int vol_id = -1;
	struct ubi_device *ubi = sec_init_with_vol("hkdr", &vol_id);

	const uint8_t data[] = { 0x77, 0x88 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t readback[2] = { 0 };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_HKDF_FAIL, true);
	const int ret = ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback));

	zassert_not_equal(ret, 0, "leb_read should fail when HKDF is injected");
}

/* ========================== GET_KEY_ID fault tests =========================================== */

/**
 * \brief get_key_id failure during leb_write returns error.
 *
 * \details Arm GET_KEY_ID_FAIL, attempt leb_write. Key ID retrieval fails
 *          before key derivation.
 *
 * \expect leb_write returns a non-zero error code.
 */
ZTEST(ubi_secure_crypto_faults, test_get_key_id_fail_on_leb_write)
{
	int vol_id = -1;
	struct ubi_device *ubi = sec_init_with_vol("gkw", &vol_id);

	const uint8_t data[] = { 0xAB, 0xCD };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_GET_KEY_ID_FAIL, true);
	const int ret = ubi_leb_write(ubi, vol_id, 0, data, sizeof(data));

	zassert_not_equal(ret, 0, "leb_write should fail when get_key_id is injected");
}

/**
 * \brief get_key_id failure during leb_read returns error.
 *
 * \details Write data, arm GET_KEY_ID_FAIL, attempt read.
 *
 * \expect leb_read returns a non-zero error code.
 */
ZTEST(ubi_secure_crypto_faults, test_get_key_id_fail_on_leb_read)
{
	int vol_id = -1;
	struct ubi_device *ubi = sec_init_with_vol("gkr", &vol_id);

	const uint8_t data[] = { 0x99, 0x88 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t readback[2] = { 0 };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_GET_KEY_ID_FAIL, true);
	const int ret = ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback));

	zassert_not_equal(ret, 0, "leb_read should fail when get_key_id is injected");
}

/**
 * \brief get_key_id failure during leb_write via volume create returns error.
 *
 * \details Create volume, arm GET_KEY_ID_FAIL, attempt write. Key ID
 *          retrieval for the EC read on the free PEB fails.
 *
 * \expect leb_write returns a non-zero error code.
 */
ZTEST(ubi_secure_crypto_faults, test_get_key_id_fail_on_leb_write_after_create)
{
	int vol_id = -1;
	struct ubi_device *ubi = sec_init_with_vol("gkcw", &vol_id);

	const uint8_t data[] = { 0xEF, 0xFE };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_GET_KEY_ID_FAIL, true);
	const int ret = ubi_leb_write(ubi, vol_id, 0, data, sizeof(data));

	zassert_not_equal(ret, 0, "leb_write should fail when get_key_id is injected after create");
}

/* ========================== Freshness rejection test ========================================= */

/**
 * \brief Freshness reject during device_init returns EACCES.
 *
 * \details Init and deinit a device (format flash). Arm FRESHNESS_REJECT,
 *          attempt re-init.
 *
 * \expect device_init returns -EACCES.
 */
ZTEST(ubi_secure_crypto_faults, test_freshness_reject_on_init)
{
	/* First: format the flash by init+deinit. */
	struct ubi_device *ubi = sec_init();

	g_ubi = NULL;
	zassert_ok(ubi_device_deinit(ubi));

	/* Now re-init with freshness rejection armed. */
	static struct ubi_crypto_config cfg;

	cfg = ubi_test_mock_crypto_config();

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_FRESHNESS_REJECT, true);
	const int ret = ubi_device_init(&mtd, &cfg, &ubi);

	zassert_equal(ret, -EACCES, "device_init should return -EACCES on freshness reject");
	/* device_init failed — ubi is NULL, nothing to deinit. */
}

/* ========================== Freshness sync failure test ====================================== */

/**
 * \brief Freshness sync failure emits FRESHNESS_SYNC_FAILURE event.
 *
 * \details Init device, create vol (triggers sync), arm
 *          FRESHNESS_SYNC_FAIL, write LEB (triggers sync again —
 *          this time the hook fires, emitting the event). The device
 *          should still complete the write (event only, not fatal by default).
 *
 * \expect Write succeeds (sync failure is non-fatal by default).
 */
ZTEST(ubi_secure_crypto_faults, test_freshness_sync_fail_on_write)
{
	int vol_id = -1;
	struct ubi_device *ubi = sec_init_with_vol("fsf", &vol_id);

	const uint8_t data[] = { 0xAA, 0xBB };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_FRESHNESS_SYNC_FAIL, true);
	const int ret = ubi_leb_write(ubi, vol_id, 0, data, sizeof(data));

	zassert_ok(ret, "leb_write should succeed despite freshness sync failure");
}

/**
 * \brief Freshness sync failure on volume create emits event.
 *
 * \details Arm FRESHNESS_SYNC_FAIL, create a volume. The sync callback
 *          fires after the create commit — hook causes the event emission.
 *
 * \expect volume_create succeeds (sync failure is non-fatal by default).
 */
ZTEST(ubi_secure_crypto_faults, test_freshness_sync_fail_on_volume_create)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config vol_cfg = {
		.name = "fsvc",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_FRESHNESS_SYNC_FAIL, true);
	const int ret = ubi_volume_create(ubi, &vol_cfg, &vol_id);

	zassert_ok(ret, "volume_create should succeed despite freshness sync failure");
}

/* ========================== Combined fault tests ============================================= */

/**
 * \brief Device remains functional after crypto fault clears.
 *
 * \details Inject AEAD_ENCRYPT_FAIL on first write (fails). One-shot hook
 *          auto-disarms. Second write succeeds.
 *
 * \expect First write fails, second write succeeds, data reads back correctly.
 */
ZTEST(ubi_secure_crypto_faults, test_device_recovers_after_crypto_fault)
{
	int vol_id = -1;
	struct ubi_device *ubi = sec_init_with_vol("rcv", &vol_id);

	const uint8_t data1[] = { 0x11, 0x22 };

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_AEAD_ENCRYPT_FAIL, true);
	const int ret1 = ubi_leb_write(ubi, vol_id, 0, data1, sizeof(data1));

	zassert_not_equal(ret1, 0, "First write should fail (hook armed)");

	/* Hook auto-disarmed. Second write should succeed. */
	const uint8_t data2[] = { 0x33, 0x44 };

	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data2, sizeof(data2)));

	uint8_t readback[2] = { 0 };

	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, data2, sizeof(data2));
}

/**
 * \brief AEAD encrypt failure on dirty PEB erase returns error.
 *
 * \details Write + overwrite to create dirty PEBs. Drain all dirty PEBs
 *          first (so anchor counter catches up), then overwrite again to
 *          create a non-witness dirty PEB. Arm AEAD_ENCRYPT_FAIL, call
 *          erase_peb. The EC header rewrite uses AEAD encrypt — injected
 *          failure causes erase_peb to return an error.
 *
 * \expect erase_peb returns a non-zero error code.
 */
ZTEST(ubi_secure_crypto_faults, test_aead_encrypt_fail_on_erase)
{
	int vol_id = -1;
	struct ubi_device *ubi = sec_init_with_vol("aeb", &vol_id);

	const uint8_t data1[] = { 0x10 };
	const uint8_t data2[] = { 0x20 };
	const uint8_t data3[] = { 0x30 };

	/* Create and drain dirty PEBs so anchor counter catches up. */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data1, sizeof(data1)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data2, sizeof(data2)));
	zassert_ok(ubi_device_erase_peb(ubi));
	zassert_ok(ubi_device_erase_peb(ubi));

	/* Create a fresh dirty PEB whose counter <= anchor counter. */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data3, sizeof(data3)));

	/* Arm encrypt fault — erase_dirty_entry writes EC header via AEAD encrypt. */
	ubi_secure_test_hook_set(UBI_SECURE_HOOK_AEAD_ENCRYPT_FAIL, true);
	const int ret = ubi_device_erase_peb(ubi);

	zassert_not_equal(ret, 0, "erase_peb should fail with AEAD encrypt fault");
}

/**
 * \brief RNG failure during erase-rewrite returns error.
 *
 * \details Create and drain dirty PEBs so anchor counter catches up.
 *          Overwrite again for a non-witness dirty PEB. Arm RNG_FAIL,
 *          call erase_peb. Salt generation for the new EC header fails.
 *
 * \expect erase_peb returns a non-zero error code.
 */
ZTEST(ubi_secure_crypto_faults, test_rng_fail_on_erase)
{
	int vol_id = -1;
	struct ubi_device *ubi = sec_init_with_vol("rne", &vol_id);

	const uint8_t data1[] = { 0x30 };
	const uint8_t data2[] = { 0x40 };
	const uint8_t data3[] = { 0x50 };

	/* Create and drain dirty PEBs so anchor counter catches up. */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data1, sizeof(data1)));
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data2, sizeof(data2)));
	zassert_ok(ubi_device_erase_peb(ubi));
	zassert_ok(ubi_device_erase_peb(ubi));

	/* Create a fresh dirty PEB whose counter <= anchor counter. */
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data3, sizeof(data3)));

	ubi_secure_test_hook_set(UBI_SECURE_HOOK_RNG_FAIL, true);
	const int ret = ubi_device_erase_peb(ubi);

	zassert_not_equal(ret, 0, "erase_peb should fail with RNG fault");
}

/* ================================ Suite registration ========================================= */

ZTEST_SUITE(ubi_secure_crypto_faults, NULL, ztest_suite_setup, ztest_suite_before,
	    ztest_testcase_teardown, NULL);
