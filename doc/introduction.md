# Introduction

**What this page covers:** Why UBI exists, how it compares to other Zephyr storage options, what it provides, what it does not, and its resource footprint.

**Prerequisites:** [Overview](overview.md) for the mental model.

## What is UBI?

UBI (Unsorted Block Images) is a volume management layer for raw flash devices on Zephyr RTOS. It maps Logical Erase Blocks (LEBs) to Physical Erase Blocks (PEBs), solving three fundamental problems of raw flash:

1. **Wear-leveling** — distributes writes across all PEBs so no single block wears out prematurely.
2. **Bad block management** — detects and isolates failed blocks transparently.
3. **Logical volumes** — partitions a single flash region into multiple named volumes, each independently readable, writable, and resizable.

UBI is analogous to the Logical Volume Manager (LVM) in Linux, but operates on erase blocks instead of sectors.

## Why UBI on Zephyr?

Zephyr provides several flash abstractions, but none offer a general-purpose volume manager with wear-leveling for raw flash:

| Existing Solution | What It Does | What It Lacks |
|-------------------|--------------|---------------|
| Flash Map (flash_area) | Maps named partitions to fixed flash regions | No wear-leveling, no volumes, static layout |
| NVS | Key-value store with wear-leveling | Single key-value namespace, not a volume manager |
| LittleFS | Filesystem with wear-leveling | File-grained, heavier footprint, no raw block access |
| FCB | Flash circular buffer | Append-only, no random-access volumes |

UBI fills this gap as a **thin, low-overhead volume manager** providing:

- Multiple named volumes on a single flash partition
- Transparent wear-leveling across all volumes
- Raw block-level read/write — ideal for firmware images, configuration blobs, or structured binary data
- Bad block isolation without application awareness

## Features

- Dynamic volume creation, removal, and resizing (dynamic volumes)
- Global wear-leveling across the entire flash partition
- Transparent bad block detection and isolation
- Dual-bank metadata headers for crash resilience (configurable 2–4 reserved PEB copies)
- Crash recovery via sequence-number-based conflict resolution
- Thread-safe operations via per-device Zephyr mutex
- Heap-allocated device and volume state; a small static partition guard (mutex + bitfield) enforces one open handle per `partition_id`

## Non-Goals

UBI intentionally does **not** provide:

| Non-goal | Rationale |
|----------|-----------|
| Filesystem (files, directories, POSIX API) | UBI is a block-level volume manager. Use LittleFS or FAT on top if you need a filesystem. |
| FTL replacement for eMMC / SD | Managed flash has its own translation layer. UBI adds no value. |
| Power-loss atomicity for user data | UBI protects metadata (dual-bank + sqnum). User data writes are not journaled — a power loss mid-write may leave a LEB partially written. |
| Encryption | Planned as an optional `CONFIG_UBI_CRYPTO` layer (AES-128-CCM). See the [Roadmap](roadmap.md). |


## Resource Usage

UBI is designed for resource-constrained embedded systems. The following measurements were taken with `west build -b b_u585i_iot02a ./sample` (STM32U5, Cortex-M33): `CONFIG_UBI_ENABLE=y`, `CONFIG_SIZE_OPTIMIZATIONS=y`, and no test-only options. Library footprint comes from `arm-none-eabi-size build/stm32u5/sample/modules/ubi/lib/lib..__ubi__lib.a` (sum of `.text` + `.data` for flash, `.data` + `.bss` for static RAM in that archive). The CI pipeline also records flash usage (see the `flash-usage` build artifact). Actual numbers vary with board, toolchain, and Kconfig.

### Flash and Static RAM

| Metric     | Value    | Notes |
|------------|----------|-------|
| Flash      | 8,522 B  | Sample app / production-style Kconfig; `.text` + `.data` in `lib..__ubi__lib.a` |
| Static RAM | 24 B     | Partition guard (`ubi_partition_guard.c`): `.data` + `.bss` in the same archive; device/volume state remains heap-allocated |

Enabling `CONFIG_UBI_TEST_API_ENABLE` (Ztest builds) pulls in extra code paths and logging; the same archive on the `tests/` app was approximately **16.2 KiB** flash (`.text` + `.data` only) with `CONFIG_DEBUG_OPTIMIZATIONS=y`.

### Runtime RAM (Dynamic Allocations)

| Runtime Object             | RAM per instance |
|----------------------------|------------------|
| Device (`ubi_device`)      | 112 B            |
| Volume (`ubi_volume`)      | 48 B             |
| PEB (free/dirty/mapped)    | 16 B             |
| Bad PEB                    | 12 B             |

All allocations are dynamic (`k_malloc`). Runtime RAM is proportional to the number of PEBs and volumes.

### Example: Typical Deployment

For a device with 16 PEBs (8 KB erase blocks, 128 KB partition) and 2 volumes:

- Device: 112 B
- PEB tracking: 14 data PEBs x 16 B = 224 B
- Volumes: 2 x 48 B = 96 B
- **Total runtime RAM: ~432 B**
