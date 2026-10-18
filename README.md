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

The native TDLib library (`tdjson.dll` + companion DLLs) is not committed;
place it in `vendor/` or point `TBOX_TDJSON` at it.

## License

Apache-2.0 — see `LICENSE`.
