# Contributing

Thank you for your interest in contributing to UBI on Zephyr.

## Getting Started

1. Fork the repository and clone your fork.
2. Set up the development environment following [Quick Start](quick_start.md) and the build/test/coverage instructions in [Test Strategy](test_strategy.md).
3. Create a feature branch from `main`.

## Development Prerequisites

| Tool | Purpose | Install |
|------|---------|---------|
| [west](https://docs.zephyrproject.org/latest/develop/west/index.html) | Zephyr meta-tool | `pip install west` |
| [Zephyr SDK](https://docs.zephyrproject.org/latest/develop/toolchains/zephyr_sdk.html) | Cross-compilation toolchain | See Zephyr docs |
| Python 3 + pip | Documentation build (`sphinx`, `breathe`, `myst-parser`) | `pip install -r doc/requirements.txt` |
| Doxygen | API reference generation | `sudo apt install doxygen` |

## Fast Local Loop

Build and run the full test suite on the simulator:

```sh
# Plain mode (default)
./scripts/run_tests.sh

# Secure mode
./scripts/run_tests.sh native_sim secure

# Chunked mode
./scripts/run_tests.sh native_sim chunked
```

Build the sample application:

```sh
west build -p --build-dir build/native_sim/sample -b native_sim ./sample/
./build/native_sim/sample/zephyr/zephyr.exe
```

Generate a coverage report:

```sh
# Plain coverage
bash scripts/coverage.sh plain
# Report: build/coverage-plain/html/index.html

# Secure coverage
bash scripts/coverage.sh secure
# Report: build/coverage-secure/html/index.html
```

Build documentation locally:

```sh
make -C doc html
# Output: doc/_build/html/index.html
```

## Code Style

This project uses `clang-format` for consistent formatting. Run the formatter before committing:

```sh
./scripts/format.sh
```

Use `--check` mode for CI-style dry-run:

```sh
./scripts/format.sh --check
```

The configuration is in `.clang-format` at the repository root.

## Repository Layout

```
lib/
  include/
    ubi.h                    Public API (all structures and function declarations)
    ubi_crypto.h             Secure backend public types and callbacks
    ubi_test.h               Test API (fault injection, shutdown hooks)
  src/
    ubi.c                    Unified init — dispatches to plain or secure backend
    common/                  Shared infrastructure
      ubi_backend.h          Backend-ops vtable definition
      ubi_cache.c/.h         Red-black tree comparator and search helpers
      ubi_internal.h         Shared internal types (ubi_device, ubi_volume)
      ubi_mem.c/.h           Memory abstraction (static slab / heap)
      ubi_partition_guard.c  Single-handle-per-partition guard
    plain/                   Plain (unencrypted) backend
      ubi_core_init.c        Device init — format, scan, mount
      ubi_core_runtime.c     Runtime — get_info, erase_peb, deinit
      ubi_volume.c           Volume management
      ubi_leb.c              LEB operations
      ubi_io_metadata.c      Metadata I/O (device and volume headers)
      ubi_io_data.c          Data I/O (EC/VID header and LEB data)
      ubi_flash_res_peb.c    Reserved PEB scanning, recovery, commit
    secure/                  Secure (AES-128-CCM encrypted) backend
      ubi_core_init.c        Secure device init — mode detect, format, attach
      ubi_secure_runtime.c   Secure runtime — erase, reclaim, anchor rewrite
      ubi_secure_volume.c    Secure volume ops — create, resize, remove, anchors
      ubi_secure_leb.c       Secure LEB read/write (single-tag and chunked)
      ubi_secure_io.c        Authenticated EC/VID/LEB I/O
      ubi_secure_reserved.c  Encrypted reserved PEB management
      ubi_secure_crypto.c    Key derivation, AEAD, salt, zeroization
      ubi_secure_ser.c       CBOR-like serialization for secure metadata
sample/                      Example application (full init/create/write/read/deinit cycle)
tests/                       Integration tests (ZTest, native_sim + b_u585i_iot02a + nrf5340dk)
doc/                         Sphinx documentation sources
scripts/                     CI, coverage, formatting, forensic scan, and test runner scripts
```

## Testing Expectations

| When | What to run |
|------|-------------|
| Every change | `west build` + run tests on `native_sim` |
| API or logic changes | `bash scripts/coverage.sh` — verify line >= 80%, branch >= 70% |
| New test cases | Add to the appropriate `tests/src/tests_ubi_*.c` file |
| Cross-compilation check | `west build -b b_u585i_iot02a ./tests/` and `west build -b nrf5340dk/nrf5340/cpuapp ./tests/` (build only) |
| Stress / torture tests | Only on `native_sim` with `CONFIG_FLASH_SIMULATOR=y` |
| Secure forensic scan | `python3 scripts/scan_flash.py flash.bin` after secure test run |
| Test docblock check | `python3 scripts/check_test_descriptions.py tests/src/secure/` |

## Documentation Expectations

Any change that affects the following **must** include a documentation update:

- Public API (`lib/include/ubi.h`) — update Doxygen comments and `doc/api.rst` if needed
- On-flash layout or header structures — update `doc/plain_architecture.md` (or `doc/onflash_format_spec.md` for SECURE mode)
- Kconfig options — update `doc/configuration.md`
- Recovery semantics or failure modes — update `doc/plain_architecture.md` (or `doc/onflash_format_spec.md` for SECURE mode)
- Flash/RAM footprint — update `doc/plain_architecture.md` (single source of truth for resource profile)
- Test matrix — update `doc/test_strategy.md`

## Pull Request Process

1. Keep changes focused — one feature or fix per PR.
2. Write clear commit messages describing what changed and why.
3. Update documentation if your change affects the public API, architecture, or behavior.
4. Ensure the test suite passes (`bash scripts/run_tests.sh`).
5. Update `CHANGELOG.md` following [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) format.
6. Open a pull request against `main`.

### PR Checklist

- [ ] Tests pass on `native_sim`
- [ ] Coverage targets met (line >= 80%, branch >= 70%)
- [ ] Code formatted with `./scripts/format.sh`
- [ ] Documentation updated for any API/architecture/config changes
- [ ] CHANGELOG entry added under `[Unreleased]`
- [ ] No new compiler warnings (`-Werror -Wextra -Wshadow`)
- [ ] Secure tests include `\brief`, `\details`, `\expected` docblocks
- [ ] Forensic scan passes on secure flash image (`scripts/scan_flash.py`)
