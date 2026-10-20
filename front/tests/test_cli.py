# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Tests for the login screen.

A scripted engine (this same Python, emitting the event JSON a real engine
would) drives the screen, so the whole user-facing flow is covered without
Telegram, a network or an account.
"""

from __future__ import annotations

import contextlib
import io
import sys
import unittest
from unittest import mock

from tbox import cli
from tbox.core import Core
from tbox.errors import ConfigError


def scripted_engine(*lines: str, echo_password: bool = False) -> Core:
    """An engine that prints the given events (and optionally echoes the
    password it is sent, so a test can prove it travelled)."""
    source = [
        "import json, sys",
        "def out(**fields):",
        "    print(json.dumps(fields), flush=True)",
    ]
    for line in lines:
        source.append(line)
    if echo_password:
        source.append("pw = sys.stdin.readline().strip()")
        source.append("out(event='ready', user='pw=' + pw)")
    return Core.spawn([sys.executable, "-c", "\n".join(source)])


def rendered(core: Core, verbose: bool = False) -> tuple[int, str]:
    buffer = io.StringIO()
    try:
        with contextlib.redirect_stdout(buffer):
            code = cli.login_screen(core, verbose=verbose)
    finally:
        core.stop()  # reap the scripted engine and close its pipes
    return code, buffer.getvalue()


class LoginScreenTest(unittest.TestCase):
    def test_fresh_login_shows_the_code_and_the_account(self):
        core = scripted_engine(
            "out(event='auth_state', state='WAIT_PHONE')",
            "out(event='qr_link', link='tg://login?token=TEST')",
            "out(event='ready', user='Tarang Patel')",
            "out(event='closed')",
        )
        code, text = rendered(core)
        self.assertEqual(code, 0)
        self.assertIn("Scan this with your phone", text)
        self.assertIn("Linked as Tarang Patel.", text)
        self.assertIn("Session closed.", text)
        self.assertNotIn("state: WAIT_QR", text)  # quiet: the code says it

    def test_password_is_asked_for_and_sent_to_the_engine(self):
        core = scripted_engine(
            "out(event='password_needed', hint='your pet')",
            echo_password=True,
        )
        with mock.patch("tbox.cli.getpass.getpass", return_value="s3cret"):
            code, text = rendered(core)
        self.assertEqual(code, 0)
        self.assertIn("hint: your pet", text)
        self.assertIn("Linked as pw=s3cret.", text)

    def test_an_engine_error_is_reported_and_changes_the_exit_code(self):
        core = scripted_engine(
            "out(event='error', message='API_ID_INVALID', code=400)"
        )
        code, text = rendered(core)
        self.assertEqual(code, 1)
        self.assertIn("error: API_ID_INVALID (code 400)", text)

    def test_verbose_shows_raw_events(self):
        core = scripted_engine(
            "out(event='connection', state='connectionStateConnecting')",
            "out(event='ready', user='Someone')",
        )
        code, text = rendered(core, verbose=True)
        self.assertEqual(code, 0)
        self.assertIn('"event": "connection"', text)

    def test_no_account_linked_means_failure(self):
        code, _ = rendered(scripted_engine())
        self.assertEqual(code, 1)


class MainTest(unittest.TestCase):
    def test_missing_credentials_point_at_setup(self):
        # a non-interactive stdin must never trigger a prompt
        with mock.patch("tbox.cli.config.api_id", side_effect=ConfigError("nope")):
            with mock.patch("tbox.cli.sys.stdin", io.StringIO()):
                buffer = io.StringIO()
                with contextlib.redirect_stdout(buffer):
                    code = cli.main(["login"])
        self.assertEqual(code, 1)
        self.assertIn("tbox setup", buffer.getvalue())

    def test_version_is_printed(self):
        with self.assertRaises(SystemExit):
            cli.main(["--version"])


if __name__ == "__main__":
    unittest.main()