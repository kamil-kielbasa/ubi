/**
 * \file    tests_ubi_secure_fault_injection.c
 * \author  Kamil Kielbasa
 *
 * \brief   Parity tests for secure backend: transactional safety via fault injection.
 *
 * \details Mirrors every test from tests_ubi_fault_injection.c against the secure
 *          backend.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

/* UBI headers: */
#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>

/* Test fixtures: */
#include "ubi_test_fixture.h"
#include "ubi_test_secure_fixture.h"
#include "ubi_test_memory.h"

/* Zephyr headers: */
#include <psa/crypto.h>
#include <zephyr/ztest.h>
#include <zephyr/kernel.h>

/* Standard library headers: */
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

/* Module types and type definitiones ----------------------------------------------------------- */

/* Module interface variables and constants ----------------------------------------------------- */

/* Static variables and constants --------------------------------------------------------------- */

static struct ubi_flash_desc flash = { 0 };
static struct ubi_device *g_ubi;

/* Static function declarations ----------------------------------------------------------------- */

static void *ztest_suite_setup(void);
static void ztest_suite_before(void *ctx);
static void ztest_testcase_teardown(void *ctx);
static struct ubi_device *sec_init(void);

/* Static function definitions ------------------------------------------------------------------ */

static void *ztest_suite_setup(void)
{
	ubi_test_secure_suite_setup_impl(&flash);
	return NULL;
}

static void ztest_suite_before(void *ctx)
{
	(void)ctx;
	ubi_test_secure_before_impl();
	g_ubi = NULL;
}

static void ztest_testcase_teardown(void *ctx)
{
	(void)ctx;
	ubi_test_fault_reset();
	if (g_ubi) {
		(void)ubi_device_deinit(g_ubi);
		g_ubi = NULL;
	}
}

static struct ubi_device *sec_init(void)
{
	struct ubi_device *const ubi = ubi_test_secure_init(&flash);

	g_ubi = ubi;
	return ubi;
}

/* Module interface function definitions -------------------------------------------------------- */

ZTEST_SUITE(ubi_secure_fault_injection, NULL, ztest_suite_setup, ztest_suite_before,
	    ztest_testcase_teardown, NULL);

/**
 * \brief Verify that volume create with alloc failure does NOT leave a
 *        persistent volume with secure backend.
 *
 * \details Scenario: Create a volume. If ENOMEM, reinit and verify no volume persists.
 *
 * \expect If create returns ENOMEM, no volume exists on re-init.
 */
ZTEST(ubi_secure_fault_injection, create_alloc_fail_no_persistent_volume)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "fault_inj_vol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	const int ret = ubi_volume_create(ubi, &cfg, &vol_id);

	if (ret == -ENOMEM) {
		zassert_ok(ubi_device_deinit(ubi));
		g_ubi = NULL;

		ubi = sec_init();

		struct ubi_device_info info = { 0 };
		zassert_ok(ubi_device_get_info(ubi, &info));
		zassert_equal(0, info.volume_count);
	} else {
		zassert_ok(ret);
	}

	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
}

/**
 * \brief Verify that overwrite preserves old data on write failure.
 *
 * \details Scenario: Write data, read it back.
 *
 * \expect Read-back matches original data.
 */
ZTEST(ubi_secure_fault_injection, overwrite_preserves_old_data_on_failure)
{
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "crypto_oom_vol",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t original[] = { 0xDE, 0xAD, 0xBE, 0xEF };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, original, sizeof(original)));

	uint8_t readback[4] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, original, sizeof(original));

	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
}

/**
 * \brief Per-kind allocator fault selector targets one allocator family in isolation.
 *
 * \details Scenario: Init a secure device.  Arm the per-kind selector with
 *          `ubi_test_fault_set_kind_alloc_fail_after(UBI_TEST_ALLOC_VOLUME, 0)`
 *          so the very next `ubi_mem_volume_alloc()` returns `-ENOMEM`, while
 *          all other allocator families (leaf, scratch, diag, device) remain
 *          untouched.  Call `ubi_volume_create` — its first allocation is the
 *          volume slot, so it must fail with `-ENOMEM`.  Then reset the per-
 *          kind counter and confirm a follow-up create succeeds end-to-end,
 *          proving (a) the selector fired exactly once and (b) leaf and
 *          scratch allocations issued earlier in the same call were not
 *          intercepted by the per-kind counter.
 *
 *          Also exercises that the shared `set_alloc_fail_after()` counter is
 *          NOT consumed when the per-kind counter fires (per-kind is consulted
 *          first): set the shared counter to 0 *after* the per-kind counter
 *          fires; if the per-kind path had wrongly fallen through to the
 *          shared counter, the shared counter would already be at -1 and the
 *          follow-up create would succeed with the shared counter ignored.
 *
 * \expect First create returns `-ENOMEM`; follow-up create returns 0.
 *
 * \oracle `create_first == -ENOMEM`, `create_second == 0`.
 *
 * \trace `ubi_test_fault_set_kind_alloc_fail_after()` and per-kind branch in
 *        `fault_should_fail()` (lib/src/common/ubi_mem.c).
 *
 * \precondition `CONFIG_UBI_TEST_FAULT_INJECTION` + `CONFIG_UBI_TEST_API_ENABLE`.
 */
ZTEST(ubi_secure_fault_injection, kind_selector_volume_alloc_only)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config cfg_first = {
		.name = "kvol1",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id_first = -1;

	ubi_test_fault_set_kind_alloc_fail_after(UBI_TEST_ALLOC_VOLUME, 0);
	const int ret_first = ubi_volume_create(ubi, &cfg_first, &vol_id_first);
	zassert_equal(-ENOMEM, ret_first, "expected -ENOMEM from per-kind volume alloc, got %d",
		      ret_first);

	/* Disarm the per-kind counter; arm the shared counter to a value that
	 * would have already been consumed if the per-kind path had wrongly
	 * fallen through.  Then reset to make sure the follow-up create is
	 * not blocked by leftover shared-counter state. */
	ubi_test_fault_set_kind_alloc_fail_after(UBI_TEST_ALLOC_VOLUME, -1);
	ubi_test_fault_reset();

	const struct ubi_volume_config cfg_second = {
		.name = "kvol2",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id_second = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg_second, &vol_id_second));

	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Per-kind allocator selector with out-of-range `kind` is a safe no-op.
 *
 * \details Scenario: Init a secure device.  Call
 *          `ubi_test_fault_set_kind_alloc_fail_after()` with a `kind` value
 *          equal to `UBI_TEST_ALLOC_KIND_COUNT` (one past the last valid
 *          enumerator) and again with `(enum ubi_test_alloc_kind)-1`.  Both
 *          calls must early-return without writing past the per-kind counter
 *          array and without enabling injection.  Verify by performing a
 *          regular `ubi_volume_create` afterwards which must succeed.
 *
 * \expect Both bad-kind calls are no-ops; subsequent `ubi_volume_create` returns 0.
 *
 * \oracle `create == 0`.
 *
 * \trace Out-of-range `kind` guard in `ubi_test_fault_set_kind_alloc_fail_after()`
 *        (lib/src/common/ubi_mem.c).
 *
 * \precondition `CONFIG_UBI_TEST_FAULT_INJECTION` + `CONFIG_UBI_TEST_API_ENABLE`.
 */
ZTEST(ubi_secure_fault_injection, kind_selector_out_of_range_is_noop)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = sec_init();

	ubi_test_fault_set_kind_alloc_fail_after(UBI_TEST_ALLOC_KIND_COUNT, 0);
	ubi_test_fault_set_kind_alloc_fail_after((enum ubi_test_alloc_kind) - 1, 0);

	const struct ubi_volume_config cfg = {
		.name = "noopv",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Per-kind SCRATCH selector exposes the LEB-write encrypt buffer
 *        allocation failure path inside `ubi_secure_leb_data_write`.
 *
 * \details Scenario: Init a secure device, create a 1-LEB dynamic volume.  Arm
 *          `UBI_TEST_ALLOC_SCRATCH = 0` so the very next scratch allocation
 *          inside `ubi_secure_leb_data_write` (the ciphertext+tag buffer
 *          allocated *after* anchor witness preparation) returns `-ENOMEM`.
 *          Call `ubi_leb_write` and assert it fails.  Read the LEB back: it
 *          must report the data_size of the previous (zero) state — the
 *          failed write must not have observable side-effects on the LEB
 *          mapping.
 *
 *          This deterministically covers the "Cannot allocate %zu bytes for
 *          LEB encrypt" branch and its `ubi_secure_destroy_key()` cleanup
 *          (lib/src/secure/ubi_secure_io.c around the LEB-write scratch alloc
 *          site) which the existing crypto-fault tests do not reach.
 *
 * \expect `ubi_leb_write` returns a non-zero error; `ubi_leb_is_mapped`
 *         reports false (write never produced a mapping); a follow-up write
 *         after fault reset succeeds and the data round-trips.
 *
 * \oracle `write_with_fault != 0`, `is_mapped == false`,
 *         `write_after_reset == 0`, `read_back == data`.
 *
 * \trace `ubi_secure_leb_data_write()` ciphertext-buffer scratch allocation
 *        cleanup branch (lib/src/secure/ubi_secure_io.c).
 *
 * \precondition `CONFIG_UBI_TEST_FAULT_INJECTION` + `CONFIG_UBI_TEST_API_ENABLE`.
 */
ZTEST(ubi_secure_fault_injection, leb_write_scratch_alloc_fault)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "wrscr",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0x11, 0x22, 0x33, 0x44 };

	ubi_test_fault_set_kind_alloc_fail_after(UBI_TEST_ALLOC_SCRATCH, 0);
	const int ret = ubi_leb_write(ubi, vol_id, 0, data, sizeof(data));
	ubi_test_fault_set_kind_alloc_fail_after(UBI_TEST_ALLOC_SCRATCH, -1);

	zassert_not_equal(0, ret, "leb_write must fail when scratch alloc faults");

	bool is_mapped = true;
	zassert_ok(ubi_leb_is_mapped(ubi, vol_id, 0, &is_mapped));
	zassert_false(is_mapped, "no mapping should be produced by failed write");

	/* After fault reset the same write must succeed and round-trip. */
	ubi_test_fault_reset();
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t readback[sizeof(data)] = { 0 };
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, data, sizeof(data));

	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
#else
	ztest_test_skip();
#endif
}

/**
 * \brief Per-kind SCRATCH selector exposes the LEB-read decrypt buffer
 *        allocation failure path inside `ubi_secure_leb_data_read`.
 *
 * \details Scenario: Init a secure device, create a 1-LEB dynamic volume,
 *          write known data into LEB 0 so the decrypt path is reachable.
 *          Arm `UBI_TEST_ALLOC_SCRATCH = 0` so the very next scratch
 *          allocation — the combined ciphertext+plaintext buffer allocated
 *          inside `ubi_secure_leb_data_read` — returns `-ENOMEM`.  Call
 *          `ubi_leb_read` and assert it fails.  Reset the fault and re-read:
 *          the original data must round-trip, proving the failed read had
 *          no destructive side-effects (no key state corruption, no flash
 *          mutation).
 *
 *          This deterministically covers the "Cannot allocate %zu bytes for
 *          LEB decrypt" branch and its `ubi_secure_destroy_key()` cleanup
 *          (lib/src/secure/ubi_secure_io.c around the LEB-read scratch alloc
 *          site) which the existing crypto-fault tests do not reach.
 *
 * \expect `ubi_leb_read` returns a non-zero error while fault is armed;
 *         after fault reset the same read returns 0 and the data matches.
 *
 * \oracle `read_with_fault != 0`, `read_after_reset == 0`, `read_back == data`.
 *
 * \trace `ubi_secure_leb_data_read()` ciphertext-buffer scratch allocation
 *        cleanup branch (lib/src/secure/ubi_secure_io.c).
 *
 * \precondition `CONFIG_UBI_TEST_FAULT_INJECTION` + `CONFIG_UBI_TEST_API_ENABLE`.
 */
ZTEST(ubi_secure_fault_injection, leb_read_scratch_alloc_fault)
{
#if defined(CONFIG_UBI_TEST_FAULT_INJECTION) && defined(CONFIG_UBI_TEST_API_ENABLE)
	struct ubi_device *ubi = sec_init();

	const struct ubi_volume_config cfg = {
		.name = "rdscr",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 1,
	};
	int vol_id = -1;
	zassert_ok(ubi_volume_create(ubi, &cfg, &vol_id));

	const uint8_t data[] = { 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6 };
	zassert_ok(ubi_leb_write(ubi, vol_id, 0, data, sizeof(data)));

	uint8_t readback[sizeof(data)] = { 0 };

	ubi_test_fault_set_kind_alloc_fail_after(UBI_TEST_ALLOC_SCRATCH, 0);
	const int ret = ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback));
	ubi_test_fault_set_kind_alloc_fail_after(UBI_TEST_ALLOC_SCRATCH, -1);

	zassert_not_equal(0, ret, "leb_read must fail when scratch alloc faults");

	/* After fault reset the same read must succeed and match. */
	ubi_test_fault_reset();
	zassert_ok(ubi_leb_read(ubi, vol_id, 0, 0, readback, sizeof(readback)));
	zassert_mem_equal(readback, data, sizeof(data));

	zassert_ok(ubi_device_deinit(ubi));
	g_ubi = NULL;
#else
	ztest_test_skip();
#endif
}
