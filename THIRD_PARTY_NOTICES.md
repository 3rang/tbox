<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2026 Tarang Patel -->
# Third-party notices

Everything tbox vendors or installs, with version, license and source.
Adding a dependency means adding it here; the vendor DLLs are fetched by
`scripts/fetch-tdjson.ps1` and pinned to one TDLib version.

## Vendored (shipped inside the repository)

| Name | Version | License | Source |
|---|---|---|---|
| cJSON | 1.7.19 | MIT | <https://github.com/DaveGamble/cJSON> — vendored at `third_party/cjson/` |
| TDLib | 1.8.67 | Boost Software License 1.0 | <https://github.com/tdlib/td> — vendored as `vendor/tdjson.dll` |
| OpenSSL | 3.x | Apache-2.0 | <https://github.com/openssl/openssl> — vendored as `libcrypto-3-x64.dll`, `libssl-3-x64.dll` |
| zlib | 1.3.2 | zlib License | <https://github.com/madler/zlib> — vendored as `z.dll` |

The TDLib runtime and its companion DLLs come prebuilt from the tdlib.native
packaging of official TDLib releases:
<https://github.com/ForNeVeR/tdlib.native/releases/download/v1.8.67>.

## Installed by pip (shipped by the wheel)

| Name | Version | License | Source |
|---|---|---|---|
| qrcode | 8.2 | BSD | <https://github.com/lincolnloop/python-qrcode> |
| colorama | 0.4.6 | BSD | <https://github.com/tartley/colorama> — transitive dep of qrcode on Windows |
| PySide6 | 6.12.0 | LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only | <https://code.qt.io/pyside/pyside-setup> — dual license, commercial options sold separately by Qt |

PySide6 is used dynamically (unmodified PyQt-like exceptions apply); it is
bundled into the frozen app by PyInstaller, not linked statically.

## Build-time tools (not shipped inside the app)

| Name | Version | License | Source |
|---|---|---|---|
| pillow | 13.0.0 | MIT-CMU | <https://github.com/python-pillow/Pillow> — draws the app icon and the README demo |
| pyinstaller | 6.22.3 | GPLv2-or-later with its special bootloader linker exception | <https://github.com/pyinstaller/pyinstaller> — freezes the app |
