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

    def test_modern_protocol_events_render_the_same_way(self):
        # the real engine speaks auth.state/auth.qr/auth.ready/auth.closed
        # with the payload under "data" (spec 6.5)
        core = scripted_engine(
            "out(event='auth.state', data={'state': 'WAIT_QR'})",
            "out(event='auth.qr', data={'link': 'tg://login?token=T'})",
            "out(event='auth.ready', data={'display_name': 'Tarang Patel'})",
            "out(event='auth.closed', data={})",
        )
        code, text = rendered(core)
        self.assertEqual(code, 0)
        self.assertIn("Scan this with your phone", text)
        self.assertIn("Linked as Tarang Patel.", text)
        self.assertIn("Session closed.", text)

    def test_modern_engine_error_changes_the_exit_code(self):
        core = scripted_engine(
            "out(event='engine.error', data={'message': 'API_ID_INVALID', 'code': 400})"
        )
        code, text = rendered(core)
        self.assertEqual(code, 1)
        self.assertIn("API_ID_INVALID", text)


class LogoutTest(unittest.TestCase):
    def test_logout_sends_the_command_and_reports_success(self):
        sent: list[str] = []

        class FakeClient:
            def __init__(self, *args, **kwargs) -> None:
                pass

            def start(self) -> None:
                pass

            def auth_logout(self) -> None:
                sent.append("logout")

            def close(self, timeout: float = 5.0) -> None:
                pass

            def __enter__(self):
                return self

            def __exit__(self, *exc):
                self.close()

        buffer = io.StringIO()
        with mock.patch("tbox.cli.EngineClient", FakeClient):
            with contextlib.redirect_stdout(buffer):
                code = cli.main(["logout"])
        self.assertEqual(code, 0)
        self.assertEqual(sent, ["logout"])
        self.assertIn("Logged out", buffer.getvalue())


class CatalogCommandsTest(unittest.TestCase):
    """``tbox ls`` and ``tbox rescan`` against a scripted engine."""

    class FakeClient:
        """Answers the way the engine would, without one."""

        def __init__(self, profile_dir, on_event=None, **kwargs) -> None:
            self.profile_dir = profile_dir
            self.on_event = on_event
            self.items: list[dict] = []
            self.op_id = "0000000000000001"
            self.signed_in = True
            self.closed = False
            self.asked: list[str] = []

        def start(self) -> None:
            pass

        def auth_start(self, api_id: str, api_hash: str, db_key: str = "") -> None:
            self.asked.append("auth.start")
            if not self.signed_in:
                self._emit({"event": "auth.qr", "data": {"link": "tg://login?token=T"}})
                return
            self._emit({"event": "auth.ready", "data": {"display_name": "Someone"}})

        def catalog_list(self, folder_id: str, timeout: float = 900.0) -> list[dict]:
            self.asked.append(f"catalog.list {folder_id}")
            return list(self.items)

        def sync_rescan(self, timeout: float = 900.0) -> str:
            self.asked.append("sync.rescan")
            self._emit(
                {
                    "event": "sync.done",
                    "data": {"op_id": self.op_id, "objects": 3, "inbox_items": 1},
                }
            )
            return self.op_id

        def close(self, timeout: float = 5.0) -> None:
            self.closed = True

        def _emit(self, event: dict) -> None:
            if self.on_event is not None:
                self.on_event(event)

    def run_cli(self, argv: list[str], client) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()

        def factory(profile_dir, on_event=None, **kwargs):
            client.on_event = on_event
            return client

        with mock.patch("tbox.cli.EngineClient", factory):
            with mock.patch("tbox.cli.config.api_id", return_value="1"):
                with mock.patch("tbox.cli.config.api_hash", return_value="a" * 32):
                    with mock.patch("tbox.cli.config.db_key", return_value=""):
                        with contextlib.redirect_stdout(out):
                            with contextlib.redirect_stderr(err):
                                code = cli.main(argv)
        return code, out.getvalue(), err.getvalue()

    def test_ls_prints_the_items_of_a_folder(self):
        client = self.FakeClient("")
        client.items = [
            {"kind": "f", "name": "report.pdf", "size": 2048, "mtime": 1760000000},
            {"kind": "d", "name": "docs", "size": None, "mtime": None},
        ]

        code, out, _ = self.run_cli(["ls"], client)

        self.assertEqual(code, 0)
        self.assertIn("report.pdf", out)
        self.assertIn("2.0 KB", out)
        self.assertIn("docs/", out)
        self.assertIn("catalog.list root", client.asked)

    def test_ls_asks_for_the_folder_it_was_given(self):
        client = self.FakeClient("")

        code, out, _ = self.run_cli(["ls", "inbox"], client)

        self.assertEqual(code, 0)
        self.assertIn("catalog.list inbox", client.asked)
        self.assertIn("Inbox", out)

    def test_ls_says_when_the_folder_is_empty(self):
        client = self.FakeClient("")

        code, out, _ = self.run_cli(["ls"], client)

        self.assertEqual(code, 0)
        self.assertIn("nothing here", out)

    def test_rescan_reports_what_it_found(self):
        client = self.FakeClient("")

        code, out, _ = self.run_cli(["rescan"], client)

        self.assertEqual(code, 0)
        self.assertIn("sync.rescan", client.asked)
        self.assertIn("3 objects", out)

    def test_a_computer_that_is_not_signed_in_is_told_so(self):
        client = self.FakeClient("")
        client.signed_in = False

        code, _, err = self.run_cli(["ls"], client)

        self.assertEqual(code, 1)
        self.assertIn("not signed in", err)

    def test_missing_credentials_point_at_setup(self):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch("tbox.cli.config.api_id", side_effect=ConfigError("nope")):
            with contextlib.redirect_stdout(out):
                with contextlib.redirect_stderr(err):
                    code = cli.main(["ls"])
        self.assertEqual(code, 1)
        self.assertIn("tbox setup", err.getvalue())


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