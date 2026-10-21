# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Running the engine on its own thread.

The window never blocks on the engine: this worker owns the long-running
``serve`` child process and turns its event stream into Qt signals, which is
the only thing the window ever sees. Requests (a two-step password, a
logout) are made from the GUI thread; the engine client guards its own
writes, so that is safe.

That is the whole point of the earlier split: swapping a terminal for a
window changes nothing about how the engine is driven.
"""

from __future__ import annotations

from PySide6.QtCore import QThread, Signal

from ..engine_client import EngineClient
from ..errors import TboxError


class EngineWorker(QThread):
    """A long-running engine session, off the UI thread."""

    event = Signal(dict)
    failed = Signal(str)

    def __init__(
        self,
        session_dir: str,
        api_id: str,
        api_hash: str,
        db_key: str = "",
        parent=None,
    ) -> None:
        super().__init__(parent)
        self._session_dir = session_dir
        self._api_id = api_id
        self._api_hash = api_hash
        self._db_key = db_key
        self._client: EngineClient | None = None

    def run(self) -> None:  # executes on the worker thread
        try:
            self._client = EngineClient(self._session_dir, on_event=self.event.emit)
            self._client.start()
            self._client.auth_start(self._api_id, self._api_hash, self._db_key)
        except TboxError as exc:
            self.failed.emit(str(exc))
            return
        # stay alive as long as the engine does, so later session events
        # (connection changes, an eventual close) keep reaching the window
        try:
            while not self._client.wait_for_close(5.0):
                pass
        except Exception:  # pragma: no cover - defensive
            pass
        finally:
            if self._client is not None:
                self._client.close()

    def submit_password(self, password: str) -> None:
        """Answer the engine's ``auth.password_needed``."""
        if self._client is not None:
            self._client.auth_password(password)

    def logout(self) -> None:
        """Ask the engine to log out on Telegram."""
        if self._client is not None:
            self._client.auth_logout()

    def stop(self, timeout_ms: int = 4000) -> None:
        """Shut the engine down and wait for the thread to finish."""
        if self._client is not None:
            self._client.close()
        self.wait(timeout_ms)
