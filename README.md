# UBI on Zephyr

An [Unsorted Block Images (UBI)](http://www.linux-mtd.infradead.org/doc/ubi.html) implementation for [Zephyr RTOS](https://www.zephyrproject.org/).

UBI is a volume management layer for raw flash devices. It maps logical erase blocks (LEBs) to physical erase blocks (PEBs), providing wear-leveling, bad block handling, and multiple logical volumes on a single flash partition — similar to what LVM does for block devices.

This is a from-scratch implementation targeting resource-constrained embedded systems running Zephyr. It requires approximately 2.8 KB of flash and zero static RAM.

## Features

- Dynamic volume creation, removal, and resizing (dynamic volumes)
- Global wear-leveling across the entire flash partition
- Transparent bad block detection and isolation
- Dual-bank metadata headers for crash resilience
- Thread-safe operations via Zephyr mutexes
- Zero static RAM usage

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

    /* Create a volume */
    struct ubi_volume_config cfg = {
        .name = "my_vol",
        .type = UBI_VOLUME_TYPE_DYNAMIC,
        .leb_count = 4,
    };
    int vol_id;
    ubi_volume_create(ubi, &cfg, &vol_id);

    /* Write and read data */
    const char data[] = "Hello, UBI!";
    ubi_leb_write(ubi, vol_id, 0, data, sizeof(data));

    char buf[64];
    ubi_leb_read(ubi, vol_id, 0, 0, buf, sizeof(data));

    ubi_device_deinit(ubi);
    return 0;
}
```

Error handling is omitted for brevity. All API functions return `0` on success or a negative `errno` code on failure. See [`sample/`](sample/) for a complete buildable example.

## Resource Usage

| Metric | Value (v0.5.0) |
|--------|----------------|
| Flash  | 2802 B         |
| Static RAM | 0 B        |

| Runtime Object | RAM per instance |
|----------------|------------------|
| Device         | 112 B            |
| Volume         | 48 B             |
| PEB (free/dirty/mapped) | 16 B   |
| Bad PEB        | 12 B             |

## Documentation

| Document | Description |
|----------|-------------|
| [Architecture Guide](doc/architecture.md) | Concepts, data structures, initialization flow, ASCII diagrams |
| [Environment Setup](doc/environment_setup.md) | Build, flash, and debug instructions for STM32U5 |
| [Roadmap](doc/roadmap.md) | Planned features and development priorities |
| [Changelog](CHANGELOG.md) | Version history with detailed change notes |
| [Contributing](CONTRIBUTING.md) | How to contribute to the project |

## License

MIT License. See [LICENSE](LICENSE) for details.

## Contact

Kamil Kielbasa — kamkie1996@gmail.com
