# Environment Setup

Build, flash, and debug guide for UBI on Zephyr.

Supported boards:

| Board | Target | Notes |
|-------|--------|-------|
| `native_sim` | Host (x86) | Zephyr simulator — no hardware required |
| `b_u585i_iot02a` | STM32U585 (Cortex-M33) | ST B-U585I-IOT02A discovery kit |
| `nrf5340dk/nrf5340/cpuapp` | nRF5340 (Cortex-M33) | Nordic nRF5340 DK, application core |

## Prerequisites

| Tool | Purpose | Install |
|------|---------|---------|
| [west](https://docs.zephyrproject.org/latest/develop/west/index.html) | Zephyr meta-tool (build, flash, manage manifests) | `pip install west` |
| [Zephyr SDK](https://docs.zephyrproject.org/latest/develop/toolchains/zephyr_sdk.html) | Cross-compilation toolchain | See Zephyr docs |
| [STM32CubeProgrammer CLI](https://www.st.com/en/development-tools/stm32cubeprog.html) | Flash erase and programming (STM32 only) | ST website |
| [nRF Command Line Tools](https://www.nordicsemi.com/Products/Development-tools/nRF-Command-Line-Tools) | Flash and debug (nRF5340 only) | Nordic website |
| [picocom](https://github.com/npat-efault/picocom) | Serial terminal for UART output (hardware only) | `sudo apt install picocom` |
| [clang-format](https://clang.llvm.org/docs/ClangFormat.html) | Code formatting (optional) | `sudo apt install clang-format` |

## 1. Initialize Workspace

Set up the Zephyr west workspace and fetch dependencies:

```sh
west init -l .
west update --narrow -o=--depth=1
```

## 2. Code Formatting (Optional)

Apply the project's `.clang-format` rules to all source files:

```sh
./scripts/format.sh
```

## 3. Build

### 3.1 native_sim (simulator)

Build tests with the default **static** memory backend:

```sh
west build -p --build-dir build/native/tests -b native_sim ./tests/
```

Build tests with the **heap** memory backend:

```sh
west build -p --build-dir build/native/tests-heap -b native_sim ./tests/ \
  -- -DCONFIG_UBI_MEM_BACKEND_HEAP=y
```

Build the sample application:

```sh
west build -p --build-dir build/native/sample -b native_sim ./sample/
```

### 3.2 STM32U585 (b_u585i_iot02a)

Build tests with the default **static** memory backend:

```sh
west build -p --build-dir build/stm32u5/tests -b b_u585i_iot02a ./tests/
```

Build tests with the **heap** memory backend:

```sh
west build -p --build-dir build/stm32u5/tests-heap -b b_u585i_iot02a ./tests/ \
  -- -DCONFIG_UBI_MEM_BACKEND_HEAP=y
```

Build the sample application:

```sh
west build -p --build-dir build/stm32u5/sample -b b_u585i_iot02a ./sample/
```

### 3.3 nRF5340 DK (nrf5340dk/nrf5340/cpuapp)

Build tests with the default **static** memory backend:

```sh
west build -p --build-dir build/nrf5340dk/tests -b nrf5340dk/nrf5340/cpuapp ./tests/
```

Build tests with the **heap** memory backend:

```sh
west build -p --build-dir build/nrf5340dk/tests-heap -b nrf5340dk/nrf5340/cpuapp ./tests/ \
  -- -DCONFIG_UBI_MEM_BACKEND_HEAP=y
```

Build the sample application:

```sh
west build -p --build-dir build/nrf5340dk/sample -b nrf5340dk/nrf5340/cpuapp ./sample/
```

### 3.4 Memory Backends

UBI supports two memory backends selected via Kconfig (see [Configuration](configuration.md)):

| Backend | Kconfig | Description |
|---------|---------|-------------|
| Static (default) | `CONFIG_UBI_MEM_BACKEND_STATIC=y` | Fixed-size `k_mem_slab` pools sized at compile time |
| Heap | `CONFIG_UBI_MEM_BACKEND_HEAP=y` | `k_malloc` / `k_free` from the global Zephyr heap |

To switch backend pass `-DCONFIG_UBI_MEM_BACKEND_HEAP=y` on the `west build` command line (the static backend is the default and requires no extra flags).

## 4. Run Tests

### 4.1 native_sim

The simulator produces a host executable — run it directly:

```sh
./build/native/tests/zephyr/zephyr.exe
```

For the heap backend:

```sh
./build/native/tests-heap/zephyr/zephyr.exe
```

Test results are printed to `stdout`. Look for `PROJECT EXECUTION SUCCESSFUL` at the end.

### 4.2 STM32U585

Erase all flash contents before programming (required on first use or when switching builds):

```sh
STM32_Programmer_CLI -c port=SWD -e all
```

Flash and run:

```sh
STM32_Programmer_CLI -c port=SWD -d ./build/stm32u5/tests/zephyr/zephyr.hex
```

Open a serial terminal to observe test output:

```sh
picocom -b 115200 /dev/ttyACM0
```

If you get a "permission denied" error, add your user to the `dialout` group:

```sh
sudo usermod -aG dialout $USER
```

Then log out and back in for the change to take effect.

### 4.3 nRF5340 DK

Flash and run:

```sh
west flash --build-dir build/nrf5340dk/tests
```

Open a serial terminal to observe test output:

```sh
picocom -b 115200 /dev/ttyACM0
```

## 5. Flash Sample Application

Flash the **sample** application to STM32U585:

```sh
STM32_Programmer_CLI -c port=SWD -d ./build/stm32u5/sample/zephyr/zephyr.hex
```

Flash the **sample** application to nRF5340 DK:

```sh
west flash --build-dir build/nrf5340dk/sample
```

## 6. Measure Flash Usage

After building for the ARM target, measure the UBI library footprint:

```sh
arm-none-eabi-size build/stm32u5/tests/modules/ubi/lib/lib..__ubi__lib.a
```

For a per-section breakdown:

```sh
arm-none-eabi-size -A build/stm32u5/tests/modules/ubi/lib/lib..__ubi__lib.a
```

For the nRF5340 DK build:

```sh
arm-none-eabi-size build/nrf5340dk/tests/modules/ubi/lib/lib..__ubi__lib.a
arm-none-eabi-size -A build/nrf5340dk/tests/modules/ubi/lib/lib..__ubi__lib.a
```

The CI pipeline collects this measurement automatically for both boards (see the `flash-usage-b_u585i_iot02a` and `flash-usage-nrf5340dk_cpuapp` build artifacts).

## 7. Resource Reports

Generate flash (ROM) and static RAM usage reports for the whole firmware image:

```sh
west build -p --build-dir build/stm32u5/sample -b b_u585i_iot02a ./sample/ -t rom_report
west build -p --build-dir build/stm32u5/sample -b b_u585i_iot02a ./sample/ -t ram_report
```

UBI-only library numbers (partition size vs. code size) are documented in [Introduction](introduction.md): the DeviceTree `ubi_partition` used in this repo is **128 KiB** (STM32U5) / **64 KiB** (nRF5340); the measured **library** ROM footprint is **~9.2 KB** plain / **~59.3 KB** secure (Cortex-M33, `-Os`).
