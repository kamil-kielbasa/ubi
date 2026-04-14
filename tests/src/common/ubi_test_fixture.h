/**
 * \file    ubi_test_fixture.h
 * \brief   Shared test helpers: MTD setup, partition erase, device lifecycle.
 *
 * \copyright Copyright (c) 2026
 */

#ifndef UBI_TEST_FIXTURE_H
#define UBI_TEST_FIXTURE_H

#include <ubi.h>
#include <ubi_test.h>

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

#include <stddef.h>
#include <string.h>

#define UBI_TEST_PARTITION_NAME ubi_partition
#define UBI_TEST_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_TEST_PARTITION_NAME)
#define UBI_TEST_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_TEST_PARTITION_NAME)
#define UBI_TEST_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_TEST_PARTITION_NAME)

/**
 * \brief Populate an ubi_mtd descriptor from the DeviceTree partition.
 */
static inline void ubi_test_setup_mtd(struct ubi_mtd *mtd)
{
	const struct device *flash_dev = UBI_TEST_PARTITION_DEVICE;
	zassert_true(device_is_ready(flash_dev));

	struct flash_pages_info page_info = { 0 };
	zassert_ok(flash_get_page_info_by_offs(flash_dev, 0, &page_info));

	mtd->partition_id = FIXED_PARTITION_ID(UBI_TEST_PARTITION_NAME);
	mtd->erase_block_size = page_info.size;
	mtd->write_block_size = flash_get_write_block_size(flash_dev);
}

/**
 * \brief Erase the entire test partition.
 */
static inline void ubi_test_erase_partition(void)
{
	ubi_test_partition_force_release_all();
	zassert_ok(flash_erase(UBI_TEST_PARTITION_DEVICE, UBI_TEST_PARTITION_OFFSET,
			       UBI_TEST_PARTITION_SIZE));
}

/**
 * \brief Init a UBI device on the test partition (after erase).
 */
static inline struct ubi_device *ubi_test_init_device(const struct ubi_mtd *mtd)
{
	struct ubi_device *ubi = NULL;
	zassert_ok(ubi_device_init(mtd, NULL, &ubi));
	return ubi;
}

/**
 * \brief Erase partition and re-init a fresh device.
 */
static inline struct ubi_device *ubi_test_reinit_device(const struct ubi_mtd *mtd)
{
	ubi_test_erase_partition();
	return ubi_test_init_device(mtd);
}

#endif /* UBI_TEST_FIXTURE_H */
