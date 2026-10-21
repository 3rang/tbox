# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Tests for secret storage (S3) and the .env migration (S4).

The OS secure store is replaced with an in-memory backend, so these run
anywhere and never touch the real Credential Manager/Keychain.
"""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import keyring
from keyring.backend import KeyringBackend

from tbox import config, paths, secrets_store


class MemoryKeyring(KeyringBackend):
    """An in-process stand-in for the OS store."""

    priority = 1

    def __init__(self) -> None:
        super().__init__()
        self.store: dict[tuple[str, str], str] = {}

    def get_password(self, service, username):
        return self.store.get((service, username))

    def set_password(self, service, username, password):
        self.store[(service, username)] = password

    def delete_password(self, service, username):
        self.store.pop((service, username), None)


class FailingKeyring(KeyringBackend):
    """A backend that refuses everything, like a locked store."""

    priority = 1

    def get_password(self, service, username):
        raise keyring.errors.KeyringError("locked")

    def set_password(self, service, username, password):
        raise keyring.errors.KeyringError("locked")

    def delete_password(self, service, username):
        raise keyring.errors.KeyringError("locked")


class SecretStoreTest(unittest.TestCase):
    def setUp(self) -> None:
        self._old = keyring.get_keyring()
        keyring.set_keyring(MemoryKeyring())

    def tearDown(self) -> None:
        keyring.set_keyring(self._old)

    def test_roundtrip(self):
        self.assertTrue(secrets_store.set_secret("k", "v"))
        self.assertEqual(secrets_store.get_secret("k"), "v")
        self.assertTrue(secrets_store.delete_secret("k"))
        self.assertIsNone(secrets_store.get_secret("k"))

    def test_a_failing_store_degrades_instead_of_raising(self):
        keyring.set_keyring(FailingKeyring())
        self.assertIsNone(secrets_store.get_secret("k"))
        self.assertFalse(secrets_store.set_secret("k", "v"))
        self.assertTrue(secrets_store.delete_secret("k"))  # nothing to remove
        self.assertFalse(secrets_store.is_available())

    def test_db_key_is_32_bytes_base64(self):
        import base64

        raw = secrets_store.generate_db_key()
        self.assertEqual(len(base64.b64decode(raw)), 32)
        # two keys are never the same
        self.assertNotEqual(raw, secrets_store.generate_db_key())


class DbKeyTest(unittest.TestCase):
    def setUp(self) -> None:
        self._old = keyring.get_keyring()
        keyring.set_keyring(MemoryKeyring())

    def tearDown(self) -> None:
        keyring.set_keyring(self._old)

    def test_db_key_is_created_once_and_remembered(self):
        first = config.db_key()
        self.assertTrue(first)
        self.assertEqual(config.db_key(), first)

    def test_db_key_is_empty_when_the_store_is_unusable(self):
        keyring.set_keyring(FailingKeyring())
        self.assertEqual(config.db_key(), "")


class EnvMigrationTest(unittest.TestCase):
    """S4: a legacy TG_API_HASH in .env moves to the store, once."""

    def setUp(self) -> None:
        self._old = keyring.get_keyring()
        self.store = MemoryKeyring()
        keyring.set_keyring(self.store)
        self.tmp = Path(tempfile.mkdtemp())
        self.env = self.tmp / ".env"
        self.env.write_text(
            "# tbox settings\nTG_API_ID=35982092\nTG_API_HASH=" + "a" * 32 + "\n",
            encoding="utf-8",
        )
        config.clear_env_cache()

    def tearDown(self) -> None:
        keyring.set_keyring(self._old)
        config.clear_env_cache()

    def test_hash_moves_to_the_store_and_the_file_drops_it(self):
        self.assertTrue(config.migrate_env_secrets(self.env))
        self.assertEqual(
            secrets_store.get_secret(secrets_store.API_HASH), "a" * 32
        )
        text = self.env.read_text(encoding="utf-8")
        self.assertNotIn("TG_API_HASH", text)
        self.assertIn("TG_API_ID=35982092", text)

    def test_migration_is_a_no_op_without_a_legacy_hash(self):
        self.env.write_text("TG_API_ID=1\n", encoding="utf-8")
        config.clear_env_cache()
        self.assertFalse(config.migrate_env_secrets(self.env))

    def test_hash_survives_when_the_store_is_unusable(self):
        keyring.set_keyring(FailingKeyring())
        self.assertFalse(config.migrate_env_secrets(self.env))
        # the credential is still in the file, and the user is told why
        self.assertIn("TG_API_HASH", self.env.read_text(encoding="utf-8"))
        self.assertIsNotNone(config.migration_warning())

    def test_api_hash_reads_the_store(self):
        with mock.patch.dict("os.environ", {}, clear=True):
            config.migrate_env_secrets(self.env)
            config.clear_env_cache()
            self.assertEqual(config.api_hash(), "a" * 32)


class RemoveLocalDataTest(unittest.TestCase):
    def setUp(self) -> None:
        self._old = keyring.get_keyring()
        self.store = MemoryKeyring()
        keyring.set_keyring(self.store)
        self.tmp = Path(tempfile.mkdtemp())

    def tearDown(self) -> None:
        keyring.set_keyring(self._old)

    def test_removes_the_session_dir_env_and_secrets(self):
        session = self.tmp / "session"
        (session / "tdlib").mkdir(parents=True)
        (session / "tdlib" / "db").write_text("x", encoding="utf-8")
        env = self.tmp / ".env"
        env.write_text("TG_API_ID=1\n", encoding="utf-8")
        secrets_store.set_secret(secrets_store.API_HASH, "a" * 32)
        secrets_store.set_secret(secrets_store.TDLIB_DB_KEY, "key")

        with mock.patch("tbox.paths.session_dir", return_value=session):
            with mock.patch("tbox.paths.env_path", return_value=env):
                removed = paths.remove_local_data()

        self.assertFalse(session.exists())
        self.assertFalse(env.exists())
        self.assertIsNone(secrets_store.get_secret(secrets_store.API_HASH))
        self.assertIsNone(secrets_store.get_secret(secrets_store.TDLIB_DB_KEY))
        self.assertEqual(len(removed), 2)
