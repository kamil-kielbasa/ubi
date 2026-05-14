## What changed

<!-- One paragraph: what does this PR do? -->

## Why

<!-- Motivation. Link issue if any. -->

## How tested

<!-- Boards, commands, scenarios. -->

## PR Checklist

- [ ] Tests pass on `native_sim`
- [ ] Coverage targets met (line >= 80%, branch >= 70%)
- [ ] Code formatted with `./scripts/format.sh`
- [ ] Documentation updated for any API/architecture/config changes
- [ ] CHANGELOG entry added under `[Unreleased]`
- [ ] No new compiler warnings (`-Werror -Wextra -Wshadow`)
- [ ] Secure tests include `\brief`, `\details`, `\expected` docblocks
      (and, where applicable, the optional `\oracle`, `\trace`,
      `\precondition` tags — see [Test Strategy](../doc/project/test_strategy.md))
- [ ] Forensic scan passes on secure flash image (`scripts/scan_flash.py`)
