# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""EngineClient - the long-running engine conversation (spec 6).

One ``tbox-engine serve <profile-dir>`` process per profile, spoken to in
JSON lines: requests carry an integer ``id`` and get exactly one response;
unsolicited lines are events (``{"event": ..., "data": ...}``).

The reader runs on its own thread so the GUI thread never blocks on the
engine, and so an event can be delivered while a request is in flight. This
is the UI the spec wants: the engine owns no user interface, this side owns
all of it.
"""

from __future__ import annotations

import json
import subprocess
import threading
from collections.abc import Callable
from typing import Any

from . import config
from .errors import CoreError

#: The protocol version this client speaks (spec 6.4, `hello`).
PROTOCOL_VERSION = 1

#: How long a request may wait for its response before giving up.
DEFAULT_TIMEOUT = 30.0

EventCallback = Callable[[dict[str, Any]], None]


class EngineError(CoreError):
    """A request came back ``ok:false``; carries the spec error code."""

    def __init__(self, code: str, message: str, retryable: bool = False) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message
        self.retryable = retryable


class EngineClient:
    """A running engine, driven by requests and an event callback.

    Usage::

        with EngineClient("/path/to/profile", on_event=show) as client:
            client.start()                # hello handshake
            client.auth_start(api_id, api_hash, db_key)

    ``on_event`` is called on the reader thread with each event dict; it must
    be cheap or hand off to the UI's own queue.
    """

    def __init__(
        self,
        profile_dir: str,
        on_event: EventCallback | None = None,
        *,
        exe: str | None = None,
        argv: list[str] | None = None,
    ) -> None:
        self._profile_dir = str(profile_dir)
        self._on_event = on_event
        self._exe = exe or str(config.core_exe())
        #: A full command to run instead of ``<exe> serve <profile>`` - used by
        #: tests to substitute a scripted engine.
        self._argv = list(argv) if argv is not None else None
        self._proc: subprocess.Popen | None = None
        self._next_id = 1
        self._id_lock = threading.Lock()
        self._pending: dict[int, dict[str, Any]] = {}
        self._waiters: dict[int, threading.Condition] = {}
        self._reader: threading.Thread | None = None
        self._write_lock = threading.Lock()
        self._closed = threading.Event()
        self._exit_error: str | None = None
        self.engine_version = ""

    def set_event_handler(self, on_event: EventCallback | None) -> None:
        """Replaces the event callback (used by the CLI's login renderer)."""
        self._on_event = on_event

    def wait_for_close(self, timeout: float, interval: float = 0.25) -> bool:
        """Waits up to ``timeout`` seconds for the engine to exit.

        Returns True when the engine has stopped. This lets a render loop
        wait on events without busy-spinning on a raw condition.
        """
        waited = 0.0
        while waited < timeout:
            if self._closed.is_set() or self._proc is None:
                return True
            if interval > 0:
                self._closed.wait(min(interval, timeout - waited))
            waited += interval
        return self._closed.is_set() or self._proc is None

    # ---- lifecycle ---------------------------------------------------

    def start(self) -> EngineClient:
        """Spawns ``serve`` and completes the ``hello`` handshake.

        Raises :class:`CoreError` when the engine is missing or busy
        (``PROFILE_BUSY`` tells the caller another window owns the profile).
        """
        argv = self._argv or [self._exe, "serve", self._profile_dir]
        try:
            self._proc = subprocess.Popen(
                argv,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=None,  # engine diagnostics go to the terminal
                text=True,
                encoding="utf-8",
                bufsize=1,
            )
        except OSError as exc:
            raise CoreError(f"could not start {argv[0]}: {exc}") from exc

        self._reader = threading.Thread(
            target=self._read_loop, name="tbox-engine-reader", daemon=True
        )
        self._reader.start()

        try:
            result = self.request("hello", {"protocol": PROTOCOL_VERSION})
        except EngineError as exc:
            # serve reports a busy profile before it accepts any command
            self._shutdown_process()
            if exc.code == "PROFILE_BUSY":
                raise CoreError(
                    "another tbox engine already owns this profile"
                ) from exc
            raise
        except CoreError:
            self._shutdown_process()
            raise
        self.engine_version = result.get("engine_version", "")
        return self

    def close(self, timeout: float = 5.0) -> int | None:
        """Asks the engine to shut down and waits for the process to exit."""
        if self._proc is None:
            return None
        try:
            if not self._closed.is_set():
                self.request("engine.shutdown", timeout=timeout)
        except (EngineError, CoreError, TimeoutError):
            pass
        return self._shutdown_process(timeout)

    def _shutdown_process(self, timeout: float = 5.0) -> int | None:
        proc, self._proc = self._proc, None
        if proc is None:
            return None
        self._closed.set()
        if proc.stdin is not None and not proc.stdin.closed:
            try:
                proc.stdin.close()
            except OSError:
                pass
        try:
            code = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            code = proc.wait()
        if self._reader is not None and self._reader.is_alive():
            self._reader.join(timeout=2.0)
        for stream in (proc.stdout, proc.stderr):
            if stream is not None and not stream.closed:
                stream.close()
        # wake anyone still waiting
        with self._id_lock:
            for cond in self._waiters.values():
                with cond:
                    cond.notify_all()
        return code

    def __enter__(self) -> EngineClient:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    # ---- reader thread ----------------------------------------------

    def _read_loop(self) -> None:
        proc = self._proc
        if proc is None or proc.stdout is None:
            return
        for line in proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                continue
            if not isinstance(msg, dict):
                continue
            self._dispatch(msg)
        self._closed.set()
        # the process ended: unblock every waiter
        with self._id_lock:
            for cond in self._waiters.values():
                with cond:
                    cond.notify_all()

    def _dispatch(self, msg: dict[str, Any]) -> None:
        if "event" in msg:
            if self._on_event is not None:
                try:
                    self._on_event(msg)
                except Exception:
                    pass  # a broken callback must not kill the reader
            return
        mid = msg.get("id")
        if not isinstance(mid, int):
            return
        with self._id_lock:
            self._pending[mid] = msg
            cond = self._waiters.get(mid)
        if cond is not None:
            with cond:
                cond.notify_all()

    # ---- requests ----------------------------------------------------

    def request(
        self,
        cmd: str,
        args: dict[str, Any] | None = None,
        timeout: float = DEFAULT_TIMEOUT,
    ) -> dict[str, Any]:
        """Sends one command, waits for its response, returns ``result``.

        Raises :class:`EngineError` for a structured failure and
        :class:`CoreError` when the engine is gone or does not answer.
        """
        if self._proc is None or self._proc.stdin is None:
            raise CoreError("the engine is not running")

        with self._id_lock:
            mid = self._next_id
            self._next_id += 1
            cond = threading.Condition()
            self._waiters[mid] = cond

        payload: dict[str, Any] = {"id": mid, "cmd": cmd}
        if args is not None:
            payload["args"] = args
        line = json.dumps(payload)

        try:
            with self._write_lock:
                self._proc.stdin.write(line + "\n")
                self._proc.stdin.flush()
        except (OSError, ValueError) as exc:
            self._forget(mid)
            raise CoreError(f"could not talk to the engine: {exc}") from exc

        with cond:
            ok = cond.wait_for(lambda: mid in self._pending or self._closed.is_set(),
                               timeout=timeout)
        if not ok:
            self._forget(mid)
            raise CoreError(f"the engine did not answer {cmd!r} in time")

        with self._id_lock:
            msg = self._pending.pop(mid, None)
            self._waiters.pop(mid, None)
        if msg is None:
            raise CoreError("the engine stopped before it answered")

        if msg.get("ok"):
            result = msg.get("result")
            return result if isinstance(result, dict) else {}
        err = msg.get("error") or {}
        raise EngineError(
            str(err.get("code", "INTERNAL")),
            str(err.get("message", "")),
            bool(err.get("retryable", False)),
        )

    def _forget(self, mid: int) -> None:
        with self._id_lock:
            self._pending.pop(mid, None)
            self._waiters.pop(mid, None)

    # ---- the phase-0 commands ---------------------------------------

    def auth_start(self, api_id: str, api_hash: str, db_key: str | None = None) -> None:
        """Begins authorization; auth events follow (spec 6.4).

        The secrets travel only inside this message (spec S2).
        """
        args: dict[str, Any] = {"api_id": str(api_id), "api_hash": api_hash}
        if db_key:
            args["db_key"] = db_key
        self.request("auth.start", args)

    def auth_password(self, password: str) -> None:
        """Supplies the two-step (2FA) password, then ``{}``."""
        self.request("auth.password", {"password": password})

    def auth_logout(self) -> None:
        """Logs out on Telegram; ``auth.closed`` follows (spec S10)."""
        self.request("auth.logout")

    def status(self) -> dict[str, Any]:
        """``auth_state``, ``conn_state``, ``cooldown_until``, ``pending_ops``."""
        return self.request("engine.status")
