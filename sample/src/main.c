/**
 * \file    main.c
 * \author  Kamil Kielbasa
 * \brief   Sample for Unsorted Block Images (UBI) implementation.
 * \version 0.1
 * \date    2025-07-25
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files ----------------------------------------------------------- */

/* UBI header: */
#include <ubi.h>

/* Zephyr headers: */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <zephyr/toolchain/common.h>

/* Standard library headers: */
#include <stddef.h>
#include <string.h>

/* Module defines ---------------------------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)
#define UBI_PARTITION_OFFSET FIXED_PARTITION_OFFSET(UBI_PARTITION_NAME)
#define UBI_PARTITION_SIZE FIXED_PARTITION_SIZE(UBI_PARTITION_NAME)

/* Module types and type definitiones -------------------------------------- */
/* Module interface variables and constants -------------------------------- */
/* Static variables and constants ------------------------------------------ */
/* Static function declarations -------------------------------------------- */
/* Static function definitions --------------------------------------------- */
/* Module interface function definitions ----------------------------------- */

int main(void)
{
	printk("Hello world zephyr-ubi sample!\n");

	/* 1. Check and print flash and partitons informations. */

	int ret = 0;
	const struct device *flash_dev = UBI_PARTITION_DEVICE;

	if (!device_is_ready(flash_dev)) {
		printk("Flash device %s is not ready\n", flash_dev->name);
	}

	printk("Flash partition device name: %s\n", flash_dev->name);
	printk("Flash partition offset: %x\n", UBI_PARTITION_OFFSET);
	printk("Flash partition size: %x\n", UBI_PARTITION_SIZE);

	size_t write_block_size = flash_get_write_block_size(flash_dev);

	struct flash_pages_info page_info = { 0 };
	ret = flash_get_page_info_by_offs(flash_dev, 0, &page_info);
	if (ret != 0) {
		printk("Failed to get page info: %d\n", ret);
		return -1;
	}

	size_t erase_block_size = page_info.size;

	printk("Flash write block size: %zu bytes\n", write_block_size);
	printk("Flash erase block size: %zu bytes\n", erase_block_size);

	const struct mem_tech_device mtd = {
		.dev = flash_dev,
		.p_off = UBI_PARTITION_OFFSET,
		.p_size = UBI_PARTITION_SIZE,
		.eb_size = erase_block_size,
		.wb_size = write_block_size,
	};

	ret = ubi_init(&mtd);

	if (0 == ret) {
		printk("ubi_init successfully!\n");
	} else {
		printk("ubi_init failed!\n");
	}

	const size_t lnum = 0;
	bool is_mapped = false;

	ret = ubi_leb_is_mapped(lnum, &is_mapped);

	if (0 == ret) {
		printk("ubi_leb_is_mapped successfully!\n");
	} else {
		printk("ubi_leb_is_mapped failed!\n");
	}

	if (false == is_mapped) {
		ret = ubi_leb_map(lnum);

		if (0 == ret) {
			printk("ubi_leb_map successfully!\n");
		} else {
			printk("ubi_leb_map failed!\n");
		}
	} else {
		printk("ubi_leb_map already mapped!\n");
	}

	ret = ubi_leb_unmap(lnum);

	if (0 == ret) {
		printk("ubi_leb_unmap successfully!\n");
	} else {
		printk("ubi_leb_unmap failed!\n");
	}

	ret = ubi_peb_erase();

	if (0 == ret) {
		printk("ubi_peb_erase successfully!\n");
	} else {
		printk("ubi_peb_erase failed!\n");
	}

	uint8_t buf[32] = { 0 };
	for (size_t i = 0; i < 32; ++i) {
		buf[i] = (uint8_t)i;
	}

	is_mapped = false;
	ret = ubi_leb_is_mapped(lnum + 1, &is_mapped);

	if (false == is_mapped) {
		ret = ubi_leb_write(lnum + 1, buf, 32);

		if (0 == ret) {
			printk("ubi_leb_write successfully!\n");
		} else {
			printk("ubi_leb_write failed!\n");
		}
	}

	uint8_t output[32] = { 0 };
	size_t len = 0;

	ret = ubi_leb_read(lnum + 1, output, 0, &len);

	if (0 == ret) {
		printk("ubi_leb_read successfully!\n");
	} else {
		printk("ubi_leb_read failed!\n");
	}

	printk("LEB length = %u\n", len);
	for (size_t i = 0; i < len; ++i) {
		printk("%u ", output[i]);
	}

	return 0;
}
