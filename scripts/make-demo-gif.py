# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Record the login flow as an animated GIF for the README.

The window is the real one and the code path is the real one (the same
_save_credentials / _submit_password the buttons call); only the engine is
scripted, so no account, no network and no phone are involved. Regenerate
it whenever the screens change:

    .venv\\Scripts\\python scripts\\make-demo-gif.py

Output: docs/login-demo.gif
"""

from __future__ import annotations

import io
import os
import sys
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PIL import Image
from PySide6.QtCore import QBuffer, QIODevice, QObject, Signal
from PySide6.QtWidgets import QApplication

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "front" / "src"))

import tbox.gui.window as window_module
from tbox.gui import theme
from tbox.gui.window import MainWindow

# Frames are captured at the window's real size. Shrinking them sounds
# tidier but loses two ways at once: thin text (the status line, the
# password dots) blends away, and the dithering that blending causes makes
# the file larger, not smaller - 70% came out at 91 KB against 61 KB here.

# demo values only: the engine behind them is scripted, so nothing here
# is a credential worth protecting
API_ID = "35982092"
API_HASH = "0123456789abcdef0123456789abcdef"
ENV_FILE = r"C:\Users\you\AppData\Local\tbox\.env"
PASSWORD_CHUNKS = ("s", "s3c", "s3cret")


class FakeWorker(QObject):
    """Stands in for the engine thread: same signals, no subprocess."""

    event = Signal(dict)
    failed = Signal(str)

    def __init__(self, *args, **kwargs) -> None:
        super().__init__()

    def start(self) -> None:
        pass

    def send(self, line: str) -> None:
        pass

    def stop(self, timeout_ms: int = 0) -> None:
        pass


def main() -> int:
    window_module.config.save_env = lambda values, path=None: Path(ENV_FILE)
    window_module.EngineWorker = FakeWorker

    app = QApplication(sys.argv[:1])
    theme.apply(app)
    win = MainWindow(autostart=False)
    win.resize(560, 680)
    win.show()

    shots: list[tuple[Image.Image, int]] = []

    def snap(hold_ms: int) -> None:
        """One frame of whatever is on screen right now."""
        app.processEvents()
        qt_buffer = QBuffer()
        qt_buffer.open(QIODevice.OpenModeFlag.WriteOnly)
        win.grab().save(qt_buffer, "PNG")
        qt_buffer.close()
        shots.append((Image.open(io.BytesIO(bytes(qt_buffer.data()))).convert("RGB"),
                      hold_ms))

    # 1. first run: the keys screen, then the two values being typed
    win.show_page("credentials")
    win.status("First run: the app keys are needed once.")
    snap(1100)
    for length in (2, 4, 6, 8):
        win.id_field.setText(API_ID[:length])
        snap(180)
    for start in (0, 8, 16, 24):
        win.hash_field.setText(API_HASH[: start + 8])
        snap(170)
    snap(450)

    # 2. "Save and continue" - the real handler, engine stubbed out
    win._save_credentials()
    snap(1300)

    # 3. the QR, drawn from the same link format the engine sends, then a
    #    second one to show the code refreshing by itself
    win.handle_event({"event": "qr_link", "link": "tg://login?token=DEMOFIRSTTOKEN"})
    win.status("Waiting for you to scan the code...")
    snap(1900)
    win.handle_event({"event": "qr_link", "link": "tg://login?token=DEMONEWTOKEN42"})
    win.status("Waiting for you to scan the newest code...")
    snap(1900)

    # 4. two-step verification: the hint, then the password typed hidden
    win.handle_event({"event": "password_needed", "hint": "your first pet's name"})
    snap(1100)
    for chunk in PASSWORD_CHUNKS:
        win.password_field.setText(chunk)
        snap(240)

    # 5. submitted: the screen greys out while it checks
    win._submit_password()
    snap(900)

    # 6. in
    win.handle_event({"event": "ready", "user": "Tarang Patel"})
    snap(1900)
    win.handle_event({"event": "closed"})
    snap(1600)

    frames = [image for image, _ in shots]
    durations = [hold for _, hold in shots]

    out = ROOT / "docs" / "login-demo.gif"
    out.parent.mkdir(exist_ok=True)
    palette = _palette(frames)
    quantised = [frame.quantize(palette=palette, dither=Image.Dither.NONE)
                 for frame in frames]
    quantised[0].save(
        out,
        save_all=True,
        append_images=quantised[1:],
        duration=durations,
        loop=0,
        optimize=True,
    )

    size_kb = out.stat().st_size // 1024
    seconds = sum(durations) / 1000
    print(f"{out.relative_to(ROOT)}  {len(frames)} frames, {seconds:.1f}s, {size_kb} KB")
    return 0


def _palette(frames: list[Image.Image]) -> Image.Image:
    """One palette for every frame, so the animation does not flicker.

    Sampled from the first and last frames as well as the middle: colours
    that only appear at the end (the green "session closed" line) are
    otherwise absent from the palette and get remapped to something else.
    """
    picks = [frames[0], *frames[1:-1: max(1, len(frames) // 4)], frames[-1]]
    width, height = picks[0].size
    sheet = Image.new("RGB", (width, height * len(picks)))
    for index, frame in enumerate(picks):
        sheet.paste(frame, (0, index * height))
    return sheet.quantize(colors=256, method=Image.MEDIANCUT)


if __name__ == "__main__":
    raise SystemExit(main())
