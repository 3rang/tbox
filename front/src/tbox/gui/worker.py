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

import queue
import threading

from PySide6.QtCore import QThread, Signal

from ..engine_client import EngineClient, EngineError
from ..errors import CoreError, TboxError


class EngineWorker(QThread):
    """A long-running engine session, off the UI thread.

    The window never blocks on the engine: this worker owns the long-running
    ``serve`` child process and turns its event stream into Qt signals, which
    is the only thing the window ever sees. Requests (a two-step password, a
    logout) are made from the GUI thread; the engine client guards its own
    writes, so that is safe.

    That is the whole point of the earlier split: swapping a terminal for a
    window changes nothing about how the engine is driven.
    """

    event = Signal(dict)
    failed = Signal(str)
    #: A request came back: (id, result, error message). The result is a
    #: dict, or None when the request failed.
    answered = Signal(int, object, object)

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
        self._jobs: queue.Queue[tuple[int, str, dict | None, float]] = queue.Queue()
        self._job_lock = threading.Lock()
        self._next_job = 0

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
            while not self._client.wait_for_close(0.1):
                self._drain_jobs()
        except Exception:  # pragma: no cover - defensive
            pass
        finally:
            if self._client is not None:
                self._client.close()

    # ---- requests from the window -------------------------------------

    def call(
        self,
        cmd: str,
        args: dict | None = None,
        timeout: float = 120.0,
    ) -> int:
        """Asks the engine something without blocking the window.

        Returns a request id; the answer arrives as an ``answered`` signal,
        which Qt delivers on the window's own thread.
        """
        with self._job_lock:
            self._next_job += 1
            rid = self._next_job
        self._jobs.put((rid, cmd, args, timeout))
        return rid

    def _drain_jobs(self) -> None:
        """Runs the queued requests (on the worker thread)."""
        while True:
            try:
                rid, cmd, args, timeout = self._jobs.get_nowait()
            except queue.Empty:
                return
            if self._client is None:
                self.answered.emit(rid, None, "the engine is not running")
                continue
            try:
                result = self._client.request(cmd, args, timeout=timeout)
            except (EngineError, CoreError) as exc:
                self.answered.emit(rid, None, str(exc))
            except Exception as exc:  # pragma: no cover - defensive
                self.answered.emit(rid, None, str(exc))
            else:
                self.answered.emit(rid, result, "")

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
