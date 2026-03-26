# Environment Setup

Build, flash, and debug guide for UBI on Zephyr with the `b_u585i_iot02a` (STM32U5) board.

## Prerequisites

| Tool | Purpose | Install |
|------|---------|---------|
| [west](https://docs.zephyrproject.org/latest/develop/west/index.html) | Zephyr meta-tool (build, flash, manage manifests) | `pip install west` |
| [Zephyr SDK](https://docs.zephyrproject.org/latest/develop/toolchains/zephyr_sdk.html) | Cross-compilation toolchain | See Zephyr docs |
| [STM32CubeProgrammer CLI](https://www.st.com/en/development-tools/stm32cubeprog.html) | Flash erase and programming | ST website |
| [picocom](https://github.com/npat-efault/picocom) | Serial terminal for UART output | `sudo apt install picocom` |
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

Build the **test** application:

```sh
west build -p --build-dir build/stm32u5/tests -b b_u585i_iot02a ./tests/
```

Build the **sample** application:

```sh
west build -p --build-dir build/stm32u5/sample -b b_u585i_iot02a ./sample/
```

## 4. Erase Flash

Erase all flash contents before programming. This is required on first use or when switching between test and sample builds:

```sh
STM32_Programmer_CLI -c port=SWD -e all
```

## 5. Flash

Flash the **test** application:

```sh
STM32_Programmer_CLI -c port=SWD -d ./build/stm32u5/tests/zephyr/zephyr.hex
```

Flash the **sample** application:

```sh
STM32_Programmer_CLI -c port=SWD -d ./build/stm32u5/sample/zephyr/zephyr.hex
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

The CI pipeline also collects this measurement automatically (see the `flash-usage` build artifact).

## 7. Serial Console

Open a serial terminal to view log output:

```sh
picocom -b 115200 /dev/ttyACM0
```

If you get a "permission denied" error, add your user to the `dialout` group:

```sh
sudo usermod -aG dialout $USER
```

Then log out and back in for the change to take effect.

## 7. Resource Reports

Generate flash (ROM) and static RAM usage reports:

```sh
west build -p --build-dir build/stm32u5/sample -b b_u585i_iot02a ./sample/ -t rom_report
west build -p --build-dir build/stm32u5/sample -b b_u585i_iot02a ./sample/ -t ram_report
```
