# Contributing

Thank you for your interest in contributing to UBI on Zephyr.

For the full contributor guide — including build instructions, testing workflow, repository layout, and PR expectations — see the [Contributing](https://kamil-kielbasa.github.io/ubi/contributing.html) page in the documentation (source: [`doc/contributing.md`](doc/contributing.md)).

## Quick Reference

```sh
# Build and run tests (native_sim)
west build -p --build-dir build/native_sim/tests -b native_sim ./tests/
./build/native_sim/tests/zephyr/zephyr.exe

# Build docs
make -C doc html

# Format code
./scripts/format.sh
```

All tests must pass and documentation must stay in sync with code changes before submitting a pull request.
