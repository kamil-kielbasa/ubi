# Contributing

Thank you for your interest in contributing to UBI on Zephyr.

## Getting Started

1. Fork the repository and clone your fork.
2. Set up the development environment following [Getting Started](getting_started.md).
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
west build -p --build-dir build/native_sim/tests -b native_sim ./tests/
./build/native_sim/tests/zephyr/zephyr.exe
```

Build the sample application:

```sh
west build -p --build-dir build/native_sim/sample -b native_sim ./sample/
./build/native_sim/sample/zephyr/zephyr.exe
```

Generate a coverage report:

```sh
bash scripts/coverage.sh
# Report: build/coverage/html/index.html
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

The configuration is in `.clang-format` at the repository root.

## Repository Layout

```
lib/
  include/ubi.h              Public API (all structures and function declarations)
  src/
    ubi_core_init.c          Device initialization — format, scan, mount
    ubi_core_runtime.c       Device runtime — get_info, erase_peb, deinit, test API
    ubi_volume.c             Volume management — create, resize, remove, get_info
    ubi_leb.c                LEB operations — read, write, map, unmap, is_mapped, get_size
    ubi_cache.c              Red-black tree comparator and search helpers
    ubi_io_metadata.c        Metadata I/O — device and volume header read/write
    ubi_io_data.c            Data I/O — EC/VID header and LEB data read/write
    ubi_flash_res_peb.c      Reserved PEB scanning, recovery, overwrite, and commit
    ubi_internal.h           Shared internal types (ubi_device, ubi_volume) and helpers
    ubi_cache.h              RBT and linked-list item types
    ubi_io.h                 On-flash header structures and constants
    ubi_flash_res_peb.h      Reserved PEB state types and API declarations
sample/                      Example application (full init/create/write/read/deinit cycle)
tests/                       Integration tests (ZTest, native_sim + b_u585i_iot02a)
doc/                         Sphinx documentation sources
scripts/                     CI, coverage, formatting, and test runner scripts
```

## Testing Expectations

| When | What to run |
|------|-------------|
| Every change | `west build` + run tests on `native_sim` |
| API or logic changes | `bash scripts/coverage.sh` — verify line >= 80%, branch >= 70% |
| New test cases | Add to the appropriate `tests/src/tests_ubi_*.c` file |
| Cross-compilation check | `west build -b b_u585i_iot02a ./tests/` and `west build -b nrf5340dk/nrf5340/cpuapp ./tests/` (build only) |
| Stress / torture tests | Only on `native_sim` with `CONFIG_FLASH_SIMULATOR=y` |

## Documentation Expectations

Any change that affects the following **must** include a documentation update:

- Public API (`lib/include/ubi.h`) — update Doxygen comments and `doc/api.rst` if needed
- On-flash layout or header structures — update `doc/architecture.md`
- Kconfig options — update `doc/configuration.md`
- Recovery semantics or failure modes — update `doc/architecture.md`
- Flash/RAM footprint — update `doc/introduction.md` (single source of truth for resource profile)
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
