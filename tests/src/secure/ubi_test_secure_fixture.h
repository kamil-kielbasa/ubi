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
#include <ubi_crypto.h>
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

static inline enum ubi_crypto_rollback_verdict
mock_check_freshness(const struct ubi_crypto_freshness *freshness, void *user_data)
{
	(void)freshness;
	(void)user_data;
	return UBI_CRYPTO_ROLLBACK_ACCEPT;
}

static inline int mock_sync_freshness(const struct ubi_crypto_freshness *freshness, void *user_data)
{
	(void)freshness;
	(void)user_data;
	return 0;
}

static inline enum ubi_crypto_event_verdict mock_event_cb(const struct ubi_crypto_event *event,
							  void *user_data)
{
	(void)event;
	(void)user_data;
	return UBI_CRYPTO_EVENT_CONTINUE;
}

/**
 * \brief Build a mock crypto config with all callbacks wired to permissive stubs.
 */
static inline struct ubi_crypto_config ubi_test_mock_crypto_config(void)
{
	static const uint8_t allowed_versions[] = { 1 };

	struct ubi_crypto_config cfg = {
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

#endif /* UBI_TEST_SECURE_FIXTURE_H */
