# tbox

A personal file archive over Telegram Saved Messages.

- **C core** (`src/`) — tdjson bridge, QR authentication, file/chunk primitives.
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

## Login (QR)

The C core is pure: it never reads config. Credentials and paths arrive
as **parameters** — the layering rule is:

```
python front (reads .env, index, UX)  →  tbox C core  →  tdjson.dll
```

Only the C core may touch tdjson; `tbox login` below is the developer
shortcut and takes credentials on argv:

```powershell
build\tbox.exe login session <api-id> <api-hash>
```

(Create an app at https://my.telegram.org/apps for those. They land in
your shell history — fine for the debug path; the Python front will pass
them programmatically instead.)

A QR code is painted in the terminal; scan it with the phone app
(Settings > Devices > Link Desktop Device). Two-step-verification
accounts additionally type their password. The session persists under
`session/` — the second run skips the QR straight to Ready.

## License

Apache-2.0 — see `LICENSE`.
