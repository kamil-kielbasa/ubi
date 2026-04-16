# Contributing

Thank you for your interest in contributing to UBI on Zephyr.

For the full contributor guide — including build instructions, testing workflow, repository layout, and PR expectations — see the [Contributing](https://kamil-kielbasa.github.io/ubi/contributing.html) page in the documentation (source: [`doc/contributing.md`](doc/contributing.md)).

## Quick Reference

```sh
# Build and run tests (native_sim, plain)
./scripts/run_tests.sh

# Build and run tests (native_sim, secure)
./scripts/run_tests.sh native_sim secure

# Build and run tests (native_sim, chunked)
./scripts/run_tests.sh native_sim chunked

# Generate coverage (plain / secure)
./scripts/coverage.sh plain
./scripts/coverage.sh secure

# Format code
./scripts/format.sh

# Check formatting (CI mode)
./scripts/format.sh --check

# Forensic flash scan (after running secure tests with FLASH_SIMULATOR)
python3 scripts/scan_flash.py flash.bin

# Check test docblocks (brief/details/expected)
python3 scripts/check_test_descriptions.py tests/src/secure/

# Build docs
make -C doc html
```

All tests must pass and documentation must stay in sync with code changes before submitting a pull request.
