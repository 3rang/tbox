# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Starting the desktop app, and checking a packaged build.

``--selftest`` exists for the build script: a windowed .exe has no console to
print to, so the app writes a short report to a file and exits with a code.
It proves three things about a packaged build: the window comes up, the
engine binary was found, and that engine can really talk to TDLib.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile
import time

from .. import config, qr
from ..core import Core
from ..errors import TboxError
from . import theme
from .window import MainWindow


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="tbox", description="tbox desktop app")
    parser.add_argument("--selftest", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--report", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest(args.report)

    from PySide6.QtWidgets import QApplication

    app = QApplication(sys.argv[:1])
    app.setApplicationName("tbox")
    app.setApplicationDisplayName("tbox")
    theme.apply(app)

    window = MainWindow()
    window.show()
    return app.exec()


def selftest(report_path: str | None) -> int:
    """Headless check of a packaged build. Returns 0 when everything works."""
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    from PySide6.QtWidgets import QApplication

    lines: list[str] = []
    ok = True

    def note(text: str) -> None:
        lines.append(text)

    try:
        app = QApplication(sys.argv[:1])
        theme.apply(app)
        window = MainWindow(autostart=False)
        note(f"window: {type(window).__name__}, pages={len(window.pages)}")

        # the event -> screen mapping, without a network
        window.handle_event({"event": "qr_link", "link": "tg://login?token=SELFTEST"})
        rows = qr.matrix("tg://login?token=SELFTEST")
        drawn = window.qr_label.pixmap()
        note(f"qr: page={window.current_page} modules={len(rows)} "
             f"pixmap={drawn.width()}x{drawn.height()}")
        ok = ok and window.current_page == "qr" and drawn.width() > 0

        # the engine binary, and whether it truly runs
        engine = config.core_exe()
        note(f"engine: {engine}")
        note(f"frozen: {bool(getattr(sys, 'frozen', False))}")
    except Exception as exc:  # a report is worth more than a traceback here
        note(f"failed: {type(exc).__name__}: {exc}")
        ok = False
    else:
        ok = _engine_runs(note) and ok

    if report_path:
        with open(report_path, "w", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n")
    return 0 if ok else 1


def _engine_runs(note) -> bool:
    """Start the real engine with deliberately wrong keys: the point is that
    it runs at all (DLLs found, event stream readable), not that it logs in."""
    work = tempfile.mkdtemp(prefix="tbox-selftest-")
    try:
        with Core.start_login(work, "123", "bogus") as core:
            deadline = time.monotonic() + 30
            for event in core.events():
                if event.get("event") == "error":
                    note(f"engine event: {event}")
                    return True
                if time.monotonic() > deadline:
                    break
        note("engine: no error event within 30s")
        return False
    except TboxError as exc:
        note(f"engine failed: {exc}")
        return False
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())