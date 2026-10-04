# tbox

**Expose your Telegram Saved Messages as an ordinary folder tree — over FTP, then SFTP.**
One binary, no daemon, no web UI, no cloud. C17 + TDLib, Windows / Linux / macOS.

```
# the goal, once the FTP transport lands (see Status below):
tbox serve
  → WinSCP:  ftp://tbox:<token>@127.0.0.1:2121/
```

---

## Status — read this first

This is an **active rewrite (v0.5)**. The table is the honest state of the code,
not a wish list.

| | What works today |
|---|---|
| ✅ | **QR login** — `tbox auth`, QR rendered in the terminal, 2FA prompt, session persisted |
| ✅ | **Session ownership** — `tbox serve` takes an exclusive OS-level lock; a second instance is refused |
| ✅ | **Status seam** — `tbox status` reads `status.json` only (never opens TDLib), detects stale owners by pid |
| ✅ | **Archive index core** — caption ledger, path normalization, implied directories, listing/sorting (fully unit-tested) |
| ✅ | **Scan loop** — Saved Messages → index, newest-first, paginated (`getChatHistory`), rebuilt on every change |
| ✅ | **FTP server, read** — loopback-only, token login, browse the tree, `LIST`/`SIZE`/`MDTM`/`RETR`, real bodies |
| ✅ | **`tbox selftest`** — 30 offline checks of the core rules |
| 🚧 | FTP write — `STOR`/`DELE`/`MKD`/`RNFR`/`RNTO` → Telegram upload (**slice F**, next) |
| 🚧 | SFTP (**slice G**), tray / service integration (**slice H**) |

**Consequence:** today `tbox serve` walks your Saved Messages, publishes the
folder tree that was already there, and serves it read-only over FTP on
loopback — point WinSCP or FileZilla at `127.0.0.1` and the token `serve`
printed, and the real v0.3 tree browses and downloads. Upload, SFTP and
tray/service integration are not built yet. Everything that talks to Telegram is
behind the `serve` command and lands there first.

The design principle that got us here: **the interesting logic never touches
TDLib.** The archive index, the path rules and the caption ledger are pure C,
so they are 100 % testable on both operating systems with no Telegram account,
no network and no secrets.

---

## Requirements

- **CMake ≥ 3.22**, a **C17** compiler, and a platform:
  - Windows — MSVC (Visual Studio Build Tools)
  - Linux — GCC 9+ or Clang 10+
  - macOS — Clang (Xcode Command Line Tools)
- **TDLib** — the prebuilt `libtdjson` shared library. Prebuilt Windows
  binaries are committed in `vendor/`; for Linux/macOS run
  `scripts/fetch-tdjson.sh` once. Without it everything still builds and all
  tests still run — only `tbox auth` is unavailable.
- **To log in:** Telegram API credentials from <https://my.telegram.org/apps>.

---

## Credentials — environment variables only

`TG_API_ID` and `TG_API_HASH` are the **only** source. There is no config file,
no `.env`, nothing compiled into the binary, and no keyring.

```powershell
# Windows (cmd)
set TG_API_ID=123456
set TG_API_HASH=0123456789abcdef0123456789abcdef

# Windows (PowerShell)
$env:TG_API_ID   = "123456"
$env:TG_API_HASH = "0123456789abcdef0123456789abcdef"
```

```sh
# Linux / macOS
export TG_API_ID=123456
export TG_API_HASH=0123456789abcdef0123456789abcdef
```

Both are validated at run time (id must be positive digits, hash exactly 32 hex
digits) and a missing or malformed value produces a specific error plus the
one-liner for your shell. Credentials are never logged, never written to disk
and never passed on a command line, so they cannot leak into a shell history or
a process listing.

Get them from <https://my.telegram.org/apps> → *API development tools*.

---

## Quick start

```sh
# 1. build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

# 2. prove the core rules on this machine
./build/tbox selftest

# 3. log in (QR shows in your terminal; scan it with Telegram on your phone)
export TG_API_ID=123456
export TG_API_HASH=0123456789abcdef0123456789abcdef
./build/tbox auth

# 4. check the service state (never opens TDLib)
./build/tbox status
```

`auth` is fast if a session already exists — it restores from the local TDLib
database and prints `Logged in as <user>` without showing a QR code.

On Windows use `scripts\clean-configure.ps1` instead of the `cmake` lines — it
finds Visual Studio on its own — and run `.\build\tbox.exe` rather than
`./build/tbox`.

---

## Commands

```
tbox <command> [args]

  auth [--data <dir>]     log in via QR code (QR-only, no phone/SMS/code)
  serve [--data <dir>]    own the session and serve the archive
  status [--data <dir>]   show serve state (reads status.json only)
  selftest                check the core rules offline
  help | -v | -h
```

**`--data <dir>`** points every command at one state directory. Omit it and the
per-OS default is used. Point `auth`, `serve` and `status` at the same place, or
they will disagree about where the session lives.

> `tbox serve` owns the session today, but there is nothing to serve yet — the
> archive scanner is still to be written (see [Status](#status--read-this-first)
> and the [Roadmap](#roadmap)). It takes the lock, writes `status.json`, prints
> which of the two reasons applies, and exits 1. If the build has no TDLib at
> all, it says so and points at `scripts/fetch-tdjson.*`.

### Exit codes

| Code | Meaning |
|---|---|
| `0` | success |
| `1` | runtime error — the command exists but could not do its job (not logged in, already running, nothing to serve) |
| `2` | unknown command or bad usage |

---

## The data directory

One layout on every OS, so `auth`, `serve` and `status` always agree:

```
<root>/tdlib          TDLib session database
<root>/session.lock   exclusive owner lock (held by `serve`)
<root>/status.json    service status (written by `serve`, read by `status`)
```

| OS | Default `<root>` |
|---|---|
| Windows | `%LOCALAPPDATA%\tbox` (falls back to `%USERPROFILE%\AppData\Local\tbox`) |
| macOS | `~/Library/Application Support/tbox` |
| Linux | `$XDG_DATA_HOME/tbox`, else `~/.local/share/tbox` |

### `session.lock` — why "one Telegram session, one process"

Two processes opening the same Telegram session **invalidate its auth key**
within minutes (`AuthKeyUnregisteredError`). That is not a theoretical problem;
it happened in v0.3 and cost the session.

So ownership is enforced by the OS, not by the file:

- **Windows** — `CreateFile` with `FILE_SHARE_READ`, which denies a second *writer*
- **POSIX** — `flock(LOCK_EX | LOCK_NB)`

A crash releases the lock automatically, so there is never a stale lock file to
clean up by hand. If `serve` reports `already running (pid N)` and you are sure
nothing is running, check pid N before killing anything.

### `status.json` — the seam for tray / service

```json
{"v":1,"state":"ready","user":"tpat","phone":"+91…","since":"2026-10-04T09:00:00Z",
 "transport":"ftp","endpoint":"127.0.0.1:2121","indexed":128,"cache_bytes":1048576,
 "pid":4242,"error":null}
```

`state` ∈ `starting | qr_required | authorizing | ready | stopped | error`.
Written atomically (temp file + rename) on every state change, so a reader can
never observe half a file. `tbox status` reads **only** this file — it works
while `serve` runs, and after `serve` died (a dead pid is reported as
*stale status*). This is the contract a tray icon or a systemd unit will use
later; neither will be a second session owner, only a supervisor of `serve`.

---

## Building

### Windows

```powershell
# full gate: wipe build/, configure with MSVC, build, run ctest
powershell -ExecutionPolicy Bypass -File scripts\clean-configure.ps1

# just the binaries + ctest (keeps an existing build/)
scripts\build-win.cmd
```

Both locate Visual Studio themselves via `vswhere.exe`. Add `-NoTests` to the
first for a faster CLI-only loop.

### Linux / macOS

```sh
# TDLib: once, only needed for `tbox auth`
./scripts/fetch-tdjson.sh

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Skip the fetch and you get a working core build with `auth` reporting itself
unavailable.

> **Known gap (Linux/macOS):** the TDLib distribution ships extra shared
> libraries that the build does not stage next to the binary yet. If the loader
> complains about a missing `lib*.so.*`, run with `LD_LIBRARY_PATH=build:vendor`
> until that is fixed. Releases are Windows-only for the same reason — see
> `.github/workflows/release.yml`.

### Build options

| Option | Default | Effect |
|---|---|---|
| `TBOX_BUILD_TESTS` | `ON` | Build `tests/test_*` + register with CTest |
| `TBOX_TDJSON_LIBRARY` | auto-detect `vendor/` | Path to a prebuilt `libtdjson`; pass `OFF` for a TDLib-free build |

### Warnings are errors

The project compiles clean under **MSVC `/W4 /WX`** and **GCC `-Wall -Wextra
-Werror`**. This applies to project code only — vendored third-party sources are
built without it. Two rules follow from that and are worth knowing before you
touch the code:

- Never build a path with `snprintf("%s/%s")`. GCC's `-Wformat-truncation`
  cannot prove it fits and `-Werror` then fails the build. Use the
  length-checked `tbox_datadir_join()` (or a plain `memcpy`).
- POSIX files must define `_POSIX_C_SOURCE 200809L` (plus `_DEFAULT_SOURCE`
  where glibc needs it for `flock`) *before* any include. MSVC needs
  `_CRT_SECURE_NO_WARNINGS`.

---

## Tests

```sh
ctest --test-dir build -C Release --output-on-failure
```

| Suite | Covers |
|---|---|
| `test_cli` | the dispatcher matrix: exit codes for every command and usage error |
| `test_index` | caption ledger, path normalization, traversal rejection, implied dirs, listing |
| `test_scanmap` | one TDLib message → one entry: caption wins, fallbacks, dedupe |
| `test_indexfile` | the `index.json` mirror, written atomically and read back |
| `test_read` | the body cache and the fetcher contract, with a fake fetcher |
| `test_ftp_proto` | the pure FTP half: parsing, replies, listing format, path handling |
| `test_ftpd` | the socket half on a real loopback listener: login, sessions, transfers |
| `test_statusfile` | `status.json` atomic write/read, state names, lock exclusivity |
| `test_signal` | OS interrupt listener, both backends |
| `test_thread` | OS thread wrapper, both backends |
| `test_smoke` | end-to-end wiring |

**Tests are headless by rule: no TDLib, no network, no secrets, no real data
directory.** New `tests/test_*.c` files are picked up automatically — no CMake
edit needed. Every test uses scratch paths, so running the suite cannot touch
your Saved Messages or your real `%LOCALAPPDATA%\tbox`.

---

## Architecture

```
src/cli/          argv -> command dispatch
src/cmd/          the commands (auth, serve, status, selftest) + shared --data args
src/core/         archive index: caption ledger, path rules, tree listing   [pure C]
src/util/         data dir, owner lock, status file, OS signal/thread/term   [pure C]
src/td/           everything that touches TDLib: QR auth, client params
third_party/      cJSON (MIT), qrcodegen (MIT), TDLib headers
```

Two rules keep this buildable and testable:

1. **`tboxcore` never links TDLib.** TDLib-touching entry points are injected
   into the command layer as function pointers from `main.c`
   (`tbox_cmd_set_auth`, `tbox_cmd_set_serve`). A TDLib-less binary still runs
   every command; the ones that need Telegram say so honestly.
2. **One header per abstraction, one native backend per OS.**
   `util/tbox_signal.h` and `util/tbox_thread.h` each have a Windows and a POSIX
   implementation, so the tests are the *same files* on every platform.

The archive tree is **derived, not stored**. Folder structure lives in message
captions:

```
{"v":1,"type":"file","name":"backup/data/x.csv","size":1234,"mtime":1750000000,"sha256":"…"}
{"v":1,"type":"dir","name":"backup"}
```

Folders are implied by any entry's path, so no folder message is needed. A
Telegram document with no valid caption is still browsable — it shows under its
Telegram filename, or `message-<id>`. Text-only messages are invisible, which
keeps Saved Messages a file tree rather than a chat log. Captions are
byte-compatible with the v0.3 Python client, so existing archives keep working.

---

## Security

- **Credentials** come from the environment only (§ above) — never a file, never
  a command line, never compiled in.
- **Loopback only.** The planned FTP server binds `127.0.0.1` and requires
  `USER tbox` plus a locally generated token. Never expose it on a LAN; if
  remote access is ever needed, that is SFTP's job.
- **Hostile paths are the client's job to get wrong.** Every incoming path goes
  through `tbox_archive_name()`, which rejects any whole `..` segment
  (`/..foo` is a legal name). Verified by tests.
- **Session at rest is TDLib's default plain database** in this version. Sealing
  it and moving the key into the OS credential store is planned, not done —
  treat the data directory as sensitive.

---

## Roadmap

| | Step | Slice |
|---|---|---|
| ~~1~~ | ~~**Scan loop** — documents paginated newest-first, into the index~~ **done** | C |
| ~~2~~ | ~~**Read path** — `downloadFile` into a bounded cache~~ **done** | D |
| ~~3~~ | ~~**FTP server, read** — loopback + token, WinSCP/curl browse the tree~~ **done** | E |
| 4 | **FTP server, write** — upload, delete, rename, mkdir, replace-by-upload | F |
| 5 | **SFTP** — reuses everything above; only the transport changes | G |
| 6 | **OS presence** — tray / Windows service / systemd unit, driven purely by `status.json` | H |

The plan originally said the scan would use `searchMessages` with a document
filter. It cannot: `searchMessages` only ever searches TDLib's *local* database,
so on a cold session it finds nothing. The scanner therefore pages
`getChatHistory` instead — server-side, complete, and deletions visible, which
an incremental scan could never do.

Each step is small enough to review on its own and ends green on all three
platforms. The known gaps above — POSIX TDLib runtime staging, plain session at
rest — slot in alongside steps 4 and 6.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). The short version: keep the warnings-as-errors
build green on all three platforms, keep the tests headless, and add a test
with every behavior you add.

## License

[BSD 3-Clause](LICENSE) © 2026 Tarang Patel. Vendored dependencies keep their
own licenses (cJSON MIT, qrcodegen MIT, TDLib Boost-1.0).