# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Settings: read them once, validate them, hand them to the engine.

Non-secret settings live in the .env file at :func:`tbox.paths.env_path`:

    TG_API_ID=1234567
    TBOX_CORE=/path/to/tbox-engine        (optional override)

Secrets do NOT: per spec S3 the ``api_hash`` and the TDLib database key live
in the OS secure store (:mod:`tbox.secrets_store`). A legacy ``TG_API_HASH``
line in .env is moved to the store once, on first use, by
:func:`migrate_env_secrets` (spec S4) - and kept in place, with a warning, if
the store is unusable, because losing a credential is worse than a plain
file.

Real environment variables of the same name still win, which is handy for
one-off runs. Nothing here is read by the C engine - the front validates and
then passes values in as parameters.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

from . import paths, secrets_store
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


#: Set once by migrate_env_secrets() when the store could not be used, so the
#: caller can warn exactly once instead of on every read.
_migration_warning: str | None = None


def migration_warning() -> str | None:
    """The S4 warning to show the user, or None. Clears on read."""
    global _migration_warning
    note, _migration_warning = _migration_warning, None
    return note


def _write_env_file(values: dict[str, str], path: Path) -> None:
    """Rewrites the .env file with exactly ``values`` (no merge)."""
    body = ["# tbox settings - local to this computer, never commit it"]
    body += [f"{key}={value}" for key, value in sorted(values.items())]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(body) + "\n", encoding="utf-8")
    _restrict(path)
    clear_env_cache()


def migrate_env_secrets(path: Path | None = None) -> bool:
    """Move a legacy ``TG_API_HASH`` from .env into the secure store (S4).

    Returns True when a migration happened, False when there was nothing to
    do or it could not be done safely. On success the .env file is rewritten
    without the secret line. On failure the .env file is left untouched and
    :func:`migration_warning` explains why - never lose the credential.
    """
    global _migration_warning
    target = path or paths.env_path()
    values = _read_env_file(target)
    legacy = values.get("TG_API_HASH")
    if not legacy:
        return False

    if not secrets_store.is_available():
        _migration_warning = (
            "could not reach the OS secure store; keeping TG_API_HASH in "
            f"{target}. Install/repair your system keyring to move it."
        )
        return False

    if not secrets_store.set_secret(secrets_store.API_HASH, legacy):
        _migration_warning = (
            f"could not save TG_API_HASH to the OS secure store; keeping it in {target}."
        )
        return False

    del values["TG_API_HASH"]
    if values:
        _write_env_file(values, target)
    else:
        # nothing left worth keeping: remove the file rather than leave a
        # header with no settings
        target.unlink(missing_ok=True)
        clear_env_cache()
    return True


def db_key() -> str:
    """The TDLib database key (spec S5), generated once and stored (S3).

    The key is 32 random bytes, base64. It is created on first call and
    remembered; a store that cannot be used yields an empty string, which
    tells the engine to run with an unencrypted database rather than fail.
    """
    existing = secrets_store.get_secret(secrets_store.TDLIB_DB_KEY)
    if existing:
        return existing
    if not secrets_store.is_available():
        return ""
    fresh = secrets_store.generate_db_key()
    if secrets_store.set_secret(secrets_store.TDLIB_DB_KEY, fresh):
        return fresh
    return ""


def api_id() -> str:
    """Telegram API id (numeric, from .env or the environment)."""
    return validate_api_id(_setting("TG_API_ID") or "")


def api_hash() -> str:
    """Telegram API hash (32 hex characters), from the secure store.

    Order: an explicit environment variable (one-off runs), then the OS
    secure store, then a legacy .env line (which is then migrated).
    """
    raw = os.environ.get("TG_API_HASH")
    if raw:
        return validate_api_hash(raw)
    stored = secrets_store.get_secret(secrets_store.API_HASH)
    if stored:
        return validate_api_hash(stored)
    legacy = load_env().get("TG_API_HASH")
    if legacy:
        migrate_env_secrets()  # best effort; fall back to the file value
        return validate_api_hash(legacy)
    raise ConfigError(
        "no Telegram api_hash yet - run 'tbox setup' to save your credentials"
    )


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
        raise ConfigError("TG_API_HASH is missing - run 'tbox setup' to save it")
    if not _API_HASH_RE.match(raw.lower()):
        raise ConfigError(f"TG_API_HASH must be 32 hex characters, got {raw!r}")
    return raw


def api_id() -> str:
    """Telegram API id (numeric, from .env or the environment)."""
    return validate_api_id(_setting("TG_API_ID") or "")


def api_hash() -> str:
    """Telegram API hash (32 hex characters), from the secure store.

    Order: an explicit environment variable (one-off runs), then the OS
    secure store, then a legacy .env line (which is then migrated).
    """
    raw = os.environ.get("TG_API_HASH")
    if raw:
        return validate_api_hash(raw)
    stored = secrets_store.get_secret(secrets_store.API_HASH)
    if stored:
        return validate_api_hash(stored)
    legacy = load_env().get("TG_API_HASH")
    if legacy:
        migrate_env_secrets()  # best effort; fall back to the file value
        return validate_api_hash(legacy)
    raise ConfigError(
        "no Telegram api_hash yet - run 'tbox setup' to save your credentials"
    )


def save_env(values: dict[str, str], path: Path | None = None) -> Path:
    """Merge non-secret ``values`` into the .env file and forget the cache.

    Anything already in the file is kept, so saving credentials does not
    throw away other settings. A ``TG_API_HASH`` in ``values`` is written to
    the OS secure store instead of the file (spec S3); if the store is
    unusable it raises ConfigError rather than silently writing a secret to
    disk.
    """
    target = path or paths.env_path()
    values = dict(values)
    secret = values.pop("TG_API_HASH", None)
    if secret is not None:
        if not secrets_store.set_secret(secrets_store.API_HASH, secret):
            raise ConfigError(
                "the OS secure store is unavailable, so the api_hash was not "
                "saved (refusing to write a secret to a plain file)"
            )
    merged = {**_read_env_file(target), **values}
    if merged:
        _write_env_file(merged, target)
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