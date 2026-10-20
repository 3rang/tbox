# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""The look of the app: one dark, flat theme in one place.

Colours and spacing live here so every screen matches without repeating
values, and so a future light theme is a second dictionary rather than a
sweep through the widgets.
"""

from __future__ import annotations

from PySide6.QtGui import QColor, QFont, QPalette
from PySide6.QtWidgets import QApplication

BG = "#0e1015"
CARD = "#161a22"
FIELD = "#0b0d12"
BORDER = "#252a36"
TEXT = "#e9ecf3"
MUTED = "#8c95aa"
ACCENT = "#4f8cff"
ACCENT_TEXT = "#07101f"
DANGER = "#ff6b6b"
OK = "#3ddc97"

_TOKENS = {
    "@BG": BG,
    "@CARD": CARD,
    "@FIELD": FIELD,
    "@BORDER": BORDER,
    "@TEXT": TEXT,
    "@MUTED": MUTED,
    "@ACCENT": ACCENT,
    "@ACCENT_TEXT": ACCENT_TEXT,
    "@DANGER": DANGER,
    "@OK": OK,
}

_QSS = """
QWidget { background: @BG; color: @TEXT; }

#Card {
    background: @CARD;
    border: 1px solid @BORDER;
    border-radius: 18px;
}
#CardPlain { background: transparent; border: none; }

#Title { font-size: 21px; font-weight: 600; }
#Subtitle { color: @MUTED; font-size: 12px; }
#Status { color: @MUTED; font-size: 11px; }
#Muted { color: @MUTED; font-size: 11px; }
#Danger { color: @DANGER; font-size: 11px; }
#Ok { color: @OK; font-size: 11px; }

QLabel#Code {
    background: #ffffff;
    border-radius: 14px;
    padding: 12px;
}
QLabel#Badge { font-size: 40px; }

QLineEdit {
    background: @FIELD;
    border: 1px solid @BORDER;
    border-radius: 9px;
    padding: 10px 12px;
    selection-background-color: @ACCENT;
}
QLineEdit:focus { border: 1px solid @ACCENT; }
QLineEdit::placeholder { color: @MUTED; }

QPushButton {
    background: #1d222e;
    border: 1px solid @BORDER;
    border-radius: 9px;
    padding: 10px 16px;
}
QPushButton:hover { background: #242a39; }
QPushButton:pressed { background: #1a1f2a; }
QPushButton:disabled { color: @MUTED; background: #141821; }

QPushButton#Primary {
    background: @ACCENT;
    border: none;
    color: @ACCENT_TEXT;
    font-weight: 600;
}
QPushButton#Primary:hover { background: #6ea0ff; }
QPushButton#Primary:pressed { background: #3f7aeb; }
QPushButton#Primary:disabled { background: #2c3a55; }

QPushButton#Link {
    background: transparent;
    border: none;
    color: @ACCENT;
    padding: 4px;
    text-align: left;
}
QPushButton#Link:hover { color: #6ea0ff; }

QCheckBox { color: @MUTED; font-size: 11px; spacing: 6px; }
QCheckBox::indicator {
    width: 14px; height: 14px;
    border: 1px solid @BORDER; border-radius: 4px; background: @FIELD;
}
QCheckBox::indicator:checked { background: @ACCENT; border: 1px solid @ACCENT; }

QProgressBar {
    background: #1b202c;
    border: none;
    border-radius: 3px;
    height: 6px;
}
QProgressBar::chunk { background: @ACCENT; border-radius: 3px; }

QToolTip {
    background: @CARD;
    color: @TEXT;
    border: 1px solid @BORDER;
    padding: 6px;
}
"""


def apply(app: QApplication) -> None:
    """Give the whole application the dark theme."""
    app.setStyle("Fusion")

    palette = QPalette()
    palette.setColor(QPalette.ColorRole.Window, QColor(BG))
    palette.setColor(QPalette.ColorRole.Base, QColor(FIELD))
    palette.setColor(QPalette.ColorRole.AlternateBase, QColor(CARD))
    palette.setColor(QPalette.ColorRole.Text, QColor(TEXT))
    palette.setColor(QPalette.ColorRole.WindowText, QColor(TEXT))
    palette.setColor(QPalette.ColorRole.ButtonText, QColor(TEXT))
    palette.setColor(QPalette.ColorRole.Highlight, QColor(ACCENT))
    palette.setColor(QPalette.ColorRole.HighlightedText, QColor(ACCENT_TEXT))
    palette.setColor(QPalette.ColorRole.ToolTipBase, QColor(CARD))
    palette.setColor(QPalette.ColorRole.ToolTipText, QColor(TEXT))
    app.setPalette(palette)

    app.setFont(QFont(_font_family(), 10))
    app.setStyleSheet(stylesheet())


def stylesheet() -> str:
    """The QSS with the colour tokens filled in."""
    css = _QSS
    for token, colour in _TOKENS.items():
        css = css.replace(token, colour)
    return css


def _font_family() -> str:
    """Segoe UI on Windows; whatever the platform considers normal elsewhere."""
    return "Segoe UI"