# UBI on Zephyr

![CI](https://github.com/kamil-kielbasa/ubi/actions/workflows/ci.yml/badge.svg)
[![Docs](https://img.shields.io/badge/docs-GitHub%20Pages-blue)](https://kamil-kielbasa.github.io/ubi/)
[![codecov](https://codecov.io/gh/kamil-kielbasa/ubi/graph/badge.svg)](https://codecov.io/gh/kamil-kielbasa/ubi)
![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)

UBI is a lightweight **wear-leveling and logical volume management layer** for raw flash on [Zephyr RTOS](https://www.zephyrproject.org/). It sits between application-level storage logic and the physical flash device, providing logical eraseblocks, metadata redundancy, bad block handling, and crash-safe LEB-to-PEB mapping.

```
┌─────────────────────────────────────────┐
│          Application / Storage          │
├─────────────────────────────────────────┤
│              UBI Public API             │
│  volume mgmt ─ LEB I/O ─ wear-leveling  │
├─────────────────────────────────────────┤
│     Zephyr Flash Map (flash_area API)   │
├─────────────────────────────────────────┤
│        Physical Flash (NOR / NAND)      │
└─────────────────────────────────────────┘
```

## Key Properties

| Property | Value |
|----------|-------|
| Logical volumes | Multiple named volumes on a single flash partition |
| Wear-leveling | Greedy min-EC strategy across all PEBs |
| Bad block handling | Automatic detection and isolation |
| Metadata redundancy | Dual-bank reserved PEBs (configurable 2–4 copies) |
| Crash recovery | Sequence-number-based conflict resolution on init |
| Filesystem | **No** — raw block-level I/O, not file-level |
| Flash footprint | ~6.7 KB (Cortex-M33, `-Os`, default config) |
| Static RAM | 0 B |
| Runtime RAM | Proportional to PEB count + volume count (~432 B typical) |
| Thread safety | Per-device mutex; not ISR-safe |

## When to Use

- You need **logical volumes with wear-leveling** on raw NOR/NAND flash under Zephyr.
- You want to store firmware images, configuration blobs, or structured binary data with raw block access.
- You are building a higher-level storage layer and need a reliable block abstraction underneath.

## When NOT to Use

- You need a **ready-made filesystem** — use LittleFS or FAT instead.
- Your flash has a built-in **FTL** (eMMC, SD cards) — UBI adds no value.
- You only need a **key-value store** — Zephyr NVS is simpler and sufficient.
- Your device has **no flash wear concerns** (e.g., very low write frequency on high-endurance NOR).

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

Error handling is omitted for brevity. All API functions return `0` on success or a negative `errno` code on failure. See [`sample/`](sample/) for a complete buildable example.

## Documentation

| Document | What you will find |
|----------|--------------------|
| [Overview](https://kamil-kielbasa.github.io/ubi/overview.html) | Mental model, key concepts (PEB/LEB/EC/VID), how it works in 6 steps |
| [Introduction](https://kamil-kielbasa.github.io/ubi/introduction.html) | Why UBI on Zephyr, feature summary, resource usage profile |
| [Architecture](https://kamil-kielbasa.github.io/ubi/architecture.html) | On-flash layout, in-RAM structures, init flow, wear-leveling, dual-bank, recovery |
| [Getting Started](https://kamil-kielbasa.github.io/ubi/getting_started.html) | Build instructions, test suite, coverage, code formatting |
| [Configuration](https://kamil-kielbasa.github.io/ubi/configuration.html) | Kconfig options, DeviceTree overlays, sizing guidelines |
| [API Reference](https://kamil-kielbasa.github.io/ubi/api.html) | Auto-generated from Doxygen — all public types and functions |
| [Test Strategy](https://kamil-kielbasa.github.io/ubi/test_strategy.html) | Test categories, environments, coverage targets, known limitations |
| [Contributing](https://kamil-kielbasa.github.io/ubi/contributing.html) | Repository layout, local dev loop, PR checklist |

## Project Quality

| Metric | Value |
|--------|-------|
| Test suites | 17 suites, 228 tests |
| Line coverage | 85.2% (target ≥ 80%) |
| Branch coverage target | ≥ 70% |
| CI | GitHub Actions — build, test, coverage, cross-compile STM32U5 |
| Primary test platform | Zephyr `native_sim` with flash simulator |
| Hardware validation | `b_u585i_iot02a` (STM32U5 Cortex-M33) cross-compilation |

## Design Trade-offs

| UBI does | UBI does not |
|----------|--------------|
| Wear-leveling across all PEBs | Provide a filesystem (no files, directories, or POSIX API) |
| Bad block detection and isolation | Replace a hardware FTL (eMMC, SD) |
| Multiple named logical volumes | Offer encryption (crypto layer is planned, see [Roadmap](https://kamil-kielbasa.github.io/ubi/roadmap.html)) |
| Crash-safe metadata via dual-bank | Guarantee power-loss atomicity for user data writes |
| Dynamic volume resize | Shell commands for interactive device management (planned) |

## License

MIT License. See [LICENSE](LICENSE) for details.

## Contact

Kamil Kielbasa — kamkie1996@gmail.com
