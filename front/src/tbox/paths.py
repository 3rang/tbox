# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Where tbox keeps its stuff on this computer.

One module answers every "where is X?" question; nothing else hardcodes a
path. These are *front* locations (your data, your settings) — the C engine
knows none of them and receives whatever directory it is told to use.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path


def data_dir() -> Path:
    """Root folder for all tbox data.

    Windows: ``%LOCALAPPDATA%\\tbox``
    macOS:   ``~/Library/Application Support/tbox``
    Linux:   ``$XDG_DATA_HOME/tbox`` (or ``~/.local/share/tbox``)
    """
    if os.name == "nt":
        return Path(os.environ.get("LOCALAPPDATA", Path.home())) / "tbox"
    if sys.platform == "darwin":  # pragma: no cover
        return Path.home() / "Library" / "Application Support" / "tbox"
    if os.name == "posix":
        base = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share"))
        return base / "tbox"
    return Path.home() / ".tbox"


def env_path() -> Path:
    """Your Telegram app credentials (TG_API_ID, TG_API_HASH)."""
    return data_dir() / ".env"


def session_dir() -> Path:
    """TDLib's working directory.

    Handed to the C engine as a parameter; the engine creates ``tdlib/``
    and ``files/`` inside it. Keeping it means a second run skips the QR.
    """
    return data_dir() / "session"


def build_dir() -> Path:
    """Where CMake puts the compiled engine (``build/tbox`` + its DLLs).

    The front is at ``<repo>/front/src/tbox/paths.py``, so three parents up
    from this file is the repository root.
    """
    return Path(__file__).resolve().parents[3] / "build"


def bundled_dir() -> Path:
    """The engine that ships *inside* this package (``tbox/bin``).

    An installed wheel carries the binary and the DLLs it needs, so the app
    works without the repository or a compiler.
    """
    return Path(__file__).resolve().parent / "bin"