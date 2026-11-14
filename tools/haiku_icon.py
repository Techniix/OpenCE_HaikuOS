#!/usr/bin/env python3
"""Writes the Haiku package's icon, port/haiku/package/OpenCE.hvif: Haiku's
vector icon format (HVIF, Icon-O-Matic's "flat icon"), which Tracker and the
Deskbar draw at any size. It is a simplified redraw of OpenCE's icon
(port/assets/icon/opence-icon-256.png) in flat polygons, on HVIF's 64x64
grid.

    python3 tools/haiku_icon.py [preview.png]

With a file name it also draws the polygons to a 256x256 PNG (Pillow), to
look at the icon without Haiku.

The format, as Haiku's reader takes it (src/libs/icon/flat_icon,
FlatIconImporter.cpp):
- the magic 'ficn' as a little-endian uint32 ("ncif");
- styles: a count, then each a type (1: solid colour, red green blue alpha);
- paths: a count, then each its flags (closed, no curves), its point count
  and its points, each coordinate one byte (whole numbers -32..95, stored
  plus 32) or two (high bit set; (value + 128) * 102);
- shapes: a count, then each a type (10: path source), a style index, a
  path count, the paths' indices and flags (none).
"""

import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "port/haiku/package/OpenCE.hvif"

STYLE_TYPE_SOLID_COLOR = 1
SHAPE_TYPE_PATH_SOURCE = 10
PATH_FLAG_CLOSED = 1 << 1
PATH_FLAG_NO_CURVES = 1 << 3

# (colour, polygon), drawn in order, back to front
SHAPES = [
    # the background, the icon's dark blue
    ((14, 34, 76, 255), [(4, 4), (60, 4), (60, 60), (4, 60)]),
    ((22, 52, 104, 255), [(4, 4), (60, 4), (60, 22), (4, 38)]),
    # the helmet
    ((236, 238, 240, 255), [(8, 25), (12, 12), (23, 5), (39, 4), (50, 9), (55, 19), (53, 30),
                            (57, 40), (51, 51), (42, 58), (38, 60), (4, 60), (4, 44), (10, 40)]),
    # its shaded side and chin
    ((176, 182, 190, 255), [(50, 9), (55, 19), (53, 30), (57, 40), (51, 51), (42, 58), (38, 60),
                            (30, 60), (40, 47), (46, 38), (48, 27), (46, 14)]),
    ((150, 156, 166, 255), [(4, 44), (10, 40), (16, 42), (20, 52), (16, 60), (4, 60)]),
    # the visor, gold, with its darker lower half and a highlight
    ((214, 150, 30, 255), [(12, 26), (22, 20), (38, 20), (47, 23), (45, 34), (35, 41), (22, 41),
                           (14, 36)]),
    ((150, 92, 14, 255), [(14, 36), (30, 31), (46, 27), (45, 34), (35, 41), (22, 41)]),
    ((252, 214, 120, 255), [(17, 25), (23, 22), (30, 22), (20, 29)]),
]


def coordinate(value: float) -> bytes:
    if value == int(value) and -32 <= value <= 95:
        return bytes([int(value) + 32])
    stored = int((value + 128.0) * 102.0) | 0x8000
    return bytes([stored >> 8, stored & 0xFF])


def hvif() -> bytes:
    data = bytearray(struct.pack("<I", 0x6669636E))  # 'ficn'
    data.append(len(SHAPES))
    for colour, _ in SHAPES:
        data.append(STYLE_TYPE_SOLID_COLOR)
        data.extend(bytes(colour))
    data.append(len(SHAPES))
    for _, points in SHAPES:
        data.append(PATH_FLAG_CLOSED | PATH_FLAG_NO_CURVES)
        data.append(len(points))
        for x, y in points:
            data.extend(coordinate(x) + coordinate(y))
    data.append(len(SHAPES))
    for index in range(len(SHAPES)):
        data.extend([SHAPE_TYPE_PATH_SOURCE, index, 1, index, 0])
    return bytes(data)


def main() -> int:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(hvif())
    print(f"{OUTPUT.relative_to(ROOT)}: {OUTPUT.stat().st_size} bytes")
    if len(sys.argv) > 1:
        from PIL import Image, ImageDraw
        image = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        for colour, points in SHAPES:
            draw.polygon([(x * 4, y * 4) for x, y in points], fill=colour)
        image.save(sys.argv[1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
