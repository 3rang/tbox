# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Drawing a QR code in a terminal.

The engine hands over a ``tg://login?token=...`` link (never a picture); this
module turns it into characters you can point a phone at. Two renderings are
offered, and the terminal's encoding decides which one is safe:

* half-height blocks (``▀ ▄ █``) - compact and square, needs a UTF-8 console
* plain ASCII (``##`` and spaces) - works anywhere, twice as wide

Modules stay square on screen because a terminal cell is about twice as tall
as it is wide, so one half-block cell covers exactly one module.
"""

from __future__ import annotations

import sys

import qrcode
import qrcode.constants

_BLOCKS = {
    (False, False): " ",
    (True, False): "▀",  # dark on top
    (False, True): "▄",  # dark below
    (True, True): "█",
}


def matrix(text: str, border: int = 2) -> list[list[bool]]:
    """The QR modules for ``text``; ``True`` means dark."""
    code = qrcode.QRCode(border=border, error_correction=qrcode.constants.ERROR_CORRECT_L)
    code.add_data(text)
    code.make(fit=True)
    return code.get_matrix()


def render(text: str, border: int = 2) -> str:
    """``text`` as a scannable block of characters, using the best the
    terminal's encoding can print."""
    rows = matrix(text, border)
    if _encodable("█▀▄"):
        return blocks(rows)
    return ascii_art(rows)


def blocks(rows: list[list[bool]]) -> str:
    """Two module rows per line, as half-height blocks."""
    lines = []
    blank = [False] * len(rows[0]) if rows else []
    for y in range(0, len(rows), 2):
        top = rows[y]
        bottom = rows[y + 1] if y + 1 < len(rows) else blank
        lines.append(
            "".join(_BLOCKS[(bool(t), bool(b))] for t, b in zip(top, bottom))
        )
    return "\n".join(lines)


def ascii_art(rows: list[list[bool]]) -> str:
    """The fallback: two characters per module, one line per module row."""
    return "\n".join("".join("##" if cell else "  " for cell in row) for row in rows)


def _encodable(text: str) -> bool:
    """Can the terminal actually print these characters?"""
    encoding = getattr(sys.stdout, "encoding", None) or "utf-8"
    try:
        text.encode(encoding)
    except (UnicodeEncodeError, LookupError):
        return False
    return True