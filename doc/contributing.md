# Contributing

Thank you for your interest in contributing to UBI on Zephyr.

## Getting Started

1. Fork the repository and clone your fork.
2. Set up the development environment following [Getting Started](getting_started.md).
3. Create a feature branch from `main`.

## Code Style

This project uses `clang-format` for consistent formatting. Run the formatter before committing:

```sh
./scripts/format.sh
```

The configuration is in `.clang-format` at the repository root.

## Building and Testing

Build and run the test suite on the simulator:

```sh
west build -p --build-dir build/native_sim/tests -b native_sim ./tests/
./build/native_sim/tests/zephyr/zephyr.exe
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
tests/                Integration tests (ZTest, native_sim)
doc/                  Sphinx documentation
```
