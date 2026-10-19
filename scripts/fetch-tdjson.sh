# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
#
# scripts/fetch-tdjson.sh - download the TDLib runtime (libtdjson.so + deps)
# into vendor/ on Linux/macOS-style systems. Same nupkg source as the
# Windows script.
#
# Usage:
#   bash scripts/fetch-tdjson.sh
TDJSON_VERSION="1.8.67"
BASE="https://github.com/ForNeVeR/tdlib.native/releases/download/v${TDJSON_VERSION}"
ASSET="tdlib.native.linux-x64.${TDJSON_VERSION}.nupkg"

set -e
cd "$(dirname "$0")/.."
mkdir -p vendor

tmp="$(mktemp -d)"
echo "Downloading ${BASE}/${ASSET} ..."
curl -fL "${BASE}/${ASSET}" -o "${tmp}/${ASSET}"

unzip -o -q "${tmp}/${ASSET}" "runtimes/linux-x64/native/*" -d "${tmp}"
cp -f "${tmp}"/runtimes/linux-x64/native/* vendor/
rm -rf "${tmp}"

echo "Vendored into vendor/:"
ls -la vendor/
