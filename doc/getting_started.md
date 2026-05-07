---
orphan: true
---

# Getting Started

```{note}
**Legacy page.** Content is being split into {doc}`quick_start` (hands-on
onboarding) and {doc}`test_strategy` (build / test / coverage tooling) as
part of the v1.0.0 documentation restructure (PR 3). This URL stays
reachable until the split lands.
```

**What this page covers:** How to build, run, and evaluate UBI — from first build to running all 325 tests (251 plain + 74 secure).

**Prerequisites:** A working [Zephyr development environment](https://docs.zephyrproject.org/latest/develop/getting_started/index.html) (west, Zephyr SDK).

## Before You Start

| Path | What it gives you |
|------|-------------------|
| **Quick evaluation** | Build for `native_sim`, run the sample, see UBI in action — no hardware needed. |
| **Project integration** | Add UBI as a west module, configure via Kconfig and DeviceTree. See [Configuration](configuration.md). |
| **Hardware testing** | Build for `b_u585i_iot02a` or `nrf5340dk/nrf5340/cpuapp`, flash, and observe via serial. Requires STM32CubeProgrammer or nrfjprog. |

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

    struct ubi_flash_desc flash = {
        .partition_id = FIXED_PARTITION_ID(UBI_PARTITION_NAME),
        .erase_block_size = page_info.size,
        .write_block_size = flash_get_write_block_size(flash_dev),
    };

    struct ubi_device *ubi = NULL;
    ubi_device_init(&flash, NULL, &ubi);

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

### Build for nRF5340 DK (Hardware)

Build the test application:

```sh
west build -p --build-dir build/nrf5340dk/tests -b nrf5340dk/nrf5340/cpuapp ./tests/
```

Flash:

```sh
west flash --build-dir build/nrf5340dk/tests
```

Open a serial terminal to view output:

```sh
picocom -b 115200 /dev/ttyACM0
```

## Running Tests

Tests run on `native_sim` in three modes:

```sh
# Plain tests (default)
bash scripts/run_tests.sh native_sim plain

# Secure tests
bash scripts/run_tests.sh native_sim secure

# Chunked secure tests
bash scripts/run_tests.sh native_sim chunked
```

## Code Coverage

Generate separate HTML coverage reports for plain and secure:

```sh
# Plain coverage
bash scripts/coverage.sh plain

# Secure coverage
bash scripts/coverage.sh secure
```

Reports are written to `build/coverage-plain/html/` and `build/coverage-secure/html/`.

## Forensic Scan

After running secure tests, scan the flash image for forbidden plaintext:

```sh
python3 scripts/scan_flash.py flash.bin
```

## Test Description Check

Verify every `ZTEST()` has `\brief`, `\details`, `\expected`:

```sh
python3 scripts/check_test_descriptions.py tests/src/
```

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

Dry-run check (mirrors the CI format-check job):

```sh
./scripts/format.sh --check
```
