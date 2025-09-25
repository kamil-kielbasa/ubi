/**
 * \file    main.c
 * \author  Kamil Kielbasa
 * \brief   Sample for Unsorted Block Images (UBI) implementation.
 * \version 0.5
 * \date    2025-09-25
 *
 * \copyright Copyright (c) 2025
 *
 */

/* Include files ----------------------------------------------------------- */

#include <ubi.h>

/* Zephyr headers: */
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

/* Module defines ---------------------------------------------------------- */

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)

/* Module types and type definitiones -------------------------------------- */
/* Module interface variables and constants -------------------------------- */
/* Static variables and constants ------------------------------------------ */
/* Static function declarations -------------------------------------------- */
/* Static function definitions --------------------------------------------- */
/* Module interface function definitions ----------------------------------- */

int main(void)
{
	int ret = -1;

	printk("Hello world zephyr-ubi sample!\n");

	const struct device *flash_dev = UBI_PARTITION_DEVICE;
	struct flash_pages_info page_info = { 0 };

	ret = flash_get_page_info_by_offs(flash_dev, 0, &page_info);

	if (0 != ret)
		printk("Get page info failure\n");

	const size_t write_block_size = flash_get_write_block_size(flash_dev);
	const size_t erase_block_size = page_info.size;

	struct ubi_mtd mtd = { 0 };
	mtd.partition_id = FIXED_PARTITION_ID(UBI_PARTITION_NAME);
	mtd.erase_block_size = erase_block_size;
	mtd.write_block_size = write_block_size;

	struct ubi_device *ubi = NULL;
	ret = ubi_device_init(&mtd, &ubi);

	if (0 != ret)
		printk("UBI initialization failure\n");

	ret = ubi_device_deinit(ubi);

	if (0 != ret)
		printk("UBI deinitialization failure\n");

	return 0;
}
