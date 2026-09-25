#!/usr/bin/env python3
"""Bake fonts into 1-bpp bitmap tables for the RP2350 VT100 terminal.

Two source types are supported, because they need opposite treatment:

* BDF (native bitmap, e.g. the DEC VT220 ROM dump). The glyphs are already
  pixels, so they are copied one-to-one with no scaling whatsoever. This is the
  only way to get genuinely authentic terminal glyphs: a bitmap face cannot be
  resampled without destroying it.

* TTF (outline, e.g. Consolas). These must be rasterised, and rasterising
  straight to an 8xN cell bakes in aliasing and clips the glyph, because a
  monospace face at 20 pixels tall is naturally ~12 pixels wide. Glyphs are
  therefore rendered at a large supersampled size and box-filtered down, which
  averages the outline into the available cell instead of cutting it off.

Output layout matches the firmware:

    bytes per glyph = Height * ceil(Width / 8)
    one byte per 8 pixels within a row, most significant bit first
    bit set = foreground pixel

Sources and sizes come from tools/fonts.json. Run via bake-fonts.bat.
"""

import json
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

FIRST_CHAR = 0x20
LAST_CHAR = 0x7E
CELL_WIDTH = 8

# TTF entries name their source through ${FONT_DIR} and ${FONT_SUFFIX} so that
# the shipped fonts.json is portable. Outline faces are a convenience, not a
# requirement, so these are only expanded when a path actually uses them, and
# nothing here is machine-specific.
FONT_DIR_ENV = "FONT_DIR"
FONT_SUFFIX_ENV = "FONT_SUFFIX"


def default_font_dir():
    """The platform's usual font directory, or a local fallback."""
    if sys.platform == "win32":
        windir = os.environ.get("WINDIR", r"C:\Windows")
        return os.path.join(windir, "Fonts")
    if sys.platform == "darwin":
        return "/Library/Fonts"
    return "/usr/share/fonts"


def expand_font_tokens(path, root):
    """Substitute ${FONT_DIR} / ${FONT_SUFFIX} in a configured font path."""
    if "${" not in path:
        return path
    font_dir = os.environ.get(FONT_DIR_ENV) or default_font_dir()
    suffix = os.environ.get(FONT_SUFFIX_ENV)
    if not suffix:
        suffix = ".ttf" if sys.platform == "win32" else ".ttf"
    expanded = (path
                .replace("${FONT_DIR}", font_dir)
                .replace("${FONT_SUFFIX}", suffix))
    # A relative FONT_DIR is taken relative to the repository root.
    if not os.path.isabs(expanded) and not expanded.startswith(("${",)):
        expanded = os.path.join(root, expanded)
    return os.path.normpath(expanded)

# Characters whose ink spans the cell, used to fit an outline face.
FIT_PROBE = "HWMQgjpqy@#"

# DEC Special Graphics: the "line drawing" set selected with ESC ( 0, which
# CP/M software uses to draw boxes and rules. The key is the ASCII code the
# host sends; the value is the Unicode codepoint that actually holds the shape.
# The VT220 BDF carries all of these, so the historical font yields authentic
# boxes, and the outline faces are rasterised from their Unicode equivalents.
SPECIAL_GRAPHICS = {
    0x60: 0x25C6,  # diamond
    0x61: 0x2592,  # checkerboard
    0x62: 0x2409,  # symbol for horizontal tab
    0x63: 0x240C,  # symbol for form feed
    0x64: 0x240D,  # symbol for carriage return
    0x65: 0x240A,  # symbol for line feed
    0x66: 0x00B0,  # degree
    0x67: 0x00B1,  # plus/minus
    0x68: 0x2424,  # symbol for newline
    0x69: 0x240B,  # symbol for vertical tab
    0x6A: 0x2518,  # box drawing: bottom right corner
    0x6B: 0x2510,  # box drawing: top right corner
    0x6C: 0x250C,  # box drawing: top left corner
    0x6D: 0x2514,  # box drawing: bottom left corner
    0x6E: 0x253C,  # box drawing: crossing
    0x6F: 0x23BA,  # horizontal scan line 1
    0x70: 0x23BB,  # horizontal scan line 3
    0x71: 0x2500,  # box drawing: horizontal
    0x72: 0x23BC,  # horizontal scan line 7
    0x73: 0x23BD,  # horizontal scan line 9
    0x74: 0x251C,  # box drawing: left tee
    0x75: 0x2524,  # box drawing: right tee
    0x76: 0x2534,  # box drawing: bottom tee
    0x77: 0x252C,  # box drawing: top tee
    0x78: 0x2502,  # box drawing: vertical
    0x79: 0x2264,  # less than or equal to
    0x7A: 0x2265,  # greater than or equal to
    0x7B: 0x03C0,  # pi
    0x7C: 0x2260,  # not equal to
    0x7D: 0x00A3,  # pound sterling
    0x7E: 0x00B7,  # centred dot
}

# The UK character set (ESC ( A) differs from ASCII in exactly one glyph.
UK_CHARSET = {0x23: 0x00A3}  # '#' is replaced by the pound sign

SPECIAL_FIRST = min(SPECIAL_GRAPHICS)
SPECIAL_COUNT = len(SPECIAL_GRAPHICS)
UK_COUNT = len(UK_CHARSET)


# --------------------------------------------------------------------------
# BDF (native bitmap) support
# --------------------------------------------------------------------------

def parse_bdf(path):
    """Return {codepoint: (width, height, x_offset, y_offset, [row values])}."""
    glyphs = {}
    code = None
    width = height = xo = yo = 0
    rows = []

    with open(path, encoding="latin-1") as handle:
        for raw in handle:
            line = raw.strip()
            if line.startswith("ENCODING"):
                code = int(line.split()[1])
            elif line.startswith("BBX"):
                parts = line.split()
                width, height = int(parts[1]), int(parts[2])
                xo, yo = int(parts[3]), int(parts[4])
            elif line.startswith("BITMAP"):
                rows = []
            elif line.startswith("ENDCHAR"):
                if code is not None and rows:
                    glyphs[code] = (width, height, xo, yo, list(rows))
                code = None
                rows = []
            elif code is not None and rows is not None and re.fullmatch(r"[0-9A-Fa-f]+", line):
                rows.append(line)

    return glyphs


def bdf_glyph_bitmap(entry, cell_height, cell_width):
    """Normalise one BDF glyph into cell_width x cell_height bits, MSB first.

    BDF rows are hex, right-padded to a byte boundary, and the glyph's `bbx
    width` bits are left-aligned within that field. The ink of the DEC face
    occupies exactly 8 columns, so it maps onto the cell one-to-one.
    """
    width, height, xo, yo, rows = entry

    # Rows may be padded to 8/16/32 bits; recover the padded field width.
    field_bits = max(len(row) for row in rows) * 4 if rows else width

    pixels = []
    for row in rows:
        value = int(row, 16)
        bits = bin(value)[2:].zfill(field_bits)[:width]
        pixels.append([1 if b == "1" else 0 for b in bits])

    # Vertical placement: BDF y-offset is the distance from the baseline to the
    # bottom of the bitmap, so pad at the top to sit the glyph in the cell.
    result = [[0] * cell_width for _ in range(cell_height)]
    top = max(0, (cell_height - height) // 2 + (yo + height - cell_height))
    for y in range(height):
        target_y = y + top
        if not (0 <= target_y < cell_height):
            continue
        for x in range(min(width, cell_width)):
            if pixels[y][x]:
                result[target_y][x] = 1

    return result


# --------------------------------------------------------------------------
# TTF (outline) support
# --------------------------------------------------------------------------

def ttf_ink_bounds(font):
    """Ink box of the probe set, measured from the baseline.

    getbbox() without an anchor measures from the ascender top, which is a
    different origin from what anchor="ls" uses when drawing. Asking for
    anchor="ls" here keeps both measurements in the same coordinate system, so
    the baseline computed from this box actually places the ink in the cell.
    """
    boxes = [font.getbbox(ch, anchor="ls") for ch in FIT_PROBE]
    return (
        min(b[0] for b in boxes),
        min(b[1] for b in boxes),
        max(b[2] for b in boxes),
        max(b[3] for b in boxes),
    )


def ttf_pick_size(font_path, cell_height):
    """Largest size whose measured ink fits the cell, measured at 1x.

    The chosen size is then multiplied by the supersample factor when the font
    is actually loaded, so this measurement stays independent of supersampling.
    """
    best = None
    for size in range(4, cell_height * 4):
        try:
            font = ImageFont.truetype(font_path, size)
        except OSError:
            return None
        left, top, right, bottom = ttf_ink_bounds(font)
        if bottom - top > cell_height * 0.86:
            continue
        if right - left > CELL_WIDTH:
            continue
        best = size
    return best


def ttf_best_cell_height(font_path, max_height):
    """Largest cell height a face can fill at 8 pixels of width.

    Monospace outline faces are roughly 0.6 as wide as they are tall, so an
    8-pixel-wide cell can only hold about 13 pixels of cap height. This reports
    the height the face genuinely fills, which is useful when choosing a cell
    size for a TTF.
    """
    best = None
    for height in range(8, max_height + 1):
        size = ttf_pick_size(font_path, height)
        if size is None:
            continue
        font = ImageFont.truetype(font_path, size)
        _l, top, _r, bottom = ttf_ink_bounds(font)
        if bottom - top < height - 3:
            continue
        best = height
    return best


def ttf_text_geometry(font_path, cell_height, supersample, max_condense=1.7):
    """Compute the shared rasterisation geometry for an outline face.

    Outline faces are about 0.6 as wide as they are tall, while the terminal
    cell is 8 wide by 20 tall (aspect 0.4). Fitting such a face by width alone
    leaves it filling under half the cell, which looks small and lost. Real
    terminal fonts are condensed, so the ink box is fitted to the cell in both
    axes independently - the same non-square scale for every glyph, which keeps
    the spacing and stroke weights consistent. The vertical stretch is capped so
    extreme faces do not look distorted.

    Returns (font, canvas, baseline, size) ready for ttf_draw_glyph().
    """
    target_width = CELL_WIDTH * supersample

    # Largest size whose advance still fits the supersampled cell width.
    size = None
    for candidate in range(4, 400):
        font = ImageFont.truetype(font_path, candidate)
        if font.getlength("M") > target_width:
            break
        size = candidate
    if size is None:
        raise SystemExit("no usable size for %s" % font_path)

    font = ImageFont.truetype(font_path, size)
    _left, ink_top, _right, ink_bottom = ttf_ink_bounds(font)
    ink_height = ink_bottom - ink_top
    if ink_height <= 0:
        raise SystemExit("no measurable ink for %s" % font_path)

    # A canvas whose height equals the ink height makes the ink fill the cell
    # vertically once the canvas is resized down.
    canvas_height = ink_height
    natural_ratio = (target_width / CELL_WIDTH) / (canvas_height / cell_height)
    if natural_ratio > max_condense:
        # Limit the stretch by growing the canvas, which shrinks the glyph.
        canvas_height = int(round((target_width / CELL_WIDTH) * cell_height / max_condense))

    return font, (target_width, canvas_height), -ink_top, size


def ttf_render_rows(character, font, canvas, baseline, cell_height, threshold):
    """Draw one character into the cell grid and return its pixel rows."""
    advance = font.getlength(character)
    x_offset = (canvas[0] - advance) / 2.0

    image = Image.new("L", canvas, 0)
    ImageDraw.Draw(image).text(
        (x_offset, baseline), character, font=font, fill=255, anchor="ls"
    )

    small = image.resize((CELL_WIDTH, cell_height), Image.BOX)
    small = small.point(lambda v: 255 if v >= threshold else 0, "1")
    return [[1 if small.getpixel((x, y)) else 0 for x in range(CELL_WIDTH)]
            for y in range(cell_height)]


def ttf_glyph_bitmaps(font_path, cell_height, threshold, supersample):
    """Rasterise every printable ASCII glyph into the cell."""
    font, canvas, baseline, _size = ttf_text_geometry(
        font_path, cell_height, supersample)

    return [ttf_render_rows(chr(code), font, canvas, baseline, cell_height, threshold)
            for code in range(FIRST_CHAR, LAST_CHAR + 1)]


def ttf_special_bitmaps(font_path, cell_height, threshold, supersample):
    """Rasterise the DEC special graphics set from an outline face.

    These glyphs must NOT be ink-fitted like ordinary text. A box-drawing
    rule deliberately spans the whole advance width or line height so that
    neighbouring cells join seamlessly; fitting its ink to the cell would
    stretch a thin horizontal rule into a full-height bar.

    Instead the glyph's own cell box - advance width by ascent+descent - is
    mapped onto the terminal cell, and the glyph is drawn from its origin. That
    keeps the rules spanning the full cell so the boxes connect, and preserves
    the designer's intended stroke weight.
    """
    target_width = CELL_WIDTH * supersample
    size = None
    for candidate in range(4, 400):
        font = ImageFont.truetype(font_path, candidate)
        if font.getlength("M") > target_width:
            break
        size = candidate
    if size is None:
        raise SystemExit("no usable size for %s" % font_path)

    font = ImageFont.truetype(font_path, size)
    ascent, descent = font.getmetrics()
    advance = font.getlength("M")

    canvas = (max(1, int(round(advance))), max(1, ascent + descent))
    baseline = ascent

    glyphs = []
    for ascii_code in sorted(SPECIAL_GRAPHICS):
        character = chr(SPECIAL_GRAPHICS[ascii_code])
        image = Image.new("L", canvas, 0)
        # Drawn from the origin, not centred: the ink of a rule covers exactly
        # 0..advance, so aligning the origin to the cell edge is what makes the
        # rules meet across cell boundaries.
        ImageDraw.Draw(image).text(
            (0, baseline), character, font=font, fill=255, anchor="ls")

        small = image.resize((CELL_WIDTH, cell_height), Image.BOX)
        small = small.point(lambda v: 255 if v >= threshold else 0, "1")
        glyphs.append([[1 if small.getpixel((x, y)) else 0 for x in range(CELL_WIDTH)]
                       for y in range(cell_height)])

    return glyphs


def ttf_uk_bitmaps(font_path, cell_height, threshold, supersample):
    """Rasterise the UK charset overrides (the pound sign standing in for #)."""
    font, canvas, baseline, _size = ttf_text_geometry(
        font_path, cell_height, supersample)
    return [ttf_render_rows(chr(code), font, canvas, baseline, cell_height, threshold)
            for code in sorted(UK_CHARSET.values())]


# --------------------------------------------------------------------------
# Emission
# --------------------------------------------------------------------------

def pack_glyph(rows):
    """Pack a glyph's pixel rows into the firmware's byte format."""
    packed = []
    for row in rows:
        for byte_index in range((CELL_WIDTH + 7) // 8):
            value = 0
            for bit in range(8):
                column = byte_index * 8 + bit
                if column < CELL_WIDTH and row[column]:
                    value |= 0x80 >> bit
            packed.append(value)
    return packed


def emit_table(lines, symbol, glyphs, first_char=FIRST_CHAR, labels=None):
    """Emit one packed glyph table.

    `labels` supplies the comment beside each glyph, so the generated file can
    say which ASCII code a character in a sparse table (such as the special
    graphics set) belongs to.
    """
    lines.append("static const uint8_t %s[] = {" % symbol)
    for index, glyph in enumerate(glyphs):
        if labels is not None:
            lines.append("\t/* '%s' (0x%02X) */" % (labels[index], labels[index]))
        else:
            lines.append("\t/* 0x%02X */" % (index + first_char))
        for offset in range(0, len(glyph), 12):
            chunk = ", ".join("0x%02X" % b for b in glyph[offset:offset + 12])
            lines.append("\t%s," % chunk)
    lines.append("};")
    lines.append("")


def resolve(root, path):
    path = expand_font_tokens(path, root)
    return path if os.path.isabs(path) else os.path.join(root, path)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    config_path = os.path.join(root, "tools", "fonts.json")
    if not os.path.exists(config_path):
        raise SystemExit("missing config: %s" % config_path)

    with open(config_path, encoding="utf-8") as handle:
        config = json.load(handle)

    supersample = int(config.get("supersample", 4))
    default_sizes = config.get("sizes", [20])
    families = config.get("families", [])
    if not families:
        raise SystemExit("no families defined in tools/fonts.json")

    body, entries, externs = [], [], []
    count = 0

    for family in families:
        identifier = family["id"]
        name = family["name"]
        path = resolve(root, family["path"])
        if not os.path.exists(path):
            raise SystemExit(
                "font not found: %s\n"
                "  (family '%s'). Outline faces are optional: either install the\n"
                "  font, set %s / %s, or point the entry's \"path\" at a font you\n"
                "  have. See tools/fonts.json." % (path, identifier,
                                                    FONT_DIR_ENV, FONT_SUFFIX_ENV))

        kind = family.get("type", "ttf")
        threshold = int(family.get("threshold", 128))

        for cell_height in family.get("sizes", default_sizes):
            if kind == "bdf":
                glyphs = bake_bdf(path, cell_height)
                special = bake_bdf_special(path, cell_height)
                uk = bake_bdf_uk(path, cell_height)
                source = "bitmap"
            else:
                glyphs = bake_ttf(path, cell_height, threshold, supersample)
                special = bake_ttf_special(path, cell_height, threshold, supersample)
                uk = bake_ttf_uk(path, cell_height, threshold, supersample)
                source = "ttf"

            table = "font_%s_%d_table" % (identifier, cell_height)
            symbol = "Font%s%d" % (identifier.capitalize(), cell_height)

            # The special graphics and UK sets are sparse, so they are stored
            # densely and indexed by offset from the first code they cover.
            special_table = "font_%s_%d_special" % (identifier, cell_height)
            uk_table = "font_%s_%d_uk" % (identifier, cell_height)

            emit_table(body, table, glyphs)
            emit_table(body, special_table, special,
                       labels=sorted(SPECIAL_GRAPHICS))
            emit_table(body, uk_table, uk, labels=sorted(UK_CHARSET))

            body.append("const sFONT %s = { %s, %d, %d };" %
                        (symbol, table, CELL_WIDTH, cell_height))
            body.append("const vt100_extra_glyphs_t %s_extra = { %s, %s };" %
                        (symbol, special_table, uk_table))
            body.append("")
            entries.append('\t{ &%s, &%s_extra, "%s", "%dx%d" },'
                           % (symbol, symbol, name, CELL_WIDTH, cell_height))
            externs.append("extern const sFONT %s;" % symbol)
            externs.append("extern const vt100_extra_glyphs_t %s_extra;" % symbol)

            print("%-24s %s -> %dx%d" % (symbol, source, CELL_WIDTH, cell_height))
            count += 1

    body.append("const vt100_font_t VT100_FONTS[VT100_FONT_COUNT] = {")
    body.extend(entries)
    body.append("};")
    body.append("")

    out_c = os.path.join(root, "src", "fonts_vt100.c")
    with open(out_c, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(
            "/*\n"
            " * Auto-generated by tools/gen_fonts.py - do not edit by hand.\n"
            " *\n"
            " * %d bitmap fonts. BDF sources are native bitmap glyphs copied\n"
            " * one-to-one; TTF sources are supersampled %dx then filtered.\n"
            " * Bit set = foreground pixel, one byte per 8 pixels, MSB first.\n"
            " */\n"
            "#include <stdint.h>\n\n"
            '#include "fonts.h"\n'
            '#include "fonts_vt100.h"\n\n' % (count, supersample)
        )
        handle.write("\n".join(body))

    out_h = os.path.join(root, "src", "fonts_vt100.h")
    with open(out_h, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(
            "#ifndef VT100_FONTS_H\n"
            "#define VT100_FONTS_H\n\n"
            '#include "fonts.h"\n\n'
            "/* DEC Special Graphics and UK charset glyphs, baked alongside the\n"
            " * ASCII set. They are stored densely in ASCII order starting at\n"
            " * VT100_SPECIAL_FIRST / VT100_UK_FIRST respectively, so the firmware\n"
            " * indexes them by subtracting the first code they cover. */\n"
            "#define VT100_SPECIAL_FIRST 0x%02X\n"
            "#define VT100_SPECIAL_COUNT %d\n"
            "#define VT100_UK_FIRST 0x%02X\n"
            "#define VT100_UK_COUNT %d\n\n"
            % (SPECIAL_FIRST, SPECIAL_COUNT, sorted(UK_CHARSET)[0], UK_COUNT)
            + "typedef struct {\n"
            "    const uint8_t *special; /* VT100_SPECIAL_COUNT glyphs */\n"
            "    const uint8_t *uk;      /* VT100_UK_COUNT glyphs */\n"
            "} vt100_extra_glyphs_t;\n\n"
            "/* Baked by tools/gen_fonts.py from tools/fonts.json. All are 8 pixels\n"
            " * wide, so the column count is 80 and the row count follows the\n"
            " * cell height. */\n"
            + "\n".join(externs) + "\n\n"
            "typedef struct {\n"
            "    const sFONT *font;\n"
            "    const vt100_extra_glyphs_t *extra;\n"
            "    const char *family;\n"
            "    const char *size;\n"
            "} vt100_font_t;\n\n"
            "#define VT100_FONT_COUNT %d\n\n" % count
            + "extern const vt100_font_t VT100_FONTS[VT100_FONT_COUNT];\n\n"
            "#endif\n"
        )

    print("\nwrote %s" % out_c)
    print("wrote %s (%d fonts)" % (out_h, count))


def bake_bdf(path, cell_height):
    """Bake a native bitmap face, copying ASCII glyphs one-to-one."""
    table = parse_bdf(path)
    glyphs = []
    for code in range(FIRST_CHAR, LAST_CHAR + 1):
        entry = table.get(code)
        if entry is None:
            glyphs.append(pack_glyph([[0] * CELL_WIDTH] * cell_height))
            continue
        rows = bdf_glyph_bitmap(entry, cell_height, CELL_WIDTH)
        glyphs.append(pack_glyph(rows))
    return glyphs


def bake_bdf_special(path, cell_height):
    """Bake the DEC special graphics set from a native bitmap face."""
    table = parse_bdf(path)
    glyphs = []
    for ascii_code in sorted(SPECIAL_GRAPHICS):
        entry = table.get(SPECIAL_GRAPHICS[ascii_code])
        if entry is None:
            glyphs.append(pack_glyph([[0] * CELL_WIDTH] * cell_height))
            continue
        glyphs.append(pack_glyph(
            bdf_glyph_bitmap(entry, cell_height, CELL_WIDTH)))
    return glyphs


def bake_bdf_uk(path, cell_height):
    """Bake the UK charset overrides from a native bitmap face."""
    table = parse_bdf(path)
    glyphs = []
    for code in sorted(UK_CHARSET.values()):
        entry = table.get(code)
        if entry is None:
            glyphs.append(pack_glyph([[0] * CELL_WIDTH] * cell_height))
            continue
        glyphs.append(pack_glyph(
            bdf_glyph_bitmap(entry, cell_height, CELL_WIDTH)))
    return glyphs


def bake_ttf(path, cell_height, threshold, supersample):
    """Bake an outline font, supersampled down into the cell."""
    rows = ttf_glyph_bitmaps(path, cell_height, threshold, supersample)
    return [pack_glyph(r) for r in rows]


def bake_ttf_special(path, cell_height, threshold, supersample):
    """Bake the DEC special graphics set from an outline font."""
    return [pack_glyph(r) for r in
            ttf_special_bitmaps(path, cell_height, threshold, supersample)]


def bake_ttf_uk(path, cell_height, threshold, supersample):
    """Bake the UK charset overrides from an outline font."""
    return [pack_glyph(r) for r in
            ttf_uk_bitmaps(path, cell_height, threshold, supersample)]


if __name__ == "__main__":
    sys.exit(main())
