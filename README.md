<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2026 Tarang Patel -->
# tbox

A personal file archive over Telegram Saved Messages: your files live in a
chat only you can read, and a local index remembers what is where.

- **C core** (`src/`) — the engine: tdjson bridge, sessions, later file ops.
- **Python front** (`front/`) — settings, the window you actually use, and a
  command line for power users and debugging.

## Build it (one command)

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

That single command does all of it, in order:

1. builds and tests the C engine (`scripts/build-native.ps1`)
2. copies the engine and its runtime DLLs into the Python package (`tbox/bin/`)
3. installs the front into `.venv`
4. runs the front's tests
5. writes a wheel to `dist/` — the whole app, engine included
6. freezes the desktop app into one file, `dist/tbox.exe`, and runs its
   built-in self-check so a bundle that cannot start fails the build

Needs Visual Studio Build Tools (MSVC via vcvars), CMake, and Python 3.11+.
Step 2 is why one command is enough: the engine stops being a separate thing
you have to find, and becomes part of the app.

Working on one half only:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-native.ps1   # C engine + ctest
.venv\Scripts\python -m pip install -e "front[gui]"                 # front (editable, with the window)
.venv\Scripts\python -m unittest discover -s front\tests            # front tests
```

The TDLib runtime (`tdjson.dll`, OpenSSL companions, `z.dll`) ships in
`vendor/`. If it goes missing, re-download it with:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\fetch-tdjson.ps1   # Windows
bash scripts/fetch-tdjson.sh                                        # Linux/macOS
```

## Use it

Double-click `dist\tbox.exe`. That is the whole app: window, engine and
TDLib in one file. First run:

1. it asks for your Telegram `api_id` and `api_hash` — free to obtain:
   open <https://my.telegram.org/apps>, create (or open) an application, and
   copy the two values. "Where do I get these?" opens that page for you.
2. it shows a QR code; scan it with your phone:
   **Settings > Devices > Link Desktop Device**
3. if your account has two-step verification, it asks for that password once
   (typed hidden, sent only to Telegram)

then it says **Linked as Your Name** and closes. The second run skips
everything and goes straight in, because TDLib keeps the session.

![tbox login: keys, QR, two-step, linked](docs/login-demo.gif)

That is the real window running its real code path, with the engine
scripted so no account or phone is needed: the keys screen, the QR (which
refreshes once mid-animation, as it does live), the two-step prompt, then
the signed-in screen. Regenerate it from the source with
`scripts/make-demo-gif.py` whenever the screens change.

The QR refreshes by itself (roughly every 30 seconds); the window always
shows the newest one, so just keep scanning.

Prefer the command line? The same flow exists there:

```powershell
.venv\Scripts\tbox.exe setup    # asks for your Telegram api_id + api_hash
.venv\Scripts\tbox.exe login    # shows a QR code; scan it with your phone
```

MVP status: `setup` and `login` exist today. The archive commands (put, get,
list, ...) come next, built on the same engine.

### Where things live

| Path | What it is |
|---|---|
| `%LOCALAPPDATA%\tbox\.env` | your api_id (local only, never committed) |
| your OS secure store (service `tbox`) | the api_hash and the TDLib database key |
| `%LOCALAPPDATA%\tbox\session\` | TDLib's database — deleting it means scanning again |
| `build\` | the compiled engine and its test binaries (gitignored) |
| `front\src\tbox\bin\` | the engine copied into the package by `build.ps1` |
| `dist\tbox.exe` | the app as one windowed file (engine inside) |
| `dist\` | the finished wheel |

### Commands

| Command | What it does |
|---|---|
| `tbox setup` | ask for the Telegram app credentials and save them |
| `tbox login` | link this computer (QR; asks for credentials on the first run) |
| `tbox login -v` | same, plus every raw event the engine sends |
| `tbox --version` | print the version |

The desktop app has no commands beyond the developer-only `--selftest`
(which the build script uses to check a frozen bundle).

## Troubleshooting

| What you see | What it means |
|---|---|
| `error: API_ID_INVALID (code 400)` | the api_id/api_hash pair is wrong — re-check <https://my.telegram.org/apps> |
| `error: engine not found` | the engine is not built or bundled — run `build.ps1` |
| `error: no Telegram credentials yet` | run `tbox setup` (it only prompts when a terminal is attached) |
| The window says "Something went wrong" | same cause as the error above; "Change the keys" reopens the first-run screen |
| `dist\tbox.exe` starts but no window appears | your antivirus may block a freshly built exe; the same app runs from `.venv\Scripts\tbox.exe login` |
| The QR shows as `##` pairs | your terminal is not UTF-8; set `PYTHONIOENCODING=utf-8` or use Windows Terminal (the window is unaffected) |
| You want to start over | delete `%LOCALAPPDATA%\tbox\session\` and log in again |
| Nothing seems to happen | `tbox login -v` — raw engine events, including connection state |

## How the two halves talk

The engine does the work and reports **state events**; the front decides what
a person sees. No printing, prompting, QR drawing, heartbeats or timeouts
live in C — the front owns all of that, and passes credentials and paths in.

There are two fronts over the same engine: the window (`front/src/tbox/gui/`,
which runs the engine on its own thread so the UI never blocks) and the
command line (`front/src/tbox/cli.py`). Both read the events below and nothing
else, which is why they behave identically.

```
python front (settings, UX, index)  →  tbox C engine  →  tdjson.dll
```

Only the C engine touches tdjson. `src/service/tbox_session.*` is that
engine: it owns the session directory, the TDLib parameters, the auth state
machine and the request/reply plumbing, and it speaks one line of JSON per
event:

```
{"event":"auth_state","state":"WAIT_QR"}
{"event":"qr_link","link":"tg://login?token=..."}
{"event":"password_needed","hint":"..."}
{"event":"ready","user":"..."}
{"event":"closed"}
{"event":"error","code":400,"message":"API_ID_INVALID"}
```

The engine is a child process, so a crash inside TDLib cannot take the front
down with it, and the whole conversation can be logged and replayed. `tbox
login` in C is the same engine with a tiny adapter, which is how the engine
gets tested without Python.

## Layout

```
build.ps1            one command -> app
src/                 C engine (cli/, cmd/, service/, td/)
tests/               C tests (ctest gate)
front/               Python front
  src/tbox/          the package: config, core (engine driver), qr, cli
  src/tbox/gui/      the window (theme, worker thread, screens)
  packaging/         the freezer: spec, icon drawing, entry point
  tests/             Python tests, including the window (headless)
scripts/             build-native.ps1, fetch-tdjson, make-demo-gif.py
docs/                login-demo.gif, regenerated by scripts/make-demo-gif.py
third_party/cjson    vendored JSON library
vendor/              prebuilt TDLib runtime
cmake/               tdjson import-lib + staging rules
archive/             earlier designs and plans (local only)
```

## License

Apache-2.0 — see `LICENSE`.