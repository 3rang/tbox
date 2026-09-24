#!/bin/sh
set -eu

VERSION="${1:-1.8.67}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENDOR="$ROOT/vendor"
mkdir -p "$VENDOR"

case "$(uname -s)-$(uname -m)" in
  Linux-x86_64)         ASSET="tdlib.native.ubuntu-22.04.x86-64.zip" ;;
  Linux-aarch64)        ASSET="tdlib.native.ubuntu-22.04.aarch64.zip" ;;
  Darwin-x86_64)        ASSET="tdlib.native.macos.x86-64.zip" ;;
  Darwin-arm64)         ASSET="tdlib.native.macos.aarch64.zip" ;;
  *) echo "unsupported platform: $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac

URL="https://github.com/ForNeVeR/tdlib.native/releases/download/v$VERSION/$ASSET"
echo "Downloading $URL"
if command -v curl >/dev/null 2>&1; then
  curl -fsSL "$URL" -o "$TMPDIR/${ASSET}"
else
  echo "curl not found" >&2; exit 1
fi
unzip -o -j "$TMPDIR/${ASSET}" -d "$VENDOR"
echo "Vendored into $VENDOR:"
ls "$VENDOR"