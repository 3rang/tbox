# tbox

A personal file archive over Telegram Saved Messages.

- **C core** (`src/`) — tdjson bridge, session engine (auth + later file ops).
- **Python front** (planned) — config, local index, CLI driving the C core.

## Build

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-native.ps1
```

Requires Visual Studio Build Tools (MSVC via vcvars) and CMake. The script
cleans `build/`, configures, compiles, and runs the test gate.

The TDLib runtime (`tdjson.dll`, OpenSSL companions, `z.dll`) ships in
`vendor/`. If it goes missing, re-download it with:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\fetch-tdjson.ps1   # Windows
bash scripts/fetch-tdjson.sh                                        # Linux
```

## Auth (engine, no user interface)

The engine does the work and reports **state events**; the front end decides
what a user sees. No printing, prompting, QR painting, heartbeats or
timeouts live in C — a UI can draw a QR, mask a password, and give up after
whatever deadline it likes.

```
python front (reads .env, index, UX)  →  tbox C core  →  tdjson.dll
```

Only the C core touches tdjson. `src/service/tbox_session.*` is that core:
commands in, events out, credentials and paths passed **as parameters**
(the C side never reads config).

The developer adapter is `tbox login`, which prints one JSON line per event
and takes a 2FA password as a plain line on stdin:

```powershell
build\tbox.exe login session <api-id> <api-hash>
```

```
{"event":"auth_state","state":"WAIT_PHONE"}
{"event":"qr_link","link":"tg://login?token=..."}
{"event":"password_needed","hint":"..."}
{"event":"ready","user":"..."}
{"event":"closed"}
{"event":"error","code":400,"message":"API_ID_INVALID"}
```

Create an app at https://my.telegram.org/apps for those credentials (they
land in your shell history — fine for the debug path; the Python front will
pass them programmatically instead). `qr_link` rotates every ~30 s, so a
front should redraw on each new link. The session persists under `session/`
— a second run skips the QR straight to Ready.

## License

Apache-2.0 — see `LICENSE`.
