#!/usr/bin/env python3
"""Compare candidate TrueType fonts at the terminal's fixed 8-pixel cell width.

The framebuffer gives every glyph exactly 8 pixels of width (80 columns x 8
pixels = 640). Most scaled TrueType faces lose their counters and merge stems
at that size, so this tool renders sample text as ASCII art at each supported
cell height, letting a font be judged by eye before it is baked in.

Usage:
    python tools/font_preview.py [sample_text]
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

CELL_WIDTH = 8
CELL_HEIGHTS = [12, 16, 20]
SAMPLE = "Wg@m3#"


def pick_size(font_path, cell_height):
    """Largest size whose line box and advance both fit the cell."""
    best = None
    for size in range(4, cell_height + 16):
        try:
            font = ImageFont.truetype(font_path, size)
        except OSError:
            return None
        ascent, descent = font.getmetrics()
        if ascent + descent > cell_height:
            continue
        if font.getlength("M") > CELL_WIDTH:
            continue
        best = (size, font, ascent, descent)
    return best


def render(font, ascent, descent, cell_height, text, threshold=128):
    """Return the sample text as a list of ASCII-art rows."""
    baseline = (cell_height - (ascent + descent)) // 2 + ascent
    rows = []
    for gy in range(cell_height):
        line = ""
        for character in text:
            advance = int(round(font.getlength(character)))
            x_offset = max(0, (CELL_WIDTH - advance) // 2)
            image = Image.new("L", (CELL_WIDTH, cell_height), 0)
            ImageDraw.Draw(image).text(
                (x_offset, baseline), character, font=font, fill=255, anchor="ls"
            )
            for gx in range(CELL_WIDTH):
                line += "#" if image.getpixel((gx, gy)) >= threshold else "."
            line += " "
        rows.append(line)
    return rows


def main():
    text = sys.argv[1] if len(sys.argv) > 1 else SAMPLE

    candidates = [
        ("VT323 (DEC VT320)", r"tools\fonts\VT323-Regular.ttf"),
        ("Consolas", r"C:\Windows\Fonts\consola.ttf"),
        ("Cascadia Mono", r"C:\Windows\Fonts\CascadiaMono.ttf"),
        ("Lucida Console", r"C:\Windows\Fonts\lucon.ttf"),
        ("Courier New", r"C:\Windows\Fonts\cour.ttf"),
        ("Courier New Bold", r"C:\Windows\Fonts\courbd.ttf"),
        ("Segoe UI", r"C:\Windows\Fonts\segoeui.ttf"),
        ("Tahoma", r"C:\Windows\Fonts\tahoma.ttf"),
    ]

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    for name, relative in candidates:
        path = os.path.join(root, relative) if not os.path.isabs(relative) else relative
        if not os.path.exists(path):
            print("== %s: NOT FOUND ==" % name)
            continue

        print("== %s ==" % name)
        for cell_height in CELL_HEIGHTS:
            picked = pick_size(path, cell_height)
            if picked is None:
                print("  %dpx: no size fits" % cell_height)
                continue
            size, font, ascent, descent = picked
            print("  %dpx cell (%dpt):" % (cell_height, size))
            for row in render(font, ascent, descent, cell_height, text):
                print("    " + row)
        print()


if __name__ == "__main__":
    main()
