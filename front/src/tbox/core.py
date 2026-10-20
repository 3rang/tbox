# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Driving the C engine: start it, feed it lines, read its JSON events.

The engine is a child process. Plain text goes in on stdin; one JSON object
per stdout line comes back. That is the whole protocol, and it is deliberate:
no shared memory and no ctypes, so a crash inside TDLib cannot take this
process down with it, and the conversation can be logged and replayed.

Calling model: :meth:`Core.events` blocks the calling thread until the next
event arrives (that is the "blocking call" shape). A front that must stay
responsive runs it in a worker thread — the engine does not care which.
"""

from __future__ import annotations

import json
import subprocess
from typing import Iterator

from . import config
from .errors import CoreError


class Core:
    """A running C engine, speaking JSON lines over stdin/stdout."""

    def __init__(self, proc: subprocess.Popen) -> None:
        self._proc = proc

    @classmethod
    def start(cls, *args: str) -> "Core":
        """Start ``build/tbox <args...>`` (resolved by :func:`tbox.config.core_exe`)."""
        return cls.spawn([str(config.core_exe()), *args])

    @classmethod
    def spawn(cls, argv: list[str]) -> "Core":
        """Start an explicit command.

        Used by tests, and by anyone running an engine from elsewhere.
        """
        try:
            proc = subprocess.Popen(
                argv,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=None,  # engine diagnostics go straight to your terminal
                text=True,
                encoding="utf-8",
                bufsize=1,  # line buffered: events arrive as soon as they happen
            )
        except OSError as exc:
            raise CoreError(f"could not start {argv[0]}: {exc}") from exc
        return cls(proc)

    def events(self) -> Iterator[dict]:
        """Yield one dict per event, until the engine exits.

        A line that is not JSON is skipped rather than fatal: the engine may
        print a human hint one day, and that must not break a front.
        """
        stream = self._proc.stdout
        if stream is None:
            return
        for line in stream:
            line = line.strip()
            if not line:
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(event, dict):
                yield event

    def send(self, line: str) -> None:
        """Write one line to the engine (for example, a 2FA password)."""
        if self._proc.stdin is None:
            raise CoreError("the engine's input stream is closed")
        self._proc.stdin.write(line + "\n")
        self._proc.stdin.flush()

    def stop(self, timeout: float = 5.0) -> int:
        """Close the engine's input, wait for it to exit, return its exit code."""
        if self._proc.stdin is not None and not self._proc.stdin.closed:
            self._proc.stdin.close()
        try:
            code = self._proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self._proc.kill()
            code = self._proc.wait()
        # the pipes stay open (and warn) unless we close them ourselves
        for stream in (self._proc.stdout, self._proc.stderr):
            if stream is not None and not stream.closed:
                stream.close()
        return code

    def __enter__(self) -> "Core":
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.stop()