# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
"""Draw the app icon. Run it, get tbox.ico next to this file.

The mark is a lowercase "t" on a rounded square: the same shape everywhere,
drawn large and downscaled so the small sizes stay crisp. Written here rather
than committed as a binary, so the icon is version-controlled source like
everything else.

    python front/packaging/make_icon.py [output.ico]
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw

ACCENT = (79, 140, 255)       # the same blue the window uses for its buttons
ACCENT_DARK = (58, 116, 232)  # a deeper band across the bottom third
INK = (255, 255, 255)

CANVAS = 1024  # drawn big, then shrunk with smoothing
RADIUS = 230


def _tile(image: Image.Image) -> None:
    """The rounded square, with a darker bottom band that reads as a box."""
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((0, 0, CANVAS - 1, CANVAS - 1), radius=RADIUS, fill=ACCENT)

    # the band is a second rounded tile, cut down to its bottom third and
    # pasted back so the corners stay round
    band = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    band_draw = ImageDraw.Draw(band)
    band_draw.rounded_rectangle((0, 0, CANVAS - 1, CANVAS - 1), radius=RADIUS, fill=ACCENT_DARK)
    band_draw.rectangle((0, 0, CANVAS - 1, 700), fill=(0, 0, 0, 0))
    image.paste(band, (0, 0), band)


def _letter_t(image: Image.Image) -> None:
    """A lowercase t: a stem and a crossbar, both with rounded ends."""
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((452, 168, 572, 862), radius=60, fill=INK)  # stem
    draw.rounded_rectangle((268, 292, 756, 412), radius=60, fill=INK)  # crossbar


def make_icon(out: Path) -> Path:
    """Write ``out`` as a multi-size .ico and return its path."""
    art = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    _tile(art)
    _letter_t(art)

    art = art.resize((256, 256), Image.Resampling.LANCZOS)
    out.parent.mkdir(parents=True, exist_ok=True)
    art.save(out, format="ICO", sizes=[(16, 16), (24, 24), (32, 32), (48, 48),
                                       (64, 64), (128, 128), (256, 256)])
    return out


if __name__ == "__main__":
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).with_name("tbox.ico")
    print(make_icon(target))
