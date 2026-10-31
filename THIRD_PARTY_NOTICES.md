<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2026 Tarang Patel -->
# Third-party notices

tbox itself is Apache-2.0 (see [LICENSE](LICENSE)). It uses:

| Name | Version | License | How |
|---|---|---|---|
| [cJSON](https://github.com/DaveGamble/cJSON) | 1.7.19 | MIT | vendored at `third_party/cjson/`, compiled in |
| [QR Code generator](https://github.com/nayuki/QR-Code-generator) (Project Nayuki) | c library, 2020 | MIT | vendored at `third_party/qrcodegen/`, compiled in; draws the login QR code |
| [TDLib](https://github.com/tdlib/td) | 1.8.67 | Boost Software License 1.0 | linked at build time: the prebuilt `libtdjson` downloaded by CMake and shipped beside `tbox` in releases, or compiled from source into `tbox` with `-DTBOX_TDLIB_FROM_SOURCE=ON` |

The prebuilt TDLib comes from the [tdlib.native](https://github.com/ForNeVeR/tdlib.native)
packaging of official TDLib releases. The Windows package also contains
OpenSSL 3 (`libcrypto-3-x64.dll`, `libssl-3-x64.dll`, Apache-2.0) and
zlib (`z.dll`, zlib license), which TDLib links against. On Linux and macOS
TDLib uses the system OpenSSL and zlib. A from-source build also compiles
TDLib's bundled SQLite (public domain).
