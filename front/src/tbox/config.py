# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Settings: read them once, validate them, hand them to the engine.

The .env file lives in :func:`tbox.paths.env_path`. Keys:

    TG_API_ID=1234567
    TG_API_HASH=0123456789abcdef0123456789abcdef

Real environment variables of the same name win over the file, which is
handy for one-off runs. Nothing here is read by the C engine — the front
validates and then passes values in as parameters.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

from . import paths
from .errors import ConfigError, CoreError

_API_ID_RE = re.compile(r"^\d+$")
_API_HASH_RE = re.compile(r"^[0-9a-f]{32}$")

_env_cache: dict[str, str] | None = None


def clear_env_cache() -> None:
    """Drop the cached .env values (mainly for tests)."""
    global _env_cache
    _env_cache = None


def _read_env_file(path: Path) -> dict[str, str]:
    """Parse KEY=VALUE lines; missing file or junk lines are simply skipped."""
    values: dict[str, str] = {}
    if not path.exists():
        return values
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        values[key.strip()] = value.strip().strip('"').strip("'")
    return values


def load_env(path: Path | None = None) -> dict[str, str]:
    """The .env values, cached for the process (pass ``path`` to bypass)."""
    global _env_cache
    if path is not None:
        return _read_env_file(path)
    if _env_cache is None:
        _env_cache = _read_env_file(paths.env_path())
    return _env_cache


def _setting(key: str) -> str | None:
    """Environment first (one-off overrides), then the .env file."""
    return os.environ.get(key) or load_env().get(key)


def validate_api_id(raw: str) -> str:
    """Return ``raw`` unchanged, or raise ConfigError explaining why not."""
    if not raw:
        raise ConfigError(f"TG_API_ID is missing (put it in {paths.env_path()})")
    if not _API_ID_RE.match(raw):
        raise ConfigError(f"TG_API_ID must be numeric, got {raw!r}")
    return raw


def validate_api_hash(raw: str) -> str:
    """Return ``raw`` unchanged, or raise ConfigError explaining why not."""
    if not raw:
        raise ConfigError(f"TG_API_HASH is missing (put it in {paths.env_path()})")
    if not _API_HASH_RE.match(raw.lower()):
        raise ConfigError(f"TG_API_HASH must be 32 hex characters, got {raw!r}")
    return raw


def api_id() -> str:
    """Telegram API id (numeric, from .env or the environment)."""
    return validate_api_id(_setting("TG_API_ID") or "")


def api_hash() -> str:
    """Telegram API hash (32 hex characters)."""
    return validate_api_hash(_setting("TG_API_HASH") or "")


def save_env(values: dict[str, str], path: Path | None = None) -> Path:
    """Merge ``values`` into the .env file and forget the cached copy.

    Anything already in the file is kept, so saving credentials does not
    throw away other settings.
    """
    target = path or paths.env_path()
    merged = {**_read_env_file(target), **values}
    target.parent.mkdir(parents=True, exist_ok=True)
    body = ["# tbox settings - local to this computer, never commit it"]
    body += [f"{key}={value}" for key, value in sorted(merged.items())]
    target.write_text("\n".join(body) + "\n", encoding="utf-8")
    _restrict(target)
    clear_env_cache()
    return target


def _restrict(path: Path) -> None:
    """Best effort owner-only permissions (Windows profile ACLs are already
    private, so this only matters on POSIX)."""
    if os.name != "posix":  # pragma: no cover
        return
    try:  # pragma: no cover
        path.chmod(0o600)
    except OSError:  # pragma: no cover
        pass


def _frozen_dirs() -> list[Path]:
    """Where a PyInstaller build unpacks its files, if this *is* one.

    A one-file build extracts everything (the engine and the DLLs it needs)
    into a temporary directory that lives only for this run, so the engine
    has to be looked for there rather than in ``tbox/bin``.
    """
    if not getattr(sys, "frozen", False):
        return []
    dirs: list[Path] = []
    extract_dir = getattr(sys, "_MEIPASS", None)
    if extract_dir:  # one-file build: the temporary unpack directory
        dirs.append(Path(extract_dir))
    dirs.append(Path(sys.executable).resolve().parent)  # one-folder build
    return dirs


def core_exe() -> Path:
    """The compiled C engine, in order of preference:

    1. ``TBOX_CORE`` — an explicit path, for unusual setups
    2. inside this process — a frozen app bundles its own ``tbox-engine``
    3. ``tbox/bin/`` inside this package — what an installed app uses
    4. ``build/`` in the repository — what a checkout uses

    The DLLs the engine needs (tdjson and friends) sit beside it, so a
    bundled copy is a complete, self-contained app.
    """
    # tbox-engine, not tbox: `tbox` belongs to the GUI app and the pip
    # console script, so the two can never be confused.
    name = "tbox-engine.exe" if os.name == "nt" else "tbox-engine"
    override = _setting("TBOX_CORE")
    if override:
        candidates = [Path(override).expanduser()]
    else:
        candidates = [d / name for d in _frozen_dirs()]
        candidates += [paths.bundled_dir() / name, paths.build_dir() / name]

    for exe in candidates:
        if exe.is_file():
            return exe

    looked_in = ", ".join(str(exe.parent) for exe in candidates)
    raise CoreError(
        f"engine not found (looked for {name} in {looked_in})\n"
        "  build it first:  powershell -ExecutionPolicy Bypass -File build.ps1"
    )