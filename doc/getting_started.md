# Getting Started

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

Error handling is omitted for brevity. All API functions return `0` on success or a negative `errno` code on failure. See the [sample/](https://github.com/kamil-kielbasa/ubi/tree/main/sample) directory for a complete buildable example.

## Build Instructions

### Prerequisites

| Tool | Purpose | Install |
|------|---------|---------|
| [west](https://docs.zephyrproject.org/latest/develop/west/index.html) | Zephyr meta-tool (build, flash, manage manifests) | `pip install west` |
| [Zephyr SDK](https://docs.zephyrproject.org/latest/develop/toolchains/zephyr_sdk.html) | Cross-compilation toolchain | See Zephyr docs |
| [STM32CubeProgrammer CLI](https://www.st.com/en/development-tools/stm32cubeprog.html) | Flash erase and programming (hardware only) | ST website |
| [picocom](https://github.com/npat-efault/picocom) | Serial terminal for UART output (hardware only) | `sudo apt install picocom` |

### Initialize Workspace

```sh
west init -l .
west update --narrow -o=--depth=1
```

### Build for native_sim (Simulator)

Build and run the **test** suite:

```sh
west build -p --build-dir build/native_sim/tests -b native_sim ./tests/
./build/native_sim/tests/zephyr/zephyr.exe
```

Build and run the **sample** application:

```sh
west build -p --build-dir build/native_sim/sample -b native_sim ./sample/
./build/native_sim/sample/zephyr/zephyr.exe
```

### Build for STM32U5 (Hardware)

Build the test application:

```sh
west build -p --build-dir build/stm32u5/tests -b b_u585i_iot02a ./tests/
```

Erase flash and program:

```sh
STM32_Programmer_CLI -c port=SWD -e all
STM32_Programmer_CLI -c port=SWD -d ./build/stm32u5/tests/zephyr/zephyr.hex
```

Open a serial terminal to view output:

```sh
picocom -b 115200 /dev/ttyACM0
```

## Running Tests

All 111 tests run on `native_sim`:

```sh
bash scripts/run_tests.sh
```

## Code Coverage

Generate an HTML coverage report:

```sh
bash scripts/coverage.sh
```

The report is written to `build/coverage/html/index.html`.

### Coverage Targets

| Metric          | Target |
|-----------------|--------|
| Line coverage   | ≥ 80%  |
| Branch coverage | ≥ 70%  |

## Code Formatting

Apply the project's `.clang-format` rules:

```sh
./scripts/format.sh
```
