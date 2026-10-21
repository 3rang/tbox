# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Secret storage (spec S3).

The seed secrets a tbox install owns - the Telegram ``api_hash`` and the
TDLib database key - live in the OS secure store, never in a plain file and
never in the environment. `keyring` picks the platform backend (Windows
Credential Manager, macOS Keychain, Secret Service on Linux).

Everything here degrades instead of crashing: a machine with no usable
backend returns ``None`` from :func:`get_secret` and ``False`` from
:func:`set_secret`, and the caller decides what to tell the user. S4 is
explicit that a broken keyring must never lose a credential.
"""

from __future__ import annotations

import keyring
from keyring.errors import KeyringError

#: One keyring service for everything tbox owns.
SERVICE = "tbox"

#: Usernames (keyring "accounts") under that service.
API_HASH = "api_hash"
TDLIB_DB_KEY = "tdlib_db_key"


class SecretStoreError(Exception):
    """The OS secure store could not be used (unavailable, refused, locked)."""


def is_available() -> bool:
    """True when a real, working backend is present.

    `keyring` always hands back *some* backend object; on a headless Linux
    box that can be a null one that silently remembers nothing. A tiny
    round-trip is the only honest test, and it is cheap.
    """
    try:
        backend = keyring.get_keyring()
    except Exception:  # pragma: no cover - backend import surprises
        return False
    if backend is None:
        return False
    name = type(backend).__name__.lower()
    # the deliberately useless fallbacks keyring ships
    if "fail" in name or "null" in name:
        return False
    try:
        probe = "_tbox_probe"
        keyring.set_password(SERVICE, probe, "1")
        ok = keyring.get_password(SERVICE, probe) == "1"
        keyring.delete_password(SERVICE, probe)
        return ok
    except Exception:
        return False


def get_secret(username: str) -> str | None:
    """The stored secret, or None when unset or the store is unusable."""
    try:
        return keyring.get_password(SERVICE, username)
    except KeyringError:
        return None


def set_secret(username: str, value: str) -> bool:
    """Stores a secret. Returns False (never raises) when it cannot."""
    try:
        keyring.set_password(SERVICE, username, value)
        return True
    except KeyringError:
        return False


def delete_secret(username: str) -> bool:
    """Removes a secret. A secret that was never there is not an error."""
    try:
        keyring.delete_password(SERVICE, username)
        return True
    except KeyringError:
        return True


def generate_db_key() -> str:
    """A fresh TDLib database key: 32 random bytes, base64 (spec S5).

    The engine wants the base64 text as ``database_encryption_key``; the
    bytes themselves never leave this function.
    """
    import base64
    import os

    return base64.b64encode(os.urandom(32)).decode("ascii")
