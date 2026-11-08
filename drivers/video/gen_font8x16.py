#!/usr/bin/env python3
"""Generate the 8x16 terminal font bitmap from JetBrains Mono Regular.

drivers/video/font8x16.h is committed generated output; this script is its
source of truth. The pinned upstream release keeps regeneration reproducible.
Redistributing the rendered bitmaps requires the license file alongside them
(drivers/video/OFL.txt).

Requires Pillow (pip install pillow). sim/tests/terminal_smoke.c compares
rendered cells against the header, so rerun the simulator tests afterwards.

Usage:
    python drivers/video/gen_font8x16.py            # download the pinned TTF
    python drivers/video/gen_font8x16.py --ttf PATH # reuse a local TTF copy
"""

from __future__ import annotations

import argparse
import pathlib
import tempfile
import urllib.request

FONT_URL = (
    "https://raw.githubusercontent.com/JetBrains/JetBrainsMono/"
    "v2.304/fonts/ttf/JetBrainsMono-Regular.ttf"
)

CELL_WIDTH, CELL_HEIGHT = 8, 16
ASCII_FIRST = 32
CHARS = [chr(c) for c in range(ASCII_FIRST, 127)]
DESCENDER_CHARS = "gpqyj(),[]{}|_@"  # glyphs that reach the lowest rows

# Sizes tried from largest to smallest; the first one where every glyph fits
# in a cell without horizontal scaling wins.
SIZE_CANDIDATES = range(20, 11, -1)
INK_THRESHOLD = 128  # anti-aliased coverage that counts as ink
MIN_SIZE = 13  # below this the generated font gets too small to read

HEADER = pathlib.Path(__file__).with_name("font8x16.h")


class Glyph:
    def __init__(self, char, rows, bearing, top, bottom):
        self.char = char
        self.rows = rows  # tuples of 0/1 ink, as rendered, tied to the baseline
        self.bearing = bearing  # hinted left side bearing relative to the pen
        self.top = top  # row above the baseline of the first ink (negative)
        self.bottom = bottom  # row below the baseline of the last ink


def probe(font, char):
    """Render one glyph and measure its thresholded ink around the baseline."""
    from PIL import Image, ImageDraw

    ascent, descent = font.getmetrics()
    pen, baseline = 6, ascent + 2
    scratch = Image.new("L", (CELL_WIDTH + 12, ascent + descent + 6), 0)
    ImageDraw.Draw(scratch).text((pen, baseline), char, font=font, fill=255,
                                 anchor="ls")
    pixels = scratch.load()
    ink = [(x, y) for y in range(scratch.height) for x in range(scratch.width)
           if pixels[x, y] >= INK_THRESHOLD]
    if not ink:  # space renders nothing and keeps the baseline placement
        return Glyph(char, (), 0, 0, 0)
    left, right = min(x for x, _ in ink), max(x for x, _ in ink)
    top, bottom = min(y for _, y in ink), max(y for _, y in ink)
    rows = tuple(
        tuple(1 if pixels[x, y] >= INK_THRESHOLD else 0
              for x in range(left, right + 1))
        for y in range(top, bottom + 1)
    )
    return Glyph(char, rows, left - pen, top - baseline, bottom - baseline)


def evaluate(size, font_path):
    """Return cell placements for this size, or None if any glyph overflows."""
    from PIL import ImageFont

    font = ImageFont.truetype(str(font_path), size)
    glyphs = {glyph.char: glyph for glyph in (probe(font, c) for c in CHARS)}
    if any(len(glyph.rows) and len(glyph.rows[0]) > CELL_WIDTH
           for glyph in glyphs.values()):
        return None
    descent = max(glyphs[char].bottom for char in DESCENDER_CHARS)
    topmost = min(glyph.top for glyph in glyphs.values() if glyph.rows)
    baseline = CELL_HEIGHT - 1 - descent  # descenders end on the last row
    if baseline + topmost < 0:
        return None
    placements = {}
    adjusted = []
    for char in CHARS:
        glyph = glyphs[char]
        if not glyph.rows:
            placements[char] = ((), 0, 0)
            continue
        width = len(glyph.rows[0])
        offset = min(max(glyph.bearing, 0), CELL_WIDTH - width)
        if offset != glyph.bearing:
            adjusted.append(char)
        placements[char] = (glyph.rows, offset, glyph.top)
    return {
        "size": size,
        "advance": font.getlength("W"),
        "baseline": baseline,
        "placements": placements,
        "adjusted": adjusted,
        "topmost": baseline + topmost,
        "descent": descent,
    }


def choose_font(font_path):
    """Pick the largest usable size and reject tiny or distorted text."""
    matches = []
    for size in SIZE_CANDIDATES:
        match = evaluate(size, font_path)
        if match:
            matches.append(match)
    if not matches:
        raise SystemExit("no candidate size fits every glyph in one cell")
    chosen = max(matches, key=lambda item: item["size"])
    if chosen["advance"] < CELL_WIDTH - 1 or chosen["size"] < MIN_SIZE:
        raise SystemExit(
            f"chosen size {chosen['size']} (advance {chosen['advance']:.2f}) "
            "is too small; the generated font would be unreadable")
    return chosen


def cell_rows(placements, baseline):
    """Map (rows, offset, top-of-ink) placements into cell bit rows."""
    cells = {}
    for char, (rows, offset, top) in placements.items():
        bits = []
        for y in range(CELL_HEIGHT):
            rel = y - baseline
            span = 0
            if rows and top <= rel < top + len(rows):
                row = rows[rel - top]
                for x, value in enumerate(row):
                    span |= (1 if value else 0) << (x + offset)
            bits.append(span)
        cells[char] = bits
    return cells


def write_header(match, cells):
    lines = [
        "/* 8x16 bitmap font for the terminal, generated by gen_font8x16.py",
        " * from JetBrains Mono v2.304 Regular (SIL Open Font License 1.1,",
        " * see OFL.txt in this directory). Rows are top-to-bottom cell rows",
        " * with bit 0 as the leftmost column; do not edit generated bytes.",
        " */",
        "#pragma once",
        "",
        "static const uint8_t font8x16[95][16] = {",
    ]
    for code in range(ASCII_FIRST, 127):
        bits = cells[chr(code)]
        glyph = ", ".join("0x%02x" % value for value in bits)
        comment = chr(code) if code != 34 and code != 92 else "escape"
        lines.append("    {%s}, /* %s */" % (glyph, comment))
    lines.append("};")
    HEADER.write_text("\n".join(lines) + "\n", newline="\n")
    size, baseline = match["size"], match["baseline"]
    print(f"font8x16.h written: size {size}, advance "
          f"{match['advance']:.2f}, baseline row {baseline}, "
          f"ink rows {match['topmost']}..{baseline + match['descent'] - 1}, "
          f"repositioned glyphs: {', '.join(match['adjusted']) or 'none'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ttf", type=pathlib.Path, default=None,
                        help="local JetBrains Mono TTF; downloads otherwise")
    arguments = parser.parse_args()
    source = arguments.ttf
    if source is None:
        source = pathlib.Path(tempfile.gettempdir()) / "vantage-font" / \
            "JetBrainsMono-Regular.ttf"
        if not source.exists():
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(urllib.request.urlopen(FONT_URL, timeout=60).read())
            print(f"downloaded {FONT_URL}")
    if not source.exists():
        raise SystemExit(f"font file not found: {source}")
    match = choose_font(source)
    cells = cell_rows(match["placements"], match["baseline"])
    if any(min(cells[chr(code)]) != 0 for code in (32,)):
        raise SystemExit("space must not render ink")
    write_header(match, cells)


if __name__ == "__main__":
    main()
