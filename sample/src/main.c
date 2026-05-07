/**
 * \file    main.c
 * \author  Kamil Kielbasa
 * \brief   Sample for Unsorted Block Images (UBI) implementation.
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files -------------------------------------------------------------------------------- */

#include <ubi.h>

/* Zephyr headers: */
#include <zephyr/logging/log.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

/* Module defines ------------------------------------------------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)

LOG_MODULE_REGISTER(ubi_sample, CONFIG_UBI_LOG_LEVEL);

/* Module types and type definitions ------------------------------------------------------------ */

/* Module interface variables and constants ----------------------------------------------------- */

/* Static variables and constants --------------------------------------------------------------- */

/* Static function declarations ----------------------------------------------------------------- */

/* Static function definitions ------------------------------------------------------------------ */

/* Module interface function definitions -------------------------------------------------------- */

int main(void)
{
	int ret = -1;

	LOG_INF("Hello world zephyr-ubi sample!");

	const struct device *flash_dev = UBI_PARTITION_DEVICE;
	struct flash_pages_info page_info = { 0 };

	ret = flash_get_page_info_by_offs(flash_dev, 0, &page_info);

	if (ret != 0) {
		LOG_ERR("Get page info failure: %d", ret);
		return ret;
	}

	const size_t write_block_size = flash_get_write_block_size(flash_dev);
	const size_t erase_block_size = page_info.size;

	struct ubi_flash_desc flash = { 0 };
	flash.partition_id = FIXED_PARTITION_ID(UBI_PARTITION_NAME);
	flash.erase_block_size = erase_block_size;
	flash.write_block_size = write_block_size;

	struct ubi_device *ubi = NULL;
	ret = ubi_device_init(&flash, NULL, &ubi);

	if (ret != 0) {
		LOG_ERR("UBI initialization failure: %d", ret);
		return ret;
	}

	/* Create a volume. */
	struct ubi_volume_config vol_cfg = {
		.name = "demo",
		.type = UBI_VOLUME_TYPE_DYNAMIC,
		.leb_count = 2,
	};
	int vol_id = -1;

	ret = ubi_volume_create(ubi, &vol_cfg, &vol_id);

	if (ret != 0) {
		LOG_ERR("Volume create failure: %d", ret);
		goto deinit;
	}

	/* Write data to LEB 0. */
	const char wdata[] = "Hello, UBI!";

	ret = ubi_leb_write(ubi, vol_id, 0, wdata, sizeof(wdata));

	if (ret != 0) {
		LOG_ERR("LEB write failure: %d", ret);
		goto deinit;
	}

	/* Read data back from LEB 0. */
	char rdata[64] = { 0 };

	ret = ubi_leb_read(ubi, vol_id, 0, 0, rdata, sizeof(wdata));

	if (ret != 0) {
		LOG_ERR("LEB read failure: %d", ret);
		goto deinit;
	}

	LOG_INF("Read back: %s", rdata);

	/* Query device info. */
	struct ubi_device_info dev_info = { 0 };

	ret = ubi_device_get_info(ubi, &dev_info);

	if (ret != 0) {
		LOG_ERR("Device get info failure: %d", ret);
		goto deinit;
	}

	LOG_INF("Volumes: %zu, Free PEBs: %zu", dev_info.volume_count, dev_info.free_peb_count);

deinit:
	ret = ubi_device_deinit(ubi);

	if (ret != 0) {
		LOG_ERR("UBI deinitialization failure: %d", ret);
	}

	return ret;
}
