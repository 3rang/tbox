# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Tests for the desktop app.

Headless: Qt runs on its "offscreen" platform, and instead of a real account
the tests push engine events straight into the window. That keeps the whole
event -> screen mapping covered without a network, an account or a subprocess.
"""

from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from tbox.errors import ConfigError

try:  # the gui extras are optional
    from PySide6.QtCore import QObject, Signal
    from PySide6.QtWidgets import QApplication

    from tbox.gui import qrview, theme
    from tbox.gui.window import MainWindow

    QT_AVAILABLE = True
except ImportError:  # pragma: no cover - depends on the environment
    QT_AVAILABLE = False

requires_qt = unittest.skipUnless(
    QT_AVAILABLE, "PySide6 is not installed (pip install -e front[gui])"
)


class FakeWorker(QObject):
    """Stands in for the engine thread: same signals, records what was sent."""

    event = Signal(dict)
    failed = Signal(str)
    answered = Signal(int, object, object)

    def __init__(self, *args, **kwargs) -> None:
        super().__init__()
        self.sent: list[str] = []
        self.stopped = False
        self.logged_out = False
        self.requests: list[tuple[str, dict | None]] = []

    def start(self) -> None:
        pass

    def send(self, line: str) -> None:
        self.sent.append(line)

    def call(self, cmd: str, args: dict | None = None, timeout: float = 120.0) -> int:
        """Stands in for the worker's request queue."""
        self.requests.append((cmd, args))
        return len(self.requests)

    def answer(self, rid: int, result: dict) -> None:
        """Brings an answer back, the way the worker does."""
        self.answered.emit(rid, result, "")

    def fail(self, rid: int, message: str) -> None:
        self.answered.emit(rid, None, message)

    def submit_password(self, password: str) -> None:
        self.sent.append(password)

    def logout(self) -> None:
        self.logged_out = True

    def stop(self, timeout_ms: int = 0) -> None:
        self.stopped = True


@requires_qt
class WindowTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication([])
        theme.apply(cls.app)

    def setUp(self) -> None:
        self.window = MainWindow(autostart=False)

    def tearDown(self) -> None:
        self.window.close()

    # ---- first run ------------------------------------------------------

    def test_the_connecting_screen_is_up_before_the_flow_starts(self):
        self.assertEqual(self.window.current_page, "connecting")

    def test_first_run_asks_for_the_keys(self):
        with mock.patch("tbox.gui.window.config.api_id", side_effect=ConfigError("none")):
            with mock.patch("tbox.gui.window.config.migrate_env_secrets"):
                with mock.patch("tbox.gui.window.config.migration_warning", return_value=None):
                    self.window.start()
        self.assertEqual(self.window.current_page, "credentials")
        self.assertIn("app keys", self.window.status_label.text())

    def test_keys_are_validated_before_anything_is_saved(self):
        saved = mock.Mock()
        self.window._launch = mock.Mock()
        with mock.patch("tbox.gui.window.config.save_env", saved):
            self.window.id_field.setText("not-a-number")
            self.window.hash_field.setText("a" * 32)
            self.window._save_credentials()

            self.assertFalse(saved.called)
            self.assertFalse(self.window.cred_error.isHidden())
            self.assertIn("numeric", self.window.cred_error.text())

            self.window.id_field.setText("35982092")
            self.window._save_credentials()

        saved.assert_called_once_with({"TG_API_ID": "35982092", "TG_API_HASH": "a" * 32})
        self.window._launch.assert_called_once_with("35982092", "a" * 32)

    def test_known_keys_start_the_engine_on_a_worker(self):
        with tempfile.TemporaryDirectory() as tmp:
            with mock.patch("tbox.gui.window.config.api_id", return_value="1"):
                with mock.patch("tbox.gui.window.config.api_hash", return_value="a" * 32):
                    with mock.patch("tbox.gui.window.config.db_key", return_value=""):
                        with mock.patch("tbox.gui.window.config.migrate_env_secrets"):
                            with mock.patch("tbox.gui.window.config.migration_warning", return_value=None):
                                with mock.patch("tbox.gui.window.EngineWorker", FakeWorker):
                                    with mock.patch(
                                        "tbox.gui.window.paths.session_dir",
                                        return_value=Path(tmp) / "session",
                                    ):
                                        self.window.start()
                                        worker = self.window._worker

        self.assertEqual(self.window.current_page, "connecting")
        self.assertIsInstance(worker, FakeWorker)
        worker.event.emit({"event": "auth.ready", "data": {"display_name": "Someone"}})
        self.assertEqual(self.window.current_page, "files")

    # ---- the journey ----------------------------------------------------

    def test_a_qr_link_draws_a_scannable_code(self):
        self.window.handle_event({"event": "auth.qr", "data": {"link": "tg://login?token=ABC"}})
        self.assertEqual(self.window.current_page, "qr")
        drawn = self.window.qr_label.pixmap()
        self.assertEqual(drawn.width(), 300)
        self.assertEqual(drawn.height(), 300)
        self.assertIn("Devices", self.window.qr_caption.text())

    def test_a_new_link_replaces_the_old_code(self):
        self.window.handle_event({"event": "auth.qr", "data": {"link": "tg://login?token=FIRST"}})
        first = self.window.qr_label.pixmap().toImage()
        self.window.handle_event({"event": "auth.qr", "data": {"link": "tg://login?token=SECOND"}})
        second = self.window.qr_label.pixmap().toImage()
        self.assertNotEqual(first, second)

    def test_two_step_asks_for_the_password_and_sends_it(self):
        worker = FakeWorker()
        self.window._worker = worker
        self.window.handle_event({"event": "auth.password_needed", "data": {"hint": "your pet"}})

        self.assertEqual(self.window.current_page, "password")
        self.assertIn("your pet", self.window.password_hint.text())

        self.window.password_field.setText("s3cret")
        self.window._submit_password()

        self.assertEqual(worker.sent, ["s3cret"])
        self.assertEqual(self.window.password_field.text(), "")  # never kept around

    def test_a_wrong_password_stays_on_the_password_screen(self):
        self.window.handle_event({"event": "auth.password_needed", "data": {"hint": ""}})
        self.window.password_field.setText("wrong")
        self.window._submit_password()
        self.window.handle_event({"event": "engine.error", "data": {"message": "PASSWORD_HASH_INVALID"}})

        self.assertEqual(self.window.current_page, "password")
        self.assertIn("did not match", self.window.password_hint.text())
        self.assertTrue(self.window.pages["password"].isEnabled())

    def test_any_other_error_offers_a_way_back_to_the_keys(self):
        self.window.handle_event(
            {"event": "engine.error", "data": {"message": "API_ID_INVALID", "code": 400}}
        )
        self.assertEqual(self.window.current_page, "error")
        self.assertIn("API_ID_INVALID", self.window.error_text.text())
        self.assertIn("400", self.window.error_text.text())

        self.window._back_to_keys()
        self.assertEqual(self.window.current_page, "credentials")

    def test_ready_then_closed_reports_the_account(self):
        self.window.handle_event({"event": "auth.ready", "data": {"display_name": "Tarang Patel"}})
        self.assertEqual(self.window.current_page, "files")
        self.assertIn("Tarang Patel", self.window.files_page.account.text())

        self.window.handle_event({"event": "auth.closed", "data": {}})
        self.assertEqual(self.window.current_page, "done")
        self.assertIn("cleanly", self.window.done_note.text())

    def test_the_session_is_already_signed_in_goes_straight_to_done(self):
        # a second start: no QR at all, just ready -> closed
        self.window.handle_event({"event": "auth.state", "data": {"state": "READY"}})
        self.window.handle_event({"event": "auth.ready", "data": {"display_name": "Tarang Patel"}})
        self.window.handle_event({"event": "auth.closed", "data": {}})
        self.assertEqual(self.window.current_page, "done")
        self.assertIn("Tarang Patel", self.window.done_title.text())

    # ---- the files screen ----------------------------------------------

    def test_the_files_screen_has_the_four_places(self):
        tree = self.window.files_page.tree
        labels = [
            tree.topLevelItem(i).text(0) for i in range(tree.topLevelItemCount())
        ]
        self.assertEqual(labels, ["Root", "Inbox", "Trash", "Lost & Found"])

    def test_the_files_screen_asks_the_engine_and_shows_the_answer(self):
        worker = FakeWorker()
        self.window._attach_worker(worker)
        self.window._worker = worker
        self.window.handle_event({"event": "auth.ready", "data": {"display_name": "Someone"}})

        self.assertEqual(worker.requests[0], ("catalog.list", {"folder_id": "root"}))

        worker.answer(
            1,
            {
                "items": [
                    {
                        "object_id": "abc",
                        "kind": "f",
                        "name": "report.pdf",
                        "size": 2048,
                        "mtime": 1760000000,
                    },
                    {
                        "object_id": "def",
                        "kind": "d",
                        "name": "docs",
                        "size": None,
                        "mtime": None,
                    },
                ]
            },
        )

        table = self.window.files_page.table
        self.assertEqual(table.rowCount(), 2)
        self.assertEqual(table.item(0, 0).text(), "report.pdf")
        self.assertEqual(table.item(0, 1).text(), "2.0 KB")
        self.assertEqual(table.item(1, 0).text(), "docs")
        self.assertEqual(table.item(1, 1).text(), "-")

    def test_choosing_another_folder_asks_again(self):
        worker = FakeWorker()
        self.window._attach_worker(worker)
        self.window._worker = worker
        self.window.handle_event({"event": "auth.ready", "data": {"display_name": "Someone"}})

        self.window.files_page.select("inbox")

        self.assertEqual(worker.requests[-1], ("catalog.list", {"folder_id": "inbox"}))

    def test_the_rescan_button_asks_for_a_new_read(self):
        worker = FakeWorker()
        self.window._attach_worker(worker)
        self.window._worker = worker
        self.window.handle_event({"event": "auth.ready", "data": {"display_name": "Someone"}})

        self.window.files_page.rescan_button.click()

        self.assertEqual(worker.requests[-1], ("sync.rescan", None))

    def test_a_failed_answer_is_shown_in_words(self):
        worker = FakeWorker()
        self.window._attach_worker(worker)
        self.window._worker = worker
        self.window.handle_event({"event": "auth.ready", "data": {"display_name": "Someone"}})

        worker.fail(1, "NOT_FOUND: no such folder")

        self.assertIn("NOT_FOUND", self.window.status_label.text())

    def test_closing_stops_the_engine(self):
        worker = FakeWorker()
        self.window._worker = worker
        self.window.close()
        self.assertTrue(worker.stopped)

    def test_logout_asks_the_worker_and_reports(self):
        worker = FakeWorker()
        self.window._worker = worker
        self.window._logout()
        self.assertTrue(worker.logged_out)
        self.assertIn("Logging out", self.window.status_label.text())

    def test_remove_local_data_confirms_then_deletes(self):
        worker = FakeWorker()
        self.window._worker = worker
        removed = ["/tmp/session", "/tmp/.env"]
        with mock.patch("tbox.gui.window.paths.remove_local_data", return_value=removed):
            with mock.patch(
                "tbox.gui.window.QMessageBox.question",
                return_value=__import__(
                    "PySide6.QtWidgets", fromlist=["QMessageBox"]
                ).QMessageBox.StandardButton.Yes,
            ):
                self.window._remove_local_data()
        self.assertTrue(worker.stopped)
        self.assertIn("Local data removed", self.window.done_note.text())

    def test_remove_local_data_is_cancelled_by_a_no(self):
        removed = mock.Mock()
        with mock.patch("tbox.gui.window.paths.remove_local_data", removed):
            with mock.patch(
                "tbox.gui.window.QMessageBox.question",
                return_value=__import__(
                    "PySide6.QtWidgets", fromlist=["QMessageBox"]
                ).QMessageBox.StandardButton.No,
            ):
                self.window._remove_local_data()
        removed.assert_not_called()


@requires_qt
class PresentationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication([])

    def test_the_stylesheet_has_no_leftover_tokens(self):
        css = theme.stylesheet()
        self.assertNotIn("@", css)
        self.assertIn(theme.ACCENT, css)

    def test_the_qr_image_is_scaled_without_smoothing(self):
        drawn = qrview.pixmap("tg://login?token=SMOOTH", 240)
        self.assertEqual(drawn.width(), 240)
        # nearest-neighbour keeps edges hard: only two shades survive scaling
        image = drawn.toImage()
        shades = {image.pixelColor(x, 0).name() for x in range(0, 240, 3)}
        self.assertLessEqual(len(shades), 2)


if __name__ == "__main__":
    unittest.main()