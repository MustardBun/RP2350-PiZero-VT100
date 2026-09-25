#!/usr/bin/env python3
"""Decode a baked font table from fonts_vt100.c back to ASCII art.

This verifies what the firmware will actually display, by reading the exact
bytes that go into the build rather than the source font. Use it after running
bake-fonts.bat to confirm glyphs are legible and correctly aligned.

Usage:
    python tools/show_baked.py FontVt22020 "AWg"
"""

import re
import sys

CELL_WIDTH = 8


def load_baked(path, symbol):
    """Return the packed glyph bytes for each printable character."""
    src = open(path).read()
    start = src.index("static const uint8_t %s[] = {" % symbol)
    end = src.index("\n};", start)
    body = re.sub(r"/\*.*?\*/", "", src[start:end], flags=re.S)
    return [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]


def main():
    path = sys.argv[1]
    symbol = sys.argv[2]
    text = sys.argv[3] if len(sys.argv) > 3 else "FLASH! A>"

    # Read width, height and the table name from the generated sFONT definition,
    # so this tool always matches whatever gen_fonts.py emitted.
    src = open(path).read()
    match = re.search(
        r"const sFONT %s = \{\s*(\w+),\s*(\d+),\s*(\d+)\s*\}" % symbol, src
    )
    if not match:
        raise SystemExit("could not find sFONT %s" % symbol)
    table_symbol, width, height = match.group(1), int(match.group(2)), int(match.group(3))

    rows_per_glyph = height * ((width + 7) // 8)
    table = load_baked(path, table_symbol)

    print("%s: %dx%d cell, %d bytes/glyph" % (symbol, width, height, rows_per_glyph))
    print()

    # Render the sample text as one continuous line of pixels.
    grid = [["."] * (CELL_WIDTH * len(text)) for _ in range(height)]
    for position, character in enumerate(text):
        code = ord(character)
        if not (0x20 <= code <= 0x7E):
            continue
        offset = (code - 0x20) * rows_per_glyph
        for y in range(height):
            byte = table[offset + y] if offset + y < len(table) else 0
            for x in range(CELL_WIDTH):
                if byte & (0x80 >> x):
                    grid[y][position * CELL_WIDTH + x] = "#"

    for row in grid:
        line = "".join(row)
        if line.strip("."):
            print("  " + line)


if __name__ == "__main__":
    main()
