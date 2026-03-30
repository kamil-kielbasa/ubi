# Configuration

UBI is configured via Zephyr's Kconfig system and DeviceTree overlays.

## Kconfig Options

Enable UBI and its options in your `prj.conf`:

```
CONFIG_UBI_ENABLE=y
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `CONFIG_UBI_ENABLE` | bool | n | Enable the UBI subsystem |
| `CONFIG_UBI_DEV_HDR_NR_OF_RES_PEBS` | int | 2 | Reserved PEBs for device/volume headers (range 2–4) |
| `CONFIG_UBI_MAX_NR_OF_VOLUMES` | int | 10 | Maximum number of volumes per device |
| `CONFIG_UBI_PEB_WRITE_RETRY_COUNT` | int | 3 | Flash write retries on data PEBs before failure (range 1–5) |
| `CONFIG_UBI_BAD_PEB_TORTURE_CYCLES` | int | 3 | Bad PEBs tortured per `erase_peb()` call (range 1–10) |
| `CONFIG_UBI_BAD_PEB_TORTURE_MAX_PER_ERASE` | int | 1 | Max erase attempts per bad PEB during torture (range 1–10) |
| `CONFIG_UBI_LOG_LEVEL_*` | choice | INF | Log verbosity: OFF, ERR, WRN, INF, DBG |
| `CONFIG_UBI_TEST_API_ENABLE` | bool | n | Enable test-only APIs (exposes `ubi_device_get_peb_ec`) |

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

- **Minimum**: At least 4 PEBs (2 reserved + 2 data). In practice, use ≥ 8 PEBs.
- **Reserved PEBs**: PEB 0 and PEB 1 are always reserved for device/volume metadata. Only PEBs 2..N-1 are available for volume data.
- **LEB size**: `erase_block_size - 48` bytes (48 bytes are consumed by EC + VID headers on each data PEB).

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
