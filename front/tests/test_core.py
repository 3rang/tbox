# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Tests for the engine driver and settings.

No engine binary, no network, no Telegram: a fake "engine" is just this same
Python interpreter running a tiny script that speaks the protocol.
"""

from __future__ import annotations

import contextlib
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tbox import config
from tbox.core import Core
from tbox.errors import ConfigError, CoreError


@contextlib.contextmanager
def env(**values: str):
    """Set environment variables for the duration of a with-block."""
    saved = {key: os.environ.get(key) for key in values}
    os.environ.update(values)
    try:
        yield
    finally:
        for key, old in saved.items():
            if old is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = old


def fake_engine(source: str) -> Core:
    """A Core whose "engine" is Python running ``source``."""
    return Core.spawn([sys.executable, "-c", source])


class EventStreamTest(unittest.TestCase):
    def test_json_lines_become_dicts_and_junk_is_skipped(self):
        core = fake_engine(
            "print('{\"event\":\"auth_state\",\"state\":\"WAIT_QR\"}')\n"
            "print('')\n"
            "print('engine chatter, not json')\n"
            "print('{\"event\":\"ready\",\"user\":\"Tarang Patel\"}')\n"
        )
        self.assertEqual(
            list(core.events()),
            [
                {"event": "auth_state", "state": "WAIT_QR"},
                {"event": "ready", "user": "Tarang Patel"},
            ],
        )
        self.assertEqual(core.stop(), 0)

    def test_send_reaches_the_engine(self):
        core = fake_engine(
            "import json, sys\n"
            "line = sys.stdin.readline().strip()\n"
            "print(json.dumps({'event': 'echo', 'line': line}), flush=True)\n"
        )
        core.send("hunter2")
        self.assertEqual(list(core.events()), [{"event": "echo", "line": "hunter2"}])
        self.assertEqual(core.stop(), 0)

    def test_stop_reports_the_exit_code(self):
        core = fake_engine("raise SystemExit(3)\n")
        self.assertEqual(list(core.events()), [])
        self.assertEqual(core.stop(), 3)

    def test_context_manager_stops_the_engine(self):
        with fake_engine("print('{}')\n") as core:
            list(core.events())
        self.assertIsNotNone(core.stop())  # already exited: harmless


class ConfigTest(unittest.TestCase):
    def test_env_file_parsing(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / ".env"
            path.write_text(
                "# a comment\n"
                "TG_API_ID=42\n"
                "\n"
                'TG_API_HASH="0123456789abcdef0123456789abcdef"\n'
                "this line has no equals sign\n",
                encoding="utf-8",
            )
            self.assertEqual(
                config.load_env(path),
                {
                    "TG_API_ID": "42",
                    "TG_API_HASH": "0123456789abcdef0123456789abcdef",
                },
            )

    def test_api_values_come_from_the_environment(self):
        with env(TG_API_ID="1234567", TG_API_HASH="A" * 32):
            self.assertEqual(config.api_id(), "1234567")
            self.assertEqual(config.api_hash(), "A" * 32)

    def test_bad_values_are_refused(self):
        with env(TG_API_ID="not-a-number"), self.assertRaises(ConfigError):
            config.api_id()
        with env(TG_API_HASH="too-short"), self.assertRaises(ConfigError):
            config.api_hash()

    def test_missing_api_hash_is_refused(self):
        with env(TG_API_HASH="x"), self.assertRaises(ConfigError):
            config.api_hash()

    def test_core_exe_honours_the_override(self):
        with env(TBOX_CORE=sys.executable):
            self.assertEqual(config.core_exe(), Path(sys.executable))

    def test_bundled_engine_is_preferred_over_the_repo_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundled = Path(tmp) / "bin"
            bundled.mkdir()
            exe = bundled / ("tbox.exe" if os.name == "nt" else "tbox")
            exe.write_text("", encoding="utf-8")
            with mock.patch("tbox.config._setting", return_value=None):
                with mock.patch("tbox.paths.bundled_dir", return_value=bundled):
                    self.assertEqual(config.core_exe(), exe)

    def test_frozen_build_finds_its_own_engine(self):
        name = "tbox.exe" if os.name == "nt" else "tbox"
        with tempfile.TemporaryDirectory() as tmp:
            extract = Path(tmp) / "meipass"
            extract.mkdir()
            exe = extract / name
            exe.write_text("", encoding="utf-8")
            with mock.patch("tbox.config._setting", return_value=None):
                with mock.patch("sys.frozen", True, create=True):
                    with mock.patch("sys._MEIPASS", str(extract), create=True):
                        self.assertEqual(config.core_exe(), exe)

    def test_missing_engine_is_reported_clearly(self):
        with tempfile.TemporaryDirectory() as tmp:
            with env(TBOX_CORE=str(Path(tmp) / "nope")):
                with self.assertRaises(CoreError) as caught:
                    config.core_exe()
        self.assertIn("build it first", str(caught.exception))


if __name__ == "__main__":
    unittest.main()