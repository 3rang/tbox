<!--
One idea per PR. A PR that needs a paragraph to explain its scope should be
split. See CONTRIBUTING.md for the gate this must pass.
-->

## What

<!-- One or two sentences. The behavior, not the file list. -->

## Why

<!-- The problem this solves. Link the issue if there is one. -->

## How

<!-- The approach, and any deliberate trade-off. -->

## Verification

<!-- Be specific - "green on Windows + WSL" is useful here. -->

- [ ] `scripts\clean-configure.ps1` (Windows) — build + ctest
- [ ] `cmake --build build && ctest --test-dir build` (Linux/macOS)
- [ ] `tbox selftest` green
- [ ] New behavior has a test in `tests/test_*.c` (no CMake edit needed)
- [ ] No TDLib, network or secrets in the tests
- [ ] `CHANGELOG.md` updated under `Unreleased`
- [ ] README updated if a command, flag, layout or build option changed

## Risk

<!-- What could this break, and what would you check if it did? -->

- [ ] Data loss (files, sessions, keys)
- [ ] Session / auth behavior
- [ ] Build portability (Windows ↔ POSIX)
- [ ] Public wire format (`status.json`, caption schema)