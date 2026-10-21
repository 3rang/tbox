# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Tests for EngineClient: the long-running JSON-lines conversation.

The engine is faked with a small Python program that speaks the same protocol
(a request line in, a response or events out), so nothing here needs TDLib, an
account or the compiled binary.
"""

from __future__ import annotations

import json
import sys
import textwrap
import threading
import time
import unittest

from tbox.engine_client import EngineClient, EngineError
from tbox.errors import CoreError

# A stand-in engine: it reads requests, answers hello/engine.status/auth.* and
# can emit events. `serve` and the profile dir arrive as argv and are ignored.
FAKE_SERVE = textwrap.dedent(
    """
    import json, sys, threading, time

    def out(obj):
        sys.stdout.write(json.dumps(obj) + "\\n")
        sys.stdout.flush()

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception:
            continue
        cmd = req.get("cmd")
        rid = req.get("id")
        if cmd == "hello":
            out({"id": rid, "ok": True, "result": {"engine_version": "test", "protocol": 1}})
        elif cmd == "auth.start":
            out({"id": rid, "ok": True, "result": {}})
            out({"event": "auth.state", "data": {"state": "WAIT_QR"}})
            out({"event": "auth.qr", "data": {"link": "tg://login?token=T"}})
        elif cmd == "auth.password":
            out({"id": rid, "ok": True, "result": {}})
        elif cmd == "engine.status":
            out({"id": rid, "ok": True, "result": {"auth_state": "WAIT_QR"}})
        elif cmd == "boom":
            out({"id": rid, "ok": False, "error": {"code": "BAD_ARGS", "message": "nope", "retryable": False}})
        elif cmd == "engine.shutdown":
            out({"id": rid, "ok": True, "result": {}})
            break
        else:
            out({"id": rid, "ok": False, "error": {"code": "UNKNOWN_COMMAND", "message": "?", "retryable": False}})
    """
)


class EngineClientTest(unittest.TestCase):
    def _client(self, on_event=None) -> EngineClient:
        # hand the client a python "engine"; `serve` is just an unused arg
        argv = [sys.executable, "-c", FAKE_SERVE]
        return EngineClient("./profile", on_event=on_event, argv=argv)

    def test_hello_handshake_reports_the_protocol(self):
        client = self._client()
        client.start()
        try:
            self.assertEqual(client.engine_version, "test")
        finally:
            client.close()

    def test_events_reach_the_callback_while_requests_are_in_flight(self):
        seen: list[dict] = []
        got = threading.Event()

        def on_event(event: dict) -> None:
            seen.append(event)
            got.set()

        client = self._client(on_event=on_event)
        client.start()
        try:
            client.auth_start("1", "hash")
            self.assertTrue(got.wait(5), "no event arrived")
            names = [e["event"] for e in seen]
            self.assertIn("auth.state", names)
            self.assertIn("auth.qr", names)
            # the data travels nested under "data" (spec 6.5)
            qr = next(e for e in seen if e["event"] == "auth.qr")
            self.assertEqual(qr["data"]["link"], "tg://login?token=T")
        finally:
            client.close()

    def test_status_request_returns_the_result(self):
        client = self._client()
        client.start()
        try:
            self.assertEqual(client.status()["auth_state"], "WAIT_QR")
        finally:
            client.close()

    def test_a_structured_error_raises_EngineError(self):
        client = self._client()
        client.start()
        try:
            with self.assertRaises(EngineError) as ctx:
                client.request("boom")
            self.assertEqual(ctx.exception.code, "BAD_ARGS")
            self.assertFalse(ctx.exception.retryable)
        finally:
            client.close()

    def test_closed_engine_fails_a_request_instead_of_hanging(self):
        client = self._client()
        client.start()
        client.close()
        with self.assertRaises(CoreError):
            client.request("engine.status")

    def test_busy_profile_maps_to_a_friendly_CoreError(self):
        # an engine that answers hello with PROFILE_BUSY
        busy = textwrap.dedent(
            """
            import json, sys
            for line in sys.stdin:
                req = json.loads(line)
                sys.stdout.write(json.dumps({"id": req["id"], "ok": False,
                    "error": {"code": "PROFILE_BUSY", "message": "busy", "retryable": False}}) + "\\n")
                sys.stdout.flush()
                break
            """
        )
        exe = [sys.executable, "-c", busy]
        client = EngineClient("./profile", argv=exe)
        with self.assertRaises(CoreError) as ctx:
            client.start()
        self.assertIn("another tbox engine", str(ctx.exception))
