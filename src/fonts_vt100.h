#ifndef VT100_FONTS_H
#define VT100_FONTS_H

#include "fonts.h"

/* DEC Special Graphics and UK charset glyphs, baked alongside the
 * ASCII set. They are stored densely in ASCII order starting at
 * VT100_SPECIAL_FIRST / VT100_UK_FIRST respectively, so the firmware
 * indexes them by subtracting the first code they cover. */
#define VT100_SPECIAL_FIRST 0x60
#define VT100_SPECIAL_COUNT 31
#define VT100_UK_FIRST 0x23
#define VT100_UK_COUNT 1

typedef struct {
    const uint8_t *special; /* VT100_SPECIAL_COUNT glyphs */
    const uint8_t *uk;      /* VT100_UK_COUNT glyphs */
} vt100_extra_glyphs_t;

/* Baked by tools/gen_fonts.py from tools/fonts.json. All are 8 pixels
 * wide, so the column count is 80 and the row count follows the
 * cell height. */
extern const sFONT FontVt22020;
extern const vt100_extra_glyphs_t FontVt22020_extra;
extern const sFONT FontModern20;
extern const vt100_extra_glyphs_t FontModern20_extra;
extern const sFONT FontTerminal20;
extern const vt100_extra_glyphs_t FontTerminal20_extra;

typedef struct {
    const sFONT *font;
    const vt100_extra_glyphs_t *extra;
    const char *family;
    const char *size;
} vt100_font_t;

#define VT100_FONT_COUNT 3

extern const vt100_font_t VT100_FONTS[VT100_FONT_COUNT];

#endif
