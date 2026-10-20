# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Turning a login link into a picture for the window.

A phone camera needs square modules and a clean light background, so the QR
is built as a small greyscale image and scaled without smoothing - blurred
edges are exactly what makes a scan fail.
"""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtGui import QImage, QPixmap

from .. import qr


def pixmap(link: str, size: int = 300) -> QPixmap:
    """A crisp, scannable QR image for ``link``."""
    rows = qr.matrix(link)
    modules = len(rows)
    image = QImage(modules, modules, QImage.Format.Format_Grayscale8)
    image.fill(255)
    for y, row in enumerate(rows):
        for x, dark in enumerate(row):
            if dark:
                image.setPixel(x, y, 0)
    return QPixmap.fromImage(
        image.scaled(size, size, Qt.AspectRatioMode.IgnoreAspectRatio,
                     Qt.TransformationMode.FastTransformation)
    )