# Introduction

## What is UBI?

UBI (Unsorted Block Images) is a volume management layer for raw flash devices. It sits between the application (or a filesystem) and the raw flash hardware, solving three fundamental problems:

1. **Wear-leveling** — flash memory cells degrade after a finite number of erase cycles. UBI distributes writes evenly across all physical erase blocks so no single block wears out prematurely.
2. **Bad block management** — flash blocks can fail over the lifetime of the device. UBI detects and isolates bad blocks transparently.
3. **Logical volumes** — UBI allows partitioning a single flash region into multiple named logical volumes, each independently readable, writable, and resizable.

UBI can be compared to the Logical Volume Manager (LVM) in Linux. Whereas LVM maps logical sectors to physical sectors, UBI maps Logical Erase Blocks (LEBs) to Physical Erase Blocks (PEBs).

## Why UBI on Zephyr?

Zephyr RTOS provides flash abstractions ([Flash Map API](https://docs.zephyrproject.org/latest/services/storage/flash_map/flash_map.html), [NVS](https://docs.zephyrproject.org/latest/services/storage/nvs/nvs.html), [LittleFS](https://docs.zephyrproject.org/latest/services/file_system/index.html)), but none of them offer a general-purpose **volume manager with wear-leveling for raw flash**:

| Existing Solution | What It Does | What It Lacks |
|-------------------|--------------|---------------|
| Flash Map (flash_area) | Maps named partitions to fixed flash regions | No wear-leveling, no volumes, static layout |
| NVS | Key-value store with wear-leveling | Single key-value namespace, not a volume manager |
| LittleFS | Filesystem with wear-leveling | File-grained, heavier footprint, no raw block access |
| FCB | Flash circular buffer | Append-only, no random-access volumes |

UBI fills this gap. It provides a **thin, low-overhead volume manager** that gives applications:

- Multiple named volumes on a single flash partition
- Transparent wear-leveling across all volumes
- Raw block-level read/write (not file-level), which is ideal for storing firmware images, configuration blobs, or structured binary data
- Bad block isolation without application awareness

## Features

- Dynamic volume creation, removal, and resizing (dynamic volumes)
- Global wear-leveling across the entire flash partition
- Transparent bad block detection and isolation
- Dual-bank metadata headers for crash resilience
- Thread-safe operations via Zephyr mutexes
- Zero static RAM usage

## Resource Usage

UBI is designed for resource-constrained embedded systems. The following measurements were taken on the `b_u585i_iot02a` (STM32U5, Cortex-M33) board with size optimization (`-Os`).
The CI pipeline measures flash usage on every push (see the `flash-usage` build artifact).

### Flash and Static RAM

| Metric     | Value    |
|------------|----------|
| Flash      | 6,876 B  |
| Static RAM | 0 B      |

### Runtime RAM (Dynamic Allocations)

| Runtime Object             | RAM per instance |
|----------------------------|------------------|
| Device (`ubi_device`)      | 112 B            |
| Volume (`ubi_volume`)      | 48 B             |
| PEB (free/dirty/mapped)    | 16 B             |
| Bad PEB                    | 12 B             |

All allocations are dynamic (`k_malloc`). Static RAM usage is zero — UBI does not declare any static variables.

### Example: Typical Deployment

For a device with 16 PEBs and 2 volumes:

- Device: 112 B
- PEB tracking: 14 data PEBs × 16 B = 224 B
- Volumes: 2 × 48 B = 96 B
- **Total runtime RAM: ~432 B**
