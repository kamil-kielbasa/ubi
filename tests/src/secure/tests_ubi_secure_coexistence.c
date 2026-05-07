/**
 * \file    tests_ubi_secure_coexistence.c
 * \author  Kamil Kielbasa
 *
 * \brief   Plain + secure backend coexistence on two partitions.
 *
 * \details Verifies that the same process can hold one plain UBI device
 *          on partition `ubi_partition` and one secure UBI device on
 *          partition `ubi_partition_2` at the same time, with both
 *          devices independently usable for volume create / LEB I/O,
 *          surviving a deinit-reinit cycle without state bleed-through.
 *          Closes audit §10.1 #1 (plain + secure coexistence).
 *
 *          Built only when both \c CONFIG_UBI_CRYPTO and the second
 *          partition exist in the active devicetree (native_sim
 *          variants).  HW boards that have only the primary
 *          \c ubi_partition skip this suite.
 *
 * \copyright Copyright (c) 2026
 */

/* Include files -------------------------------------------------------------------------------- */

#include <ubi.h>
#include <ubi_crypto.h>
#include <ubi_test.h>

#include "ubi_test_fixture.h"
#include "ubi_test_secure_fixture.h"

#include <psa/crypto.h>

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

#include <errno.h>
#include <string.h>

/* Module defines ------------------------------------------------------------------------------- */

/* Module types and type definitiones ----------------------------------------------------------- */

/* Module interface variables and constants ----------------------------------------------------- */
#define UBI_PART_PLAIN ubi_partition
#define UBI_PART_SECURE ubi_partition_2

#define UBI_PART_PLAIN_DEVICE FIXED_PARTITION_DEVICE(UBI_PART_PLAIN)
#define UBI_PART_PLAIN_OFFSET FIXED_PARTITION_OFFSET(UBI_PART_PLAIN)
#define UBI_PART_PLAIN_SIZE FIXED_PARTITION_SIZE(UBI_PART_PLAIN)

#define UBI_PART_SECURE_DEVICE FIXED_PARTITION_DEVICE(UBI_PART_SECURE)
#define UBI_PART_SECURE_OFFSET FIXED_PARTITION_OFFSET(UBI_PART_SECURE)
#define UBI_PART_SECURE_SIZE FIXED_PARTITION_SIZE(UBI_PART_SECURE)

/* Static variables and constants --------------------------------------------------------------- */

/* Static function declarations ----------------------------------------------------------------- */
static struct ubi_flash_desc flash_plain;
static struct ubi_flash_desc flash_secure;
static struct ubi_device *g_plain;
static struct ubi_device *g_secure;

/* Static function definitions ------------------------------------------------------------------ */
static void *ztest_suite_setup(void)
{
	const struct device *dev_plain = UBI_PART_PLAIN_DEVICE;
	const struct device *dev_secure = UBI_PART_SECURE_DEVICE;
	zassert_true(device_is_ready(dev_plain));
	zassert_true(device_is_ready(dev_secure));

	struct flash_pages_info pi = { 0 };
	zassert_ok(flash_get_page_info_by_offs(dev_plain, UBI_PART_PLAIN_OFFSET, &pi));
	flash_plain.partition_id = FIXED_PARTITION_ID(UBI_PART_PLAIN);
	flash_plain.erase_block_size = pi.size;
	flash_plain.write_block_size = flash_get_write_block_size(dev_plain);

	memset(&pi, 0, sizeof(pi));
	zassert_ok(flash_get_page_info_by_offs(dev_secure, UBI_PART_SECURE_OFFSET, &pi));
	flash_secure.partition_id = FIXED_PARTITION_ID(UBI_PART_SECURE);
	flash_secure.erase_block_size = pi.size;
	flash_secure.write_block_size = flash_get_write_block_size(dev_secure);

	zassert_equal(psa_crypto_init(), PSA_SUCCESS);
	ubi_test_import_root_key();

	return NULL;
}

static void ztest_suite_before(void *ctx)
{
	(void)ctx;
	g_plain = NULL;
	g_secure = NULL;
	ubi_test_partition_force_release_all();
	zassert_ok(flash_erase(UBI_PART_PLAIN_DEVICE, UBI_PART_PLAIN_OFFSET, UBI_PART_PLAIN_SIZE));
	zassert_ok(
		flash_erase(UBI_PART_SECURE_DEVICE, UBI_PART_SECURE_OFFSET, UBI_PART_SECURE_SIZE));
}

static void ztest_suite_after(void *ctx)
{
	(void)ctx;
	if (g_plain) {
		(void)ubi_device_deinit(g_plain);
		g_plain = NULL;
	}
	if (g_secure) {
		(void)ubi_device_deinit(g_secure);
		g_secure = NULL;
	}
}

/* Module interface function definitions -------------------------------------------------------- */
/**
 * \brief A plain and a secure UBI device live side by side on two partitions.
 *
 * \details Scenario: Steps:
 *  1. Init plain backend on \c ubi_partition (crypto_cfg = NULL).
 *  2. Init secure backend on \c ubi_partition_2 (crypto_cfg != NULL).
 *  3. Create one volume on each device, with distinct names.
 *  4. Write a unique payload to LEB 0 of each volume.
 *  5. Read both LEBs back and check payload integrity (no cross-talk).
 *  6. Deinit both, reattach in reverse order, verify payloads survive.
 *
 * \expect
 *  - All `ubi_device_init`, `ubi_volume_create`, `ubi_leb_write`,
 *    `ubi_leb_read`, `ubi_device_deinit` calls succeed on both devices.
 *  - Payloads written via the plain device read back unchanged from the
 *    plain device, and likewise for the secure device, with no
 *    mix-up after a full deinit / reattach cycle.
 *  - Re-init order does not matter (secure first, then plain).
 */
ZTEST(ubi_secure_coexistence, test_plain_and_secure_devices_coexist)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	/* Init both backends on their respective partitions. */
	zassert_ok(ubi_device_init(&flash_plain, NULL, &g_plain),
		   "plain init on ubi_partition failed");
	zassert_ok(ubi_device_init(&flash_secure, &cfg, &g_secure),
		   "secure init on ubi_partition_2 failed");

	/* Distinct volume configs. */
	const struct ubi_volume_config vol_plain_cfg = {
		.name = { '/', 'p', 'l', 'n' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};
	const struct ubi_volume_config vol_secure_cfg = {
		.name = { '/', 's', 'e', 'c' },
		.type = UBI_VOLUME_TYPE_STATIC,
		.leb_count = 1,
	};

	int vol_plain_id = -1;
	int vol_secure_id = -1;
	zassert_ok(ubi_volume_create(g_plain, &vol_plain_cfg, &vol_plain_id));
	zassert_ok(ubi_volume_create(g_secure, &vol_secure_cfg, &vol_secure_id));

	/* Distinct payloads to detect any cross-talk between the two devices. */
	const uint8_t payload_plain[] = { 0xAA, 0xBB, 0xCC, 0xDD };
	const uint8_t payload_secure[] = { 0x11, 0x22, 0x33, 0x44 };

	zassert_ok(ubi_leb_write(g_plain, vol_plain_id, 0, payload_plain, sizeof(payload_plain)));
	zassert_ok(
		ubi_leb_write(g_secure, vol_secure_id, 0, payload_secure, sizeof(payload_secure)));

	/* Read back each from its own device and verify no cross-talk. */
	uint8_t rb_plain[sizeof(payload_plain)] = { 0 };
	uint8_t rb_secure[sizeof(payload_secure)] = { 0 };
	zassert_ok(ubi_leb_read(g_plain, vol_plain_id, 0, 0, rb_plain, sizeof(payload_plain)));
	zassert_ok(ubi_leb_read(g_secure, vol_secure_id, 0, 0, rb_secure, sizeof(payload_secure)));
	zassert_mem_equal(rb_plain, payload_plain, sizeof(payload_plain));
	zassert_mem_equal(rb_secure, payload_secure, sizeof(payload_secure));

	/* Deinit both, reattach in reverse order, verify payloads survive. */
	zassert_ok(ubi_device_deinit(g_plain));
	g_plain = NULL;
	zassert_ok(ubi_device_deinit(g_secure));
	g_secure = NULL;

	zassert_ok(ubi_device_init(&flash_secure, &cfg, &g_secure),
		   "secure reattach on ubi_partition_2 failed");
	zassert_ok(ubi_device_init(&flash_plain, NULL, &g_plain),
		   "plain reattach on ubi_partition failed");

	memset(rb_plain, 0, sizeof(rb_plain));
	memset(rb_secure, 0, sizeof(rb_secure));
	zassert_ok(ubi_leb_read(g_plain, vol_plain_id, 0, 0, rb_plain, sizeof(payload_plain)));
	zassert_ok(ubi_leb_read(g_secure, vol_secure_id, 0, 0, rb_secure, sizeof(payload_secure)));
	zassert_mem_equal(rb_plain, payload_plain, sizeof(payload_plain));
	zassert_mem_equal(rb_secure, payload_secure, sizeof(payload_secure));

	zassert_ok(ubi_device_deinit(g_plain));
	g_plain = NULL;
	zassert_ok(ubi_device_deinit(g_secure));
	g_secure = NULL;
}

/**
 * \brief Re-attaching the same partition twice must fail with -EBUSY.
 *
 * \details Scenario: Sanity check that the partition guard still rejects a second
 *          attach to a partition that already has a live UBI device,
 *          even when another partition is in use by the other backend.
 *
 * \expect Second attach to the plain partition returns -EBUSY and the duplicate handle is NULL.
 *         Second attach to the secure partition returns -EBUSY and the duplicate handle is NULL.
 */
ZTEST(ubi_secure_coexistence, test_partition_guard_blocks_double_attach)
{
	struct ubi_crypto_config cfg = ubi_test_mock_crypto_config();

	zassert_ok(ubi_device_init(&flash_plain, NULL, &g_plain));
	zassert_ok(ubi_device_init(&flash_secure, &cfg, &g_secure));

	struct ubi_device *dup = NULL;
	zassert_equal(ubi_device_init(&flash_plain, NULL, &dup), -EBUSY,
		      "second attach to plain partition must return -EBUSY");
	zassert_is_null(dup);

	zassert_equal(ubi_device_init(&flash_secure, &cfg, &dup), -EBUSY,
		      "second attach to secure partition must return -EBUSY");
	zassert_is_null(dup);

	zassert_ok(ubi_device_deinit(g_plain));
	g_plain = NULL;
	zassert_ok(ubi_device_deinit(g_secure));
	g_secure = NULL;
}

ZTEST_SUITE(ubi_secure_coexistence, NULL, ztest_suite_setup, ztest_suite_before, ztest_suite_after,
	    NULL);
