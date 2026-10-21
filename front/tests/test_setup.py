# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Tests for first-run credentials: the questions, the .env file, the retries."""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import keyring
from keyring.backend import KeyringBackend

from tbox import cli, config


class MemoryKeyring(KeyringBackend):
    """An in-process stand-in for the OS store.

    The real Windows Credential Manager must never be touched by a test:
    writing a fake api_hash there would clobber the owner's real one.
    """

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


def run_setup(env_file: Path, api_id: str, api_hash: str) -> tuple[int, str]:
    buffer = io.StringIO()
    with mock.patch("tbox.cli.paths.env_path", return_value=env_file):
        with mock.patch("builtins.input", return_value=api_id):
            with mock.patch("tbox.cli.getpass.getpass", return_value=api_hash):
                with contextlib.redirect_stdout(buffer):
                    code = cli.main(["setup"])
    return code, buffer.getvalue()


class SetupTest(unittest.TestCase):
    def setUp(self) -> None:
        self._old_keyring = keyring.get_keyring()
        keyring.set_keyring(MemoryKeyring())
        config.clear_env_cache()

    def tearDown(self) -> None:
        keyring.set_keyring(self._old_keyring)
        config.clear_env_cache()

    def test_setup_saves_credentials_where_login_will_read_them(self):
        with tempfile.TemporaryDirectory() as tmp:
            env_file = Path(tmp) / ".env"
            code, text = run_setup(env_file, "35982092", "a" * 32)

            self.assertEqual(code, 0)
            self.assertIn("my.telegram.org/apps", text)
            self.assertIn(str(env_file), text)
            # the api_hash goes to the secure store (S3), not the .env file
            self.assertEqual(config.load_env(env_file), {"TG_API_ID": "35982092"})
            self.assertEqual(config.api_hash(), "a" * 32)

    def test_setup_keeps_settings_it_does_not_own(self):
        with tempfile.TemporaryDirectory() as tmp:
            env_file = Path(tmp) / ".env"
            env_file.write_text("TBOX_CORE=/somewhere/tbox\n", encoding="utf-8")
            run_setup(env_file, "42", "b" * 32)

            values = config.load_env(env_file)
            self.assertEqual(values["TBOX_CORE"], "/somewhere/tbox")
            self.assertEqual(values["TG_API_ID"], "42")

    def test_setup_re_asks_until_the_answer_is_valid(self):
        with tempfile.TemporaryDirectory() as tmp:
            env_file = Path(tmp) / ".env"
            answers = iter(["nonsense", "42"])
            buffer = io.StringIO()
            with mock.patch("tbox.cli.paths.env_path", return_value=env_file):
                with mock.patch("builtins.input", side_effect=answers):
                    with mock.patch("tbox.cli.getpass.getpass", return_value="c" * 32):
                        with contextlib.redirect_stdout(buffer):
                            code = cli.main(["setup"])

            self.assertEqual(code, 0)
            self.assertIn("must be numeric", buffer.getvalue())
            self.assertEqual(config.load_env(env_file)["TG_API_ID"], "42")

    def test_save_env_writes_a_readable_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            env_file = Path(tmp) / "nested" / ".env"  # parents must be created
            config.save_env({"TG_API_ID": "7", "TG_API_HASH": "d" * 32}, path=env_file)
            self.assertIn("TG_API_ID=7", env_file.read_text(encoding="utf-8"))

    def test_saved_values_are_visible_to_the_next_reader(self):
        with tempfile.TemporaryDirectory() as tmp:
            env_file = Path(tmp) / ".env"
            config.save_env({"TG_API_ID": "99"}, path=env_file)
            # the in-process cache must not hide what was just written
            self.assertEqual(config.load_env(env_file)["TG_API_ID"], "99")


if __name__ == "__main__":
    unittest.main()