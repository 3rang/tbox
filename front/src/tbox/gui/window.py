# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""The app window: six screens, driven by engine events.

The whole user-facing journey lives here and nowhere else:

    keys -> connect -> scan -> two-step password -> done

Every decision comes from an event (see :mod:`tbox.core`), so the window
never has to guess what the engine is doing. Screens are plain widgets held
in a stack; :meth:`MainWindow.handle_event` is the only thing that switches
them, which is also what makes it easy to test without an account.
"""

from __future__ import annotations

from PySide6.QtCore import Qt, QUrl
from PySide6.QtGui import QDesktopServices, QGuiApplication
from PySide6.QtWidgets import (
    QCheckBox,
    QFrame,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QProgressBar,
    QPushButton,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)

from .. import config, paths
from ..errors import TboxError
from . import qrview
from .worker import EngineWorker

MY_TELEGRAM_APPS = "https://my.telegram.org/apps"


def _label(text: str = "", name: str = "") -> QLabel:
    label = QLabel(text)
    if name:
        label.setObjectName(name)
    label.setWordWrap(True)
    return label


def _field(placeholder: str, secret: bool = False) -> QLineEdit:
    field = QLineEdit()
    field.setPlaceholderText(placeholder)
    field.setClearButtonEnabled(True)
    if secret:
        field.setEchoMode(QLineEdit.EchoMode.Password)
    return field


def _button(text: str, primary: bool = False, name: str = "") -> QPushButton:
    button = QPushButton(text)
    if primary:
        button.setObjectName("Primary")
    elif name:
        button.setObjectName(name)
    button.setCursor(Qt.CursorShape.PointingHandCursor)
    return button


def _card(*children: QWidget, spacing: int = 14) -> QFrame:
    card = QFrame()
    card.setObjectName("Card")
    layout = QVBoxLayout(card)
    layout.setContentsMargins(28, 28, 28, 28)
    layout.setSpacing(spacing)
    for child in children:
        layout.addWidget(child)
    return card


def _page(*children: QWidget) -> QWidget:
    page = QWidget()
    layout = QVBoxLayout(page)
    layout.setContentsMargins(0, 0, 0, 0)
    layout.addStretch(1)
    layout.addWidget(_card(*children))
    layout.addStretch(1)
    return page


class MainWindow(QMainWindow):
    """The one window of the app.

    ``autostart=False`` builds everything without starting a login, which is
    how tests and the packaging self-check inspect the app.
    """

    def __init__(self, autostart: bool = True) -> None:
        super().__init__()
        self.setWindowTitle("tbox")
        self.resize(580, 720)
        self._worker: EngineWorker | None = None
        self.current_page = ""
        self._build()
        self._centre()
        if autostart:
            self.start()

    # ---- what a person sees ---------------------------------------------

    def _build(self) -> None:
        self.pages: dict[str, QWidget] = {}

        # connecting
        self.progress = QProgressBar()
        self.progress.setRange(0, 0)  # indeterminate: we cannot know how long
        self.pages["connecting"] = _page(
            _label("Connecting", "Title"),
            _label("Talking to Telegram. This usually takes a few seconds.", "Subtitle"),
            self.progress,
        )

        # first run: the Telegram app keys
        self.id_field = _field("api_id  -  e.g. 35982092")
        self.hash_field = _field("api_hash  -  32 hex characters", secret=True)
        self.show_hash = QCheckBox("Show api_hash")
        self.show_hash.toggled.connect(
            lambda shown: self.hash_field.setEchoMode(
                QLineEdit.EchoMode.Normal if shown else QLineEdit.EchoMode.Password
            )
        )
        self.cred_error = _label("", "Danger")
        self.cred_error.hide()
        get_keys = _button("Where do I get these?", name="Link")
        get_keys.clicked.connect(
            lambda: QDesktopServices.openUrl(QUrl(MY_TELEGRAM_APPS))
        )
        save = _button("Save and continue", primary=True)
        save.clicked.connect(self._save_credentials)
        self.pages["credentials"] = _page(
            _label("Welcome to tbox", "Title"),
            _label(
                "tbox needs its own Telegram app keys. They are free: create an "
                "application at my.telegram.org and copy its api_id and api_hash.",
                "Subtitle",
            ),
            get_keys,
            self.id_field,
            self.hash_field,
            self.show_hash,
            self.cred_error,
            save,
        )

        # scan the code
        self.qr_label = _label()
        self.qr_label.setObjectName("Code")
        self.qr_label.setFixedSize(324, 324)
        self.qr_label.setScaledContents(False)
        self.qr_caption = _label("Scan with your phone:  Settings > Devices > Link Desktop Device")
        cancel_qr = _button("Cancel")
        cancel_qr.clicked.connect(self._cancel)
        self.pages["qr"] = _page(
            _label("Link this computer", "Title"),
            self.qr_label,
            self.qr_caption,
            _label("The code refreshes by itself - the newest one is always shown.", "Muted"),
            cancel_qr,
        )

        # two-step password
        self.password_hint = _label("Your account is protected by a password.", "Subtitle")
        self.password_field = _field("Password", secret=True)
        self.password_field.returnPressed.connect(self._submit_password)
        submit = _button("Continue", primary=True)
        submit.clicked.connect(self._submit_password)
        cancel_pw = _button("Cancel")
        cancel_pw.clicked.connect(self._cancel)
        self.pages["password"] = _page(
            _label("Two-step verification", "Title"),
            self.password_hint,
            self.password_field,
            _label("It goes to Telegram only, and is never stored.", "Muted"),
            submit,
            cancel_pw,
        )

        # done
        self.done_badge = _label("✓", "Badge")
        self.done_title = _label("Linked", "Title")
        self.done_note = _label("", "Ok")
        logout_btn = _button("Log out of Telegram")
        logout_btn.clicked.connect(self._logout)
        remove_data = _button("Remove local data")
        remove_data.clicked.connect(self._remove_local_data)
        close = _button("Close", primary=True)
        close.clicked.connect(self.close)
        self.pages["done"] = _page(
            self.done_badge,
            self.done_title,
            self.done_note,
            _label("This computer stays signed in - the next start goes straight in.", "Subtitle"),
            logout_btn,
            remove_data,
            close,
        )

        # error
        self.error_text = _label("", "Danger")
        fix_keys = _button("Change the keys", primary=True)
        fix_keys.clicked.connect(self._back_to_keys)
        quit_app = _button("Quit")
        quit_app.clicked.connect(self.close)
        self.pages["error"] = _page(
            _label("Something went wrong", "Title"),
            self.error_text,
            fix_keys,
            quit_app,
        )

        self.stack = QStackedWidget()
        for page in self.pages.values():
            self.stack.addWidget(page)
        self.show_page("connecting")  # so current_page always matches what is on screen

        self.status_label = _label("", "Status")

        central = QWidget()
        layout = QVBoxLayout(central)
        layout.setContentsMargins(18, 18, 18, 12)
        layout.setSpacing(10)
        layout.addWidget(self.stack, 1)
        layout.addWidget(self.status_label)
        self.setCentralWidget(central)

    def _centre(self) -> None:
        screen = QGuiApplication.primaryScreen()
        if screen is not None:
            self.move(screen.availableGeometry().center() - self.rect().center())

    def show_page(self, name: str) -> None:
        """Switch screens (the only way pages change)."""
        self.stack.setCurrentWidget(self.pages[name])
        self.current_page = name

    def status(self, text: str) -> None:
        self.status_label.setText(text)

    # ---- flow -----------------------------------------------------------

    def start(self) -> None:
        """Log in, asking for keys first when there are none yet."""
        config.migrate_env_secrets()
        note = config.migration_warning()
        try:
            api_id, api_hash = config.api_id(), config.api_hash()
        except TboxError:
            self._prefill_if_known()
            self.show_page("credentials")
            msg = "First run: the app keys are needed once."
            if note:
                msg = f"{note} {msg}"
            self.status(msg)
            return
        if note:
            self.status(note)
        self._launch(api_id, api_hash)

    def _launch(self, api_id: str, api_hash: str) -> None:
        self._stop_worker()
        self.show_page("connecting")
        self.status("Connecting to Telegram...")
        self._worker = EngineWorker(
            str(paths.session_dir()), api_id, api_hash, config.db_key(), self
        )
        self._worker.event.connect(self.handle_event)
        self._worker.failed.connect(self.on_failed)
        self._worker.start()

    def handle_event(self, event: dict) -> None:
        """One engine event in, at most one screen change out."""
        name = event.get("event")
        data = event.get("data") or {}

        if name == "auth.state":
            state = str(data.get("state", ""))
            if state == "WAIT_QR":
                self.status("Waiting for you to scan the code...")
            elif state == "WAIT_PASSWORD":
                self.status("Waiting for your two-step password...")
            elif state == "READY":
                self.status("Signed in - finishing up...")
            elif state == "CLOSING":
                self.status("Signing out...")
            elif state == "CLOSED":
                self.done_badge.setText("✓")
                self.done_note.setText("Session closed cleanly.")
                self.show_page("done")
                self.status("Done.")

        elif name == "conn.state":
            state = str(data.get("state", ""))
            if state.endswith("Ready"):
                self.status("Connected.")
            elif state.endswith("Connecting"):
                self.status("Connecting to Telegram...")
            elif state.endswith("Updating"):
                self.status("Syncing with Telegram...")

        elif name == "auth.qr":
            self.qr_label.setPixmap(qrview.pixmap(str(data.get("link", "")), 300))
            self.show_page("qr")
            self.status("Waiting for you to scan the code...")

        elif name == "auth.password_needed":
            hint = str(data.get("hint", "")).strip()
            if hint:
                self.password_hint.setText(f"Hint from Telegram: {hint}")
            self.pages["password"].setEnabled(True)
            self.show_page("password")
            self.password_field.setFocus()

        elif name == "auth.ready":
            who = str(data.get("display_name", "")).strip()
            self.done_title.setText(f"Linked as {who}" if who else "Linked")
            self.done_note.setText("")
            self.show_page("done")
            self.status("Signed in.")

        elif name == "auth.closed":
            self.done_badge.setText("✓")
            self.done_note.setText("Session closed cleanly.")
            self.show_page("done")
            self.status("Done.")

        elif name == "engine.error":
            code = data.get("code")
            message = str(data.get("message", "unknown error"))
            if message == "PASSWORD_HASH_INVALID":
                # not fatal: Telegram will ask again, so stay on the password
                # screen and say what happened
                self.password_hint.setText("That password did not match. Try again.")
                self.password_field.clear()
                self.pages["password"].setEnabled(True)
                self.show_page("password")
                self.password_field.setFocus()
                self.status("Wrong password.")
                return
            self.on_failed(f"Telegram said: {message}" + (f"  (code {code})" if code else ""))

        elif name == "closed":
            self.done_badge.setText("✓")
            self.done_note.setText("Session closed cleanly.")
            self.show_page("done")
            self.status("Done.")

        elif name == "error":
            message = str(data.get("message", event.get("message", "unknown error")))
            code = event.get("code")
            if message == "PASSWORD_HASH_INVALID":
                self.password_hint.setText("That password did not match. Try again.")
                self.password_field.clear()
                self.pages["password"].setEnabled(True)
                self.show_page("password")
                self.password_field.setFocus()
                self.status("Wrong password.")
                return
            self.on_failed(f"Telegram said: {message}" + (f"  (code {code})" if code else ""))

        elif name == "cancelled":
            self.status("Cancelled.")

    def on_failed(self, message: str) -> None:
        """Anything that stops the flow: show it, offer a way back."""
        self.error_text.setText(message)
        self.show_page("error")
        self.status("")

    # ---- actions --------------------------------------------------------

    def _submit_password(self) -> None:
        password = self.password_field.text()
        if not password:
            return
        self.password_field.clear()
        if self._worker is not None:
            self._worker.submit_password(password)
        self.pages["password"].setEnabled(False)
        self.status("Checking the password...")

    def _save_credentials(self) -> None:
        api_id = self.id_field.text().strip()
        api_hash = self.hash_field.text().strip()
        try:
            config.validate_api_id(api_id)
            config.validate_api_hash(api_hash)
        except TboxError as exc:
            self.cred_error.setText(str(exc))
            self.cred_error.show()
            return
        self.cred_error.hide()
        try:
            saved = config.save_env({"TG_API_ID": api_id, "TG_API_HASH": api_hash})
        except TboxError as exc:
            self.cred_error.setText(str(exc))
            self.cred_error.show()
            return
        self.status(f"Saved to {saved}")
        self._launch(api_id, api_hash)

    def _logout(self) -> None:
        if self._worker is not None:
            self._worker.logout()
            self.status("Logging out of Telegram...")
        else:
            self.status("No active session to log out.")

    def _remove_local_data(self) -> None:
        response = QMessageBox.question(
            self,
            "Remove local data",
            "This deletes this computer's session, downloaded files and saved "
            "settings. Your Telegram account is not deleted, but this device "
            "will need to be linked again. Continue?",
        )
        if response != QMessageBox.StandardButton.Yes:
            return
        self._stop_worker()
        removed = paths.remove_local_data()
        self.done_note.setText(
            "Local data removed. Run tbox login or start the app again to link this computer."
        )
        self.status(f"Removed: {', '.join(removed) if removed else 'nothing'}")

    def _prefill_if_known(self) -> None:
        known = config.load_env(paths.env_path())
        self.id_field.setText(known.get("TG_API_ID", ""))
        # api_hash may live in the secure store; nothing to prefill there
        self.hash_field.setText("")

    def _back_to_keys(self) -> None:
        self._stop_worker()
        self._prefill_if_known()
        self.show_page("credentials")
        self.status("Fix the keys, then continue.")

    def _cancel(self) -> None:
        self._stop_worker()
        self.close()

    def _stop_worker(self) -> None:
        if self._worker is not None:
            self._worker.stop()
            self._worker = None

    def closeEvent(self, event) -> None:  # noqa: N802 (Qt naming)
        self._stop_worker()
        super().closeEvent(event)
