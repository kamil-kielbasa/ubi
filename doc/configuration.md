# Configuration

**What this page covers:** All Kconfig options, DeviceTree partition setup, and sizing guidelines for UBI.

**Prerequisites:** [Overview](overview.md) and [Getting Started](getting_started.md).

UBI is configured via Zephyr's Kconfig system and DeviceTree overlays.

## Kconfig Options

Enable UBI and its options in your `prj.conf`:

```
CONFIG_UBI_ENABLE=y
```

### Option Reference

| Option | Type | Default | Range | Description |
|--------|------|---------|-------|-------------|
| `CONFIG_UBI_ENABLE` | bool | n | — | Enable the UBI subsystem |
| `CONFIG_UBI_MEM_BACKEND_STATIC` | bool | y | — | Use `k_mem_slab` pools for all UBI allocations (default) |
| `CONFIG_UBI_MEM_BACKEND_HEAP` | bool | n | — | Use `k_malloc`/`k_free` for all UBI allocations (legacy) |
| `CONFIG_UBI_MAX_NR_OF_DEVICES` | int | 1 | 1–4 | Maximum number of concurrent UBI device handles (static backend) |
| `CONFIG_UBI_MAX_NR_OF_DATA_PEBS` | int | 14 | 1–4096 | Maximum data PEBs per device (static backend pool sizing) |
| `CONFIG_UBI_MEM_STATS` | bool | n | — | Enable slab pool usage statistics (static backend only) |
| `CONFIG_UBI_DEV_HDR_NR_OF_RES_PEBS` | int | 2 | 2–4 | Number of reserved PEBs for device/volume metadata |
| `CONFIG_UBI_MAX_NR_OF_VOLUMES` | int | 10 | 1–128 | Maximum number of volumes per device |
| `CONFIG_UBI_PEB_WRITE_RETRY_COUNT` | int | 3 | 1–5 | Flash write retries on data PEBs before marking bad |
| `CONFIG_UBI_BAD_PEB_TORTURE_CYCLES` | int | 3 | 1–10 | Bad PEBs tortured per `erase_peb()` call |
| `CONFIG_UBI_BAD_PEB_TORTURE_MAX_PER_ERASE` | int | 1 | 1–10 | Max erase attempts per bad PEB during torture |
| `CONFIG_UBI_LOG_LEVEL_*` | choice | INF | — | Log verbosity: OFF, ERR, WRN, INF, DBG |
| `CONFIG_UBI_TEST_API_ENABLE` | bool | n | — | Enable test-only APIs (`ubi_device_get_peb_ec`, `ubi_device_check_invariants`) |
| `CONFIG_UBI_TEST_FAULT_INJECTION` | bool | n | — | Controllable allocation failure hook for simulating OOM. Depends on `CONFIG_UBI_TEST_API_ENABLE`. |

### Memory Backend

UBI supports two memory backends, selected at compile time via a Kconfig choice:

- **Static** (`CONFIG_UBI_MEM_BACKEND_STATIC`, default): All allocations use `k_mem_slab` pools sized at compile time. Provides deterministic RAM budget and full isolation from the application heap. Pool sizes are derived from `CONFIG_UBI_MAX_NR_OF_DEVICES`, `CONFIG_UBI_MAX_NR_OF_DATA_PEBS`, and `CONFIG_UBI_MAX_NR_OF_VOLUMES`.

- **Heap** (`CONFIG_UBI_MEM_BACKEND_HEAP`): Delegates to `k_malloc`/`k_free` (legacy behaviour). Useful for prototyping or when pool sizing is unknown.

When to use each:

| Use case | Recommended backend |
|----------|-------------------|
| Production deployment | STATIC — deterministic, no fragmentation |
| Prototyping / unknown flash geometry | HEAP — no pool sizing required |
| Multiple UBI devices | STATIC — set `CONFIG_UBI_MAX_NR_OF_DEVICES` |

### Memory Sizing Guide

Under the static backend, all pool memory is pre-allocated at compile time. The total RAM budget is:

| Pool | Formula | Example (D=14, V=2) |
|------|---------|---------------------|
| Device slab | `MAX_NR_OF_DEVICES × sizeof(ubi_device)` | 1 × 112 = 112 B |
| Volume slab | `MAX_NR_OF_DEVICES × MAX_NR_OF_VOLUMES × sizeof(ubi_volume)` | 1 × 10 × 48 = 480 B |
| Leaf slab | `MAX_NR_OF_DEVICES × (MAX_NR_OF_DATA_PEBS + MAX_NR_OF_VOLUMES) × 16` | 1 × (14+10) × 16 = 384 B |
| Scratch slab | `UBI_DEV_HDR_SIZE + MAX_NR_OF_VOLUMES × UBI_VOL_HDR_SIZE` (1 block) | 32 + 10 × 48 = 512 B |
| **Total** | | **~1,488 B** |

At init time, `ubi_device_init()` verifies that the actual flash geometry fits within the configured pools. If the flash partition has more data PEBs than `CONFIG_UBI_MAX_NR_OF_DATA_PEBS`, init returns `-ENOMEM`.

### Impact Analysis

| Option | Flash layout | RAM usage | Recovery / robustness | Performance |
|--------|-------------|-----------|----------------------|-------------|
| `MEM_BACKEND_STATIC` | No impact | Pre-allocated pools (known at compile time) | No fragmentation risk | O(1) slab alloc |
| `MEM_BACKEND_HEAP` | No impact | Dynamic; proportional to PEBs + volumes | Subject to heap fragmentation | k_malloc overhead |
| `MAX_NR_OF_DEVICES` | No impact | Multiplies all pool sizes | No impact | No impact |
| `MAX_NR_OF_DATA_PEBS` | No impact | 16 B per PEB in leaf slab | Init fails if flash has more data PEBs | No impact |
| `NR_OF_RES_PEBS` | More reserved PEBs = fewer data PEBs | +16 B per additional reserved PEB (RBT node) | Higher = more redundancy for metadata; 3–4 adds cold spares | No measurable impact |
| `MAX_NR_OF_VOLUMES` | Limits volume headers stored in reserved PEBs | +48 B per volume + 16 B per volume RBT node | No direct impact | No measurable impact |
| `PEB_WRITE_RETRY_COUNT` | No impact | No impact | Higher = more chances to recover from transient write errors | Higher = slower failure path (more retries) |
| `BAD_PEB_TORTURE_CYCLES` | No impact | No impact | Higher = more aggressive bad block recovery attempts | Higher = longer `erase_peb()` calls |
| `BAD_PEB_TORTURE_MAX_PER_ERASE` | No impact | No impact | Higher = more erase attempts per bad PEB | Higher = longer per-PEB torture |

### Log Level

UBI uses Zephyr's logging subsystem. Set the log level in `prj.conf`:

```
# Show only errors
CONFIG_UBI_LOG_LEVEL_ERR=y

# Debug everything
CONFIG_UBI_LOG_LEVEL_DBG=y
```

## Flash Partition (DeviceTree)

UBI requires a named flash partition defined in a DeviceTree overlay. The partition label is referenced at compile time via `FIXED_PARTITION_ID()` and `FIXED_PARTITION_DEVICE()`.

### native_sim (simulator)

```dts
&flash0 {
    erase-block-size = <8192>;

    partitions {
        compatible = "fixed-partitions";
        #address-cells = <1>;
        #size-cells = <1>;

        ubi_partition: partition@0 {
            label = "ubi_partition";
            reg = <0x00000000 DT_SIZE_K(128)>;
        };
    };
};
```

### b_u585i_iot02a (STM32U5)

```dts
&flash0 {
    partitions {
        compatible = "fixed-partitions";
        #address-cells = <1>;
        #size-cells = <1>;

        ubi_partition: partition@d0000 {
            label = "ubi_partition";
            reg = <0x000d0000 DT_SIZE_K(128)>;
        };
    };
};
```

### nrf5340dk/nrf5340/cpuapp (nRF5340 DK)

```dts
&flash0 {
    partitions {
        compatible = "fixed-partitions";
        #address-cells = <1>;
        #size-cells = <1>;

        ubi_partition: partition@f0000 {
            label = "ubi_partition";
            reg = <0x000f0000 DT_SIZE_K(64)>;
        };
    };
};
```

nRF5340 has 4 KB erase blocks (vs 8 KB on STM32U5). With 64 KB the partition yields 16 PEBs (2 reserved, 14 data), matching the default `CONFIG_UBI_MAX_NR_OF_DATA_PEBS`.

### Sizing Guidelines

- **Minimum**: At least `N + 2` PEBs (N reserved + 2 data), where N = `CONFIG_UBI_DEV_HDR_NR_OF_RES_PEBS`. In practice, use >= 8 PEBs.
- **Reserved PEBs**: The first N PEBs are reserved for device/volume metadata (configurable via `CONFIG_UBI_DEV_HDR_NR_OF_RES_PEBS`, default 2). Only PEBs N..total-1 are available for volume data.
- **LEB size**: `erase_block_size - 48` bytes (48 bytes are consumed by EC + VID headers on each data PEB).
- **Example (STM32U5)**: With 8 KB erase blocks and 128 KB partition: 16 total PEBs, 2 reserved, 14 data PEBs, each with 8144 B usable per LEB.
- **Example (nRF5340)**: With 4 KB erase blocks and 64 KB partition: 16 total PEBs, 2 reserved, 14 data PEBs, each with 4048 B usable per LEB.

## Zephyr Module Integration

UBI registers as a Zephyr module via `zephyr/module.yml`. When using west, add UBI as a project in your `west.yml`:

```yaml
manifest:
  projects:
    - name: ubi
      url: https://github.com/kamil-kielbasa/ubi
      revision: main
      path: module/lib/ubi
```

Then initialize and update:

```sh
west init -l .
west update --narrow -o=--depth=1
```
