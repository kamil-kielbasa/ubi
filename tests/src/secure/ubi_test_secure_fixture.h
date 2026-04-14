/**
 * \file    ubi_test_secure_fixture.h
 * \brief   Shared test helpers for the secure backend.
 *
 * \copyright Copyright (c) 2026
 */

#ifndef UBI_TEST_SECURE_FIXTURE_H
#define UBI_TEST_SECURE_FIXTURE_H

#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

#include <stddef.h>
#include <string.h>

/* ---- Mock callbacks for testing ------------------------------------------------------------- */

static inline int mock_get_key_id(uint8_t key_version, uint32_t *key_id_out)
{
	ARG_UNUSED(key_version);
	*key_id_out = 1;
	return 0;
}

static inline enum ubi_crypto_rollback_verdict
mock_check_freshness(const struct ubi_crypto_freshness *freshness, void *user_data)
{
	ARG_UNUSED(freshness);
	ARG_UNUSED(user_data);
	return UBI_CRYPTO_ROLLBACK_ACCEPT;
}

static inline int mock_sync_freshness(const struct ubi_crypto_freshness *freshness, void *user_data)
{
	ARG_UNUSED(freshness);
	ARG_UNUSED(user_data);
	return 0;
}

static inline enum ubi_crypto_event_verdict mock_event_cb(const struct ubi_crypto_event *event,
							  void *user_data)
{
	ARG_UNUSED(event);
	ARG_UNUSED(user_data);
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
