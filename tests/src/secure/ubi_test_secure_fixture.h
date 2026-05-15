/**
 * \file    ubi_test_secure_fixture.h
 * \author  Kamil Kielbasa
 * \brief   Shared test helpers for the secure backend.
 *
 * \copyright Copyright (c) 2026
 */

#ifndef UBI_TEST_SECURE_FIXTURE_H
#define UBI_TEST_SECURE_FIXTURE_H

#include <ubi.h>
#include <ubi_secure.h>
#include <ubi_test.h>

#include <psa/crypto.h>

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

#include <stddef.h>
#include <string.h>

/* PSA test key management ---------------------------------------------------------------------- */

/** 16-byte test root key material (all 0xAA). */
static const uint8_t UBI_TEST_ROOT_KEY_MATERIAL[16] = {
	0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
	0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
};

/** Global PSA key ID of the imported test root key. */
static psa_key_id_t ubi_test_root_key_id = PSA_KEY_ID_NULL;

/**
 * \brief Import the test root key into PSA.
 *
 * Must be called once per test suite setup (after psa_crypto_init).
 */
static inline void ubi_test_import_root_key(void)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
	psa_set_key_algorithm(&attr, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_DERIVE);
	psa_set_key_bits(&attr, 128);

	psa_status_t status = psa_import_key(&attr, UBI_TEST_ROOT_KEY_MATERIAL,
					     sizeof(UBI_TEST_ROOT_KEY_MATERIAL),
					     &ubi_test_root_key_id);
	zassert_equal(status, PSA_SUCCESS, "PSA import key failed: %d", (int)status);
}

/**
 * \brief Destroy the test root key from PSA.
 */
static inline void ubi_test_destroy_root_key(void)
{
	psa_destroy_key(ubi_test_root_key_id);
	ubi_test_root_key_id = PSA_KEY_ID_NULL;
}

/* Mock callbacks for testing ------------------------------------------------------------------- */

static inline int mock_get_key_id(uint8_t key_version, psa_key_id_t *key_id_out)
{
	(void)key_version;
	*key_id_out = ubi_test_root_key_id;
	return 0;
}

static inline enum ubi_secure_rollback_verdict
mock_check_freshness(const struct ubi_secure_freshness *freshness, void *user_data)
{
	(void)freshness;
	(void)user_data;
	return UBI_SECURE_ROLLBACK_ACCEPT;
}

static inline int mock_sync_freshness(const struct ubi_secure_freshness *freshness, void *user_data)
{
	(void)freshness;
	(void)user_data;
	return 0;
}

static inline enum ubi_secure_event_verdict mock_event_cb(const struct ubi_secure_event *event,
							  void *user_data)
{
	(void)event;
	(void)user_data;
	return UBI_SECURE_EVENT_CONTINUE;
}

/**
 * \brief Build a mock crypto config with all callbacks wired to permissive stubs.
 */
static inline struct ubi_secure_config ubi_test_mock_secure_config(void)
{
	static const uint8_t allowed_versions[] = { 1 };

	struct ubi_secure_config cfg = {
		.policy = {
			.requested_write_key_version = 1,
			.allowed_key_versions = allowed_versions,
			.allowed_key_versions_len = 1,
		},
		.get_key_id = mock_get_key_id,
		.check_freshness = mock_check_freshness,
		.sync_freshness = mock_sync_freshness,
		.event_cb = mock_event_cb,
		.user_data = NULL,
	};
	return cfg;
}

/* Suite-setup boilerplate ---------------------------------------------------------------------- */

#ifndef UBI_PARTITION_NAME
#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)
#endif /* UBI_PARTITION_NAME */

/**
 * \brief Initialise the shared `ubi_flash_desc` and the PSA test key.
 *
 * \details Encapsulates the boilerplate that every secure test file's
 *          `ztest_suite_setup()` previously repeated verbatim: locate the
 *          `ubi_partition` device, read its page geometry, fill the
 *          caller-owned `ubi_flash_desc`, initialise PSA and import the
 *          test root key. Each test file keeps a file-static
 *          `struct ubi_flash_desc flash` and passes a pointer here.
 */
static inline void ubi_test_secure_suite_setup_impl(struct ubi_flash_desc *flash_out)
{
	const struct device *const flash_dev = UBI_PARTITION_DEVICE;

	zassert_true(device_is_ready(flash_dev));

	struct flash_pages_info page_info = { 0 };

	zassert_ok(flash_get_page_info_by_offs(flash_dev, 0, &page_info));

	flash_out->partition_id = FIXED_PARTITION_ID(UBI_PARTITION_NAME);
	flash_out->erase_block_size = page_info.size;
	flash_out->write_block_size = flash_get_write_block_size(flash_dev);

	zassert_equal(psa_crypto_init(), PSA_SUCCESS);
	ubi_test_import_root_key();
}

/**
 * \brief Reset shared test state between secure-suite test cases.
 *
 * \details Encapsulates the boilerplate that most secure test files'
 *          `ztest_suite_before()` callbacks repeat: release any partition
 *          handles still held by the previous test, reset the test
 *          allocator fault injector to its default permissive state, and
 *          erase the entire `ubi_partition` so the next test starts from
 *          a known blank flash. Files that also track an alias-globalka
 *          such as `g_ubi` are expected to clear it themselves after the
 *          call; this helper does not know about per-file globals.
 */
static inline void ubi_test_secure_before_impl(void)
{
	ubi_test_partition_force_release_all();
	ubi_test_fault_reset();
	zassert_ok(flash_erase(UBI_PARTITION_DEVICE, UBI_PARTITION_OFFSET, UBI_PARTITION_SIZE));
}

/**
 * \brief Initialise a secure UBI device wired to the test mock crypto config.
 *
 * \details Builds a fresh `ubi_secure_config` from the test mocks (in a
 *          file-static so the device can keep its pointer alive past
 *          this call), invokes `ubi_device_init()` against the caller's
 *          flash descriptor and asserts success. Callers are expected to
 *          store the returned pointer into their per-file `g_ubi` alias
 *          so the suite's teardown safety net can recover from a test
 *          that aborts before its own deinit runs.
 */
static inline struct ubi_device *ubi_test_secure_init(struct ubi_flash_desc *flash_desc)
{
	static struct ubi_secure_config cfg;

	cfg = ubi_test_mock_secure_config();
	struct ubi_device *ubi = NULL;

	zassert_ok(ubi_device_init(flash_desc, &cfg, &ubi));
	return ubi;
}

#endif /* UBI_TEST_SECURE_FIXTURE_H */
