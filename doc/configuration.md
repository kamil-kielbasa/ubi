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
| `CONFIG_UBI_DEV_HDR_NR_OF_RES_PEBS` | int | 2 | 2–4 | Number of reserved PEBs for device/volume metadata |
| `CONFIG_UBI_MAX_NR_OF_VOLUMES` | int | 10 | — | Maximum number of volumes per device |
| `CONFIG_UBI_PEB_WRITE_RETRY_COUNT` | int | 3 | 1–5 | Flash write retries on data PEBs before marking bad |
| `CONFIG_UBI_BAD_PEB_TORTURE_CYCLES` | int | 3 | 1–10 | Bad PEBs tortured per `erase_peb()` call |
| `CONFIG_UBI_BAD_PEB_TORTURE_MAX_PER_ERASE` | int | 1 | 1–10 | Max erase attempts per bad PEB during torture |
| `CONFIG_UBI_LOG_LEVEL_*` | choice | INF | — | Log verbosity: OFF, ERR, WRN, INF, DBG |
| `CONFIG_UBI_TEST_API_ENABLE` | bool | n | — | Enable test-only APIs (`ubi_device_get_peb_ec`) |

### Impact Analysis

| Option | Flash layout | RAM usage | Recovery / robustness | Performance |
|--------|-------------|-----------|----------------------|-------------|
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

### Sizing Guidelines

- **Minimum**: At least `N + 2` PEBs (N reserved + 2 data), where N = `CONFIG_UBI_DEV_HDR_NR_OF_RES_PEBS`. In practice, use >= 8 PEBs.
- **Reserved PEBs**: The first N PEBs are reserved for device/volume metadata (configurable via `CONFIG_UBI_DEV_HDR_NR_OF_RES_PEBS`, default 2). Only PEBs N..total-1 are available for volume data.
- **LEB size**: `erase_block_size - 48` bytes (48 bytes are consumed by EC + VID headers on each data PEB).
- **Example**: With 8 KB erase blocks and 128 KB partition: 16 total PEBs, 2 reserved, 14 data PEBs, each with 8144 B usable per LEB.

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
