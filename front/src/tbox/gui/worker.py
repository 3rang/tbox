# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Running the engine on its own thread.

The engine driver blocks while it waits for the next event, so it cannot run
on the thread that paints the window — the app would freeze between events.
This worker owns the child process and turns its event stream into Qt
signals, which is the only thing the window ever sees.

That is the whole point of the earlier split: swapping a terminal for a
window changes nothing about how the engine is driven.
"""

from __future__ import annotations

from PySide6.QtCore import QThread, Signal

from ..core import Core
from ..errors import TboxError


class EngineWorker(QThread):
    """A login session, running off the UI thread."""

    event = Signal(dict)
    failed = Signal(str)

    def __init__(self, session_dir: str, api_id: str, api_hash: str, parent=None) -> None:
        super().__init__(parent)
        self._args = (session_dir, api_id, api_hash)
        self._core: Core | None = None

    def run(self) -> None:  # executes on the worker thread
        try:
            self._core = Core.start_login(*self._args)
        except TboxError as exc:
            self.failed.emit(str(exc))
            return
        with self._core:
            for event in self._core.events():
                self.event.emit(event)

    def send(self, line: str) -> None:
        """Hand the engine one line of input (a two-step password)."""
        if self._core is not None:
            self._core.send(line)

    def stop(self, timeout_ms: int = 4000) -> None:
        """Close the engine's input and wait for the thread to finish."""
        if self._core is not None:
            self._core.stop()
        self.wait(timeout_ms)