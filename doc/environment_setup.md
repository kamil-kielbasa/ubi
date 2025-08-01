# 🛠️ Zephyr + UBI: Hardware Setup & Build Guide (STM32U5)

This guide walks you through environment setup, building Zephyr projects, flashing the STM32U5 board, and viewing logs via UART. It’s designed for working with **Unsorted Block Images (UBI)** on Zephyr with the `b_u585i_iot02a` board.

---

## 📦 1. Environment Setup

Initialize and update the Zephyr workspace:

```sh
west init -l .
west update --narrow -o=--depth=1
```

---

## 🎨 2. Code Formatting (Optional)

Use `clang-format` script to ensure consistent code style:

```sh
./scripts/format.sh
```

---

## 🏗️ 3. Build Zephyr Projects

Build the **sample** and **test** applications for the STM32U5 board:

```sh
# Build the sample application
west build -p --build-dir build/stm32u5/sample -b b_u585i_iot02a ./sample/

# Build the test application
west build -p --build-dir build/stm32u5/tests -b b_u585i_iot02a ./tests/
```

---

## 🔄 4. Erase Flash Memory

Erase all flash contents using STM32CubeProgrammer CLI:

```sh
STM32_Programmer_CLI -c port=SWD -e all
```

---

## 🚀 5. Flash the Board

Flash the compiled Zephyr applications:

```sh
# Flash the sample app
STM32_Programmer_CLI -c port=SWD -d ./build/stm32u5/sample/zephyr/zephyr.hex

# Flash the test app
STM32_Programmer_CLI -c port=SWD -d ./build/stm32u5/tests/zephyr/zephyr.hex
```

---

## 🖥️ 6. View Console Output

Open a serial terminal to see log output:

```sh
picocom -b 115200 /dev/ttyACM0
```

> ⚠️ Make sure your user has permission to access `/dev/ttyACM0`. 