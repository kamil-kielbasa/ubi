# Contributing

Thank you for your interest in contributing to UBI on Zephyr.

## Getting Started

1. Fork the repository and clone your fork.
2. Set up the development environment following [doc/environment_setup.md](doc/environment_setup.md).
3. Create a feature branch from `main`.

## Code Style

This project uses `clang-format` for consistent formatting. Run the formatter before committing:

```sh
./scripts/format.sh
```

The configuration is in `.clang-format` at the repository root.

## Building and Testing

Build and run the test suite on hardware:

```sh
west build -p --build-dir build/stm32u5/tests -b b_u585i_iot02a ./tests/
STM32_Programmer_CLI -c port=SWD -e all
STM32_Programmer_CLI -c port=SWD -d ./build/stm32u5/tests/zephyr/zephyr.hex
picocom -b 115200 /dev/ttyACM0
```

All tests must pass before submitting a pull request.

## Pull Request Process

1. Keep changes focused — one feature or fix per PR.
2. Write clear commit messages describing what changed and why.
3. Update documentation if your change affects the public API or behavior.
4. Ensure the test suite passes.
5. Open a pull request against `main`.

## Project Structure

```
lib/
  include/ubi.h       Public API
  src/ubi.c           Core implementation
  src/ubi_utils.h     Internal headers and constants
  src/ubi_utils.c     Low-level flash I/O and header management
sample/               Example application
tests/                Hardware integration tests (ZTest)
doc/                  Documentation
```
