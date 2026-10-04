# Contributing to tbox

Thanks for looking at this. The project is small and the rules are short —
they exist because each one was learned the hard way.

## The gate

Nothing merges until all of this is true:

```sh
# Windows
powershell -ExecutionPolicy Bypass -File scripts\clean-configure.ps1

# Linux / macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/tbox selftest
```

CI runs the same commands on Windows, Linux and macOS, plus ASan + UBSan on
Linux. Warnings are errors: **MSVC `/W4 /WX`**, **GCC `-Wall -Wextra -Werror`**.

## Rules that are not negotiable

**1. Tests stay headless.** No TDLib, no network, no secrets, no real data
directory. A test writes only to scratch paths and cleans up. If you need to
exercise something that requires Telegram, it belongs behind an injected
function pointer — that is how `auth` and `serve` keep `tboxcore` TDLib-free.

**2. Credentials are environment-only.** `TG_API_ID` / `TG_API_HASH`, read at
run time. Never add a config file, never compile a value in, never accept a
credential on the command line. One binary serves every account.

**3. Never build a path with `snprintf("%s/%s")`.** GCC's
`-Wformat-truncation` is part of `-Wall`, and this project builds with
`-Werror`, so it fails the build. Use `tbox_datadir_join()` or a length-checked
`memcpy`.

**4. POSIX files define `_POSIX_C_SOURCE 200809L` before any include**
(add `_DEFAULT_SOURCE` where glibc needs it for `flock`). MSVC files that use
bounded string functions need `_CRT_SECURE_NO_WARNINGS`. Both go *above* the
includes, or they do nothing.

**5. One header per abstraction, one native backend per OS.** Add
`foo.h` + `foo_win.c` + `foo_posix.c`; never `#ifdef` an OS decision through a
caller.

**6. Derive, never duplicate.** Paths are normalized through
`tbox_archive_name()`, data-dir paths through `tbox_datadir_*`, `--data`
through `tbox_cmd_parse_data`. A second copy of any of these is a bug waiting
to happen.

**7. A new `tests/test_*.c` needs no CMake edit** — `tests/CMakeLists.txt`
globs with `CONFIGURE_DEPENDS`. Add the file, add a test.

## What to include in a change

- The behavior **and** the test that proves it. `tbox selftest` is a good first
  home for a pure rule; a new file with an assert is better for anything else.
- A `CHANGELOG.md` entry under `Unreleased`.
- A README change if you changed a command, a flag, a file layout or a build
  option — that document is the only thing a new user reads.

## Commit and PR style

Short imperative subject (`serve: refuse a second instance`), body explaining
*why* when it is not obvious. Mention the platform you verified on; "green on
Windows + WSL" is useful information here.

A PR should be one idea. If it needs a paragraph to explain its scope, split it.

## Reporting a bug

Use the issue template. The three things that make a bug report actionable:

- the exact command and its output,
- the OS **and** whether the TDLib layer was present (`tbox --version` plus
  whether `tbox auth` says "not available in this build"),
- `tbox status` output and, if the archive is involved, the affected path.

**Never paste `TG_API_HASH` or your session files into an issue.**

## Cutting a release

Tag `vX.Y.Z` and push the tag; `.github/workflows/release.yml` builds, tests,
packages and publishes. Before tagging, make sure the version in
`CMakeLists.txt` **and** `tests/CMakeLists.txt` matches the tag — the workflow
refuses to publish if it does not, but that is a safety net, not a workflow.

## License

BSD 3-Clause. Vendored code keeps its own license (cJSON MIT, qrcodegen MIT,
TDLib Boost-1.0); do not edit `third_party/` — re-vendor instead
(`scripts/fetch-qrcodegen.ps1`).