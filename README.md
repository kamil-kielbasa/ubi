# UBI on Zephyr

![CI](https://github.com/kamil-kielbasa/ubi/actions/workflows/ci.yml/badge.svg)
[![Docs](https://img.shields.io/badge/docs-GitHub%20Pages-blue)](https://kamil-kielbasa.github.io/ubi/)
[![codecov](https://codecov.io/gh/kamil-kielbasa/ubi/graph/badge.svg)](https://codecov.io/gh/kamil-kielbasa/ubi)
![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)

An [Unsorted Block Images (UBI)](http://www.linux-mtd.infradead.org/doc/ubi.html) volume manager for [Zephyr RTOS](https://www.zephyrproject.org/).
It provides wear-leveling, bad block management, and multiple logical volumes on raw flash — using approximately **6.7 KB of flash** (Cortex-M33, `-Os`) and **zero static RAM**.

**Full documentation**: [kamil-kielbasa.github.io/ubi](https://kamil-kielbasa.github.io/ubi/)

## Quick Start

```c
#include <ubi.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

#define UBI_PARTITION_NAME ubi_partition
#define UBI_PARTITION_DEVICE FIXED_PARTITION_DEVICE(UBI_PARTITION_NAME)

int main(void)
{
    const struct device *flash_dev = UBI_PARTITION_DEVICE;
    struct flash_pages_info page_info = { 0 };
    flash_get_page_info_by_offs(flash_dev, 0, &page_info);

    struct ubi_mtd mtd = {
        .partition_id = FIXED_PARTITION_ID(UBI_PARTITION_NAME),
        .erase_block_size = page_info.size,
        .write_block_size = flash_get_write_block_size(flash_dev),
    };

    struct ubi_device *ubi = NULL;
    ubi_device_init(&mtd, &ubi);

    struct ubi_volume_config cfg = {
        .name = "my_vol",
        .type = UBI_VOLUME_TYPE_DYNAMIC,
        .leb_count = 4,
    };
    int vol_id;
    ubi_volume_create(ubi, &cfg, &vol_id);

    const char data[] = "Hello, UBI!";
    ubi_leb_write(ubi, vol_id, 0, data, sizeof(data));

    char buf[64];
    ubi_leb_read(ubi, vol_id, 0, 0, buf, sizeof(data));

    ubi_device_deinit(ubi);
    return 0;
}
```

Error handling is omitted for brevity. All API functions return `0` on success or a negative `errno` code on failure.

## License

MIT License. See [LICENSE](LICENSE) for details.

## Contact

Kamil Kielbasa — kamkie1996@gmail.com
