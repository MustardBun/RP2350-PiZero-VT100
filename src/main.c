#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include "hardware/vreg.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#include "tmds_encode.h"
#include "GUI_Paint.h"
#include "fonts_vt100.h"

#include "pio_usb.h"
#include "tusb.h"

#define FRAME_WIDTH 640
#define FRAME_HEIGHT 480
#define VREG_VSEL VREG_VOLTAGE_1_10
#define DVI_TIMING dvi_timing_640x480p_60hz

/* Glyphs are baked 8 pixels wide and 12-20 pixels tall. Terminal geometry is
 * derived from the selected font, so a smaller font fits more rows and the
 * whole 640x480 frame is available to terminal output. */
#define MAX_CELL_WIDTH 8
#define MIN_CELL_HEIGHT 12
#define MAX_SCREEN_COLUMNS (FRAME_WIDTH / MAX_CELL_WIDTH)
#define MAX_SCREEN_ROWS (FRAME_HEIGHT / MIN_CELL_HEIGHT)

#define UART_BAUD 115200
#define Z80_UART uart0
#define UART_TX_PIN 0
#define UART_RX_PIN 1

/* Tab stops are every 8 columns, as on the VT100 and every CP/M terminal. */
#define TAB_WIDTH 8

/* Received bytes are buffered by an interrupt handler, so a slow full-screen
 * redraw can never overrun the 32-byte hardware FIFO. At 115200 baud a byte
 * arrives every ~87us, while a full redraw takes milliseconds, so draining
 * the FIFO from the main loop alone would silently drop CP/M output. */
#define RX_RING_SIZE 2048u /* power of two */
#define RX_RING_MASK (RX_RING_SIZE - 1u)

static volatile uint8_t rx_ring[RX_RING_SIZE];
static volatile uint16_t rx_head; /* written by the ISR */
static volatile uint16_t rx_tail; /* read by the main loop */

/* RGB111 colours. Bit 0 = blue, bit 1 = green, bit 2 = red, matching the
 * bitplane order used by the DVI TMDS encoder. */
#define RGB111_BLACK   0x0
#define RGB111_BLUE    0x1
#define RGB111_GREEN   0x2
#define RGB111_CYAN    0x3
#define RGB111_RED     0x4
#define RGB111_MAGENTA 0x5
#define RGB111_YELLOW  0x6
#define RGB111_WHITE   0x7

/* Amber cannot be produced by an RGB111 palette: it needs a roughly 75%
 * green channel, and this framebuffer only offers green at 0% or 100%. Solid
 * yellow is the closest available match, and is used instead of dithering.
 *
 * Dithering red with yellow does average towards amber, but the two colours
 * differ hugely in luminance (red is dark, yellow is bright), so the
 * checkerboard is plainly visible as grain over the glyphs and hurts
 * legibility. Solid colour is the better trade here.
 *
 * Phosphor still uses dithering because its pair (green + yellow) differs by
 * only 54 in luminance, so the pattern blends invisibly into a slightly warmer
 * green. */
#define AMBER_SOLID RGB111_YELLOW
#define PHOSPHOR_DARK  RGB111_GREEN
#define PHOSPHOR_LIGHT RGB111_YELLOW

#define NO_DITHER 0xFF

/* Per-cell character attributes, as set by SGR. The top two bits latch the
 * character set that was active when the character was written, because a
 * VT100 resolves the glyph at output time: selecting a different charset later
 * must not retroactively restyle text that is already on screen. */
#define ATTR_REVERSE   0x01u
#define ATTR_BOLD      0x02u
#define ATTR_DIM       0x04u
#define ATTR_UNDERLINE 0x08u
#define ATTR_BLINK     0x10u
#define ATTR_INVISIBLE 0x20u
#define ATTR_CHARSET_SHIFT 6
#define ATTR_CHARSET_MASK  0xC0u

/* Character sets assigned to G0/G1 by ESC ( x and ESC ) x, and selected for
 * output by SO/SI. */
enum {
    CHARSET_ASCII = 0, /* US ASCII */
    CHARSET_SPECIAL,   /* DEC Special Graphics: the line drawing set */
    CHARSET_UK         /* United Kingdom: '#' displays as the pound sign */
};

/* VT100 sequences may carry several ';' separated parameters. More than a
 * handful are not needed by CP/M or WordStar, but extra ones must still be
 * consumed rather than leaked onto the screen. */
#define MAX_CSI_PARAMS 8

typedef struct {
    const char *name;
    uint8_t fg;
    uint8_t alt; /* checkerboard dither partner, or NO_DITHER for solid */
} text_palette_t;

static const text_palette_t text_palette[] = {
    { "Amber",    AMBER_SOLID,    NO_DITHER   },
    { "Phosphor", PHOSPHOR_DARK,  PHOSPHOR_LIGHT },
    { "White",    RGB111_WHITE,   NO_DITHER   },
    { "Green",    RGB111_GREEN,   NO_DITHER   },
    { "Cyan",     RGB111_CYAN,    NO_DITHER   },
    { "Magenta",  RGB111_MAGENTA, NO_DITHER   },
    { "Red",      RGB111_RED,     NO_DITHER   },
    { "Blue",     RGB111_BLUE,    NO_DITHER   },
    { "Black",    RGB111_BLACK,   NO_DITHER   },
};
#define TEXT_COLOR_COUNT ((uint8_t)(sizeof(text_palette) / sizeof(text_palette[0])))

static const char *const background_names[] = { "Black", "White" };
static const uint8_t background_colors[] = { RGB111_BLACK, RGB111_WHITE };
#define BACKGROUND_COUNT ((uint8_t)(sizeof(background_names) / sizeof(background_names[0])))

/* A text colour that matches the background would render invisibly, which
 * would make the setup menu itself unreadable and leave the terminal stuck.
 * Only exact matches are rejected: every other pair differs in at least one
 * bitplane, so something is always visible. */
static inline bool color_bg_visible(uint8_t color_index, uint8_t background_index)
{
    if (color_index >= TEXT_COLOR_COUNT) return false;
    uint8_t fg = text_palette[color_index].fg;
    if (text_palette[color_index].alt != NO_DITHER) {
        /* A dithered colour also lights its partner, so it stays visible even
         * if one of the two matches the background. */
        return true;
    }
    return fg != background_colors[background_index];
}

/* Boot defaults: the authentic DEC VT220 glyphs in white on black, with
 * autowrap on and LF treated as CR+LF because many CP/M setups rely on it. */
#define DEFAULT_FONT_SELECTION 0
#define DEFAULT_COLOR_SELECTION 2
#define DEFAULT_BACKGROUND_SELECTION 0
#define DEFAULT_AUTOWRAP_SELECTION 0 /* 0 = on */
#define DEFAULT_NEWLINE_SELECTION 0  /* 0 = LF implies CR */
#define DEFAULT_CURSOR_SELECTION 0   /* 0 = cursor shown */

#define PLANE_SIZE_BYTES (FRAME_WIDTH * FRAME_HEIGHT / 8)
static uint8_t framebuf[3 * PLANE_SIZE_BYTES];
static struct dvi_inst dvi0;

/* ---------------------------------------------------------------------------
 * Settings persistence
 *
 * The settings live in the last flash sector, which the linker does not use
 * for code. A magic number guards against loading erased or foreign data, and
 * the checksum guards against a partial write.
 * ------------------------------------------------------------------------- */
/* Bumping the magic invalidates records written by older firmware. It is
 * raised whenever menu items change order or meaning, so a stale saved index
 * cannot silently select a different colour. */
#define SETTINGS_MAGIC 0x35545653u /* "SVT5" */
#define SETTINGS_SECTOR_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define SETTINGS_ADDRESS (XIP_BASE + SETTINGS_SECTOR_OFFSET)

/* flash_range_program() requires whole 256-byte pages, so the record is padded
 * out to one page with reserved bytes. The fields before the padding occupy
 * 16 bytes. */
typedef struct {
    uint32_t magic;
    uint8_t font;
    uint8_t color;
    uint8_t background;
    uint8_t autowrap;
    uint8_t newline;
    uint8_t cursor;
    uint8_t reserved[2];
    uint32_t checksum;
} settings_t;

static_assert(sizeof(settings_t) == 16, "settings header must be 16 bytes");

static settings_t pending_settings;

static uint32_t settings_checksum(const settings_t *settings)
{
    uint32_t value = settings->magic;
    value = value * 31u + settings->font;
    value = value * 31u + settings->color;
    value = value * 31u + settings->background;
    value = value * 31u + settings->autowrap;
    value = value * 31u + settings->newline;
    value = value * 31u + settings->cursor;
    return value;
}

static void flash_write_callback(void *param)
{
    const settings_t *settings = (const settings_t *)param;
    uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    memcpy(page, settings, sizeof(*settings));

    flash_range_erase(SETTINGS_SECTOR_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_SECTOR_OFFSET, page, sizeof(page));
}

static bool settings_valid(const settings_t *settings)
{
    if (settings->magic != SETTINGS_MAGIC) return false;
    if (settings->checksum != settings_checksum(settings)) return false;
    if (settings->font >= VT100_FONT_COUNT) return false;
    if (settings->color >= TEXT_COLOR_COUNT) return false;
    if (settings->background >= BACKGROUND_COUNT) return false;
    if (settings->autowrap > 1) return false;
    if (settings->newline > 1) return false;
    if (settings->cursor > 1) return false;
    /* A stored text/background pair that renders nothing would leave the menu
     * invisible and the terminal unrecoverable, so such a record is treated as
     * invalid and the boot defaults are used instead. */
    if (!color_bg_visible(settings->color, settings->background)) return false;
    return true;
}

/* Persist the current menu selections so the next power-up restores them.
 *
 * The image runs entirely from RAM (PICO_COPY_TO_RAM=1), so core 1 never
 * fetches instructions from flash and does not have to be parked while the
 * sector is erased. PICO_FLASH_ASSUME_CORE1_SAFE tells the SDK that, which
 * reduces flash_safe_execute() to disabling interrupts on this core. Parking
 * core 1 instead stalls DVI scanout long enough to lose monitor sync. */
static void settings_save(void);

static char screen[MAX_SCREEN_ROWS][MAX_SCREEN_COLUMNS];
static uint8_t screen_attributes[MAX_SCREEN_ROWS][MAX_SCREEN_COLUMNS];
static uint16_t cursor_x;
static uint16_t cursor_y;
static uint16_t saved_cursor_x;
static uint16_t saved_cursor_y;
/* DECSC/DECRC also save and restore the character set arrangement. */
static uint8_t saved_charset_g0 = CHARSET_ASCII;
static uint8_t saved_charset_g1 = CHARSET_ASCII;
static bool saved_charset_g1_active;
static uint8_t escape_state;
static uint16_t csi_value;
static uint16_t csi_values[8];
static uint8_t csi_count;
static bool csi_private;
/* The intermediate byte of an ESC sequence (0x20-0x2F), which distinguishes
 * families such as ESC ( B (charset) from ESC # 8 (alignment test). */
static uint8_t esc_intermediate;

/* Attributes applied to newly written characters (ATTR_* bits). */
static uint8_t current_attributes;
/* Attributes captured by DECSC, restored by DECRC. */
static uint8_t saved_attributes;
/* Which character set G0 and G1 hold, and which of the two is in use. */
static uint8_t charset_g0 = CHARSET_ASCII;
static uint8_t charset_g1 = CHARSET_ASCII;
static bool charset_g1_active; /* false = G0 in use, true = G1 in use (SO/SI) */
/* DECSCNM: reverses the foreground and background of the whole screen. */
static bool screen_reverse;
/* DECCKM: arrow and Home/End keys send SS3 sequences instead of CSI ones. */
static bool cursor_keys_application;
/* DECKPAM/DECKPNM: keypad sends application sequences instead of digits. */
static bool keypad_application;
/* VT52 mode, entered with DECANM (CSI ?2l) and left with ESC <.
 *
 * A VT52 is a completely different protocol, not a dialect of ANSI: its
 * sequences are ESC plus a single final character, and ESC [ has no special
 * meaning. It therefore gets its own parser rather than being folded into the
 * ANSI state machine, so neither can misinterpret the other's bytes. */
static bool vt52_mode;

/* Where the VT52 parser has got to. ESC Y is the only sequence that needs
 * more than one byte of state. */
enum {
    VT52_STATE_GROUND = 0,
    VT52_STATE_ESC,      /* saw ESC, awaiting the command letter */
    VT52_STATE_Y_ROW,    /* saw ESC Y, awaiting the row byte */
    VT52_STATE_Y_COLUMN  /* saw ESC Y <row>, awaiting the column byte */
};

static uint8_t vt52_state;
static uint16_t vt52_row;
/* VT52 graphics mode, entered with ESC F and left with ESC G. The VT52
 * character set is the same DEC line-drawing set the VT100 reaches through
 * ESC ( 0, so it reuses the glyphs already baked for that. */
static bool vt52_graphics;
/* Blinking text is repainted periodically. The phase only matters once a
 * blinking character has actually been written. */
static bool blink_visible = true;
static bool blink_attribute_used;

/* The block cursor blinks on its own phase, independent of blinking text, and
 * is drawn as an overlay rather than written into the character buffer. Keeping
 * it out of the buffer means the cell underneath survives untouched, so the
 * cursor can be turned off (or moved) without the original character needing to
 * be remembered and restored. */
static bool cursor_enabled = true;
static bool cursor_phase_visible = true;
/* Set by DECTCEM (CSI ? 25 h/l). A host that hides the cursor outranks the
 * menu setting, which is what CP/M and WordStar expect when they reposition it
 * themselves. */
static bool cursor_host_visible = true;
/* Which cell currently has the block cursor painted on it. Because the overlay
 * is not part of the character buffer, the cell it leaves behind must be
 * repainted from the buffer to erase the block; tracking the painted cell is
 * what makes that possible without repainting the whole screen. */
static uint16_t cursor_painted_x;
static uint16_t cursor_painted_y;
static bool cursor_painted;

/* Both the terminal size and the wrapping rules follow the selected font and
 * the autowrap setting, so they are runtime state rather than constants. */
static uint16_t terminal_columns = MAX_SCREEN_COLUMNS;
static uint16_t terminal_rows = MAX_SCREEN_ROWS;
static uint16_t cell_width = MAX_CELL_WIDTH;
static uint16_t cell_height = MIN_CELL_HEIGHT;
static bool autowrap_enabled = true;
/* Real VT100 hardware does not move the cursor horizontally on LF, which is
 * why hosts send CR LF. Some CP/M software emits a bare LF and then expects
 * the next line to begin at the left margin, so that behaviour is selectable. */
static bool newline_crlf = true;
/* VT100 defers the wrap when the last column is filled: the character is
 * stored, and the cursor only advances when the next character arrives. This
 * keeps a program that writes a full row from scrolling prematurely. */
static bool pending_wrap;

/* Scrolling region (DECSTBM), stored as inclusive zero-based row indices.
 * Everything outside these bounds is frozen: only rows inside move when the
 * screen scrolls. Defaults to the whole screen. */
static uint16_t scroll_top = 0;
static uint16_t scroll_bottom = MAX_SCREEN_ROWS - 1;

/* Origin mode (DECOM): while set, the cursor is positioned relative to the top
 * margin and cannot be moved outside the scrolling region. */
static bool origin_mode;

/* Tab stops, one bit per column. Initialised every TAB_WIDTH columns. */
static uint8_t tab_stops[(MAX_SCREEN_COLUMNS + 7) / 8];
static const sFONT *active_font;
/* The special-graphics and UK glyph tables that go with active_font. */
static const vt100_extra_glyphs_t *active_extra;
static uint8_t color_selection = 0;
static uint8_t background_selection = 0;
static uint8_t autowrap_selection = 0;
static uint8_t newline_selection = 0;
static uint8_t cursor_selection = 0; /* 0 = shown, 1 = hidden */
static uint8_t font_selection = 0;
static uint8_t menu_item = 0;
static bool menu_active = true;
/* True when the menu was opened over a running session rather than shown at
 * boot. It decides whether leaving the menu returns to the live screen or
 * starts a fresh one. */
static bool menu_over_terminal;
/* Set when the font changes, because a size change means the existing screen
 * content has no meaningful mapping onto the new geometry. */
static bool geometry_changed;
static bool display_ready;
static volatile bool keyboard_connected;
static bool uart_data_seen;
static volatile bool hid_report_error;
static volatile bool usb_status_changed;
static queue_t keyboard_report_queue;

static uint8_t const keycode2ascii[128][2] = { HID_KEYCODE_TO_ASCII };

/* A single received character only repaints its own cell. Operations that
 * affect the whole screen (scrolling, erase) raise this flag so the main loop
 * repaints once per burst instead of once per line. */
static bool full_redraw_pending;

static void draw_menu(void);
/* Defined after menu_exit(), which returns to the terminal when the menu was
 * the boot screen. */
static void begin_terminal_mode(void);

/* Interrupt handler: move everything the UART has received into the ring. */
static void uart_rx_irq_handler(void)
{
    while (uart_is_readable(Z80_UART)) {
        uint16_t next = (uint16_t)((rx_head + 1u) & RX_RING_MASK);
        if (next == rx_tail) {
            break; /* ring full - drop rather than overwrite unread data */
        }
        rx_ring[rx_head] = (uint8_t)uart_getc(Z80_UART);
        rx_head = next;
    }
}

static void uart_rx_init(void)
{
    rx_head = 0;
    rx_tail = 0;
    irq_set_exclusive_handler(UART0_IRQ, uart_rx_irq_handler);
    irq_set_enabled(UART0_IRQ, true);
    /* Interrupt when the receive FIFO has data. */
    uart_set_irq_enables(Z80_UART, true, false);
}

static inline bool uart_rx_available(void)
{
    return rx_head != rx_tail;
}

static inline uint8_t uart_rx_get(void)
{
    uint8_t value = rx_ring[rx_tail];
    rx_tail = (uint16_t)((rx_tail + 1u) & RX_RING_MASK);
    return value;
}

/* Reset the tab stops to the default every TAB_WIDTH columns. */
static void tabs_reset(void)
{
    memset(tab_stops, 0, sizeof(tab_stops));
    for (uint16_t column = TAB_WIDTH; column < terminal_columns; column += TAB_WIDTH) {
        tab_stops[column >> 3] |= (uint8_t)(1u << (column & 7));
    }
}

static inline bool tab_is_set(uint16_t column)
{
    return (tab_stops[column >> 3] & (1u << (column & 7))) != 0;
}

static inline void tab_clear_all(void)
{
    memset(tab_stops, 0, sizeof(tab_stops));
}

static inline void tab_set(uint16_t column)
{
    tab_stops[column >> 3] |= (uint8_t)(1u << (column & 7));
}

/* Advance to the next tab stop, clamping at the right margin. Tab never wraps
 * and never triggers a line break. */
static void tab_forward(void)
{
    uint16_t column = cursor_x;
    while (++column < terminal_columns) {
        if (tab_is_set(column)) {
            cursor_x = column;
            return;
        }
    }
    cursor_x = (uint16_t)(terminal_columns - 1);
}

/* Recompute the geometry after a font change. The screen buffer is sized for
 * the smallest font, so a larger font simply uses fewer of its rows. */
static void apply_geometry(void)
{
    cell_width = (uint16_t)active_font->Width;
    cell_height = (uint16_t)active_font->Height;
    terminal_columns = FRAME_WIDTH / cell_width;
    terminal_rows = FRAME_HEIGHT / cell_height;

    if (terminal_columns > MAX_SCREEN_COLUMNS) terminal_columns = MAX_SCREEN_COLUMNS;
    if (terminal_rows > MAX_SCREEN_ROWS) terminal_rows = MAX_SCREEN_ROWS;

    /* The scrolling region always spans the whole screen after a size change,
     * and the tab stops follow the new width. */
    scroll_top = 0;
    scroll_bottom = (uint16_t)(terminal_rows - 1);
    origin_mode = false;
    tabs_reset();
}

/* Write one pixel plane byte into the bitplaned RGB111 framebuffer.
 * The framebuffer is LSB-first per byte (bit 0 is the leftmost pixel), which
 * matches GUI_Paint's Paint_SetPixel so both renderers agree. */
static inline void fb_set_pixel(uint16_t x, uint16_t y, uint8_t color)
{
    if (x >= FRAME_WIDTH || y >= FRAME_HEIGHT) return;

    uint32_t index = (uint32_t)y * (FRAME_WIDTH / 8) + (x >> 3);
    uint8_t mask = (uint8_t)(1u << (x & 7));

    for (uint8_t plane = 0; plane < 3; ++plane) {
        uint8_t *byte = &framebuf[plane * PLANE_SIZE_BYTES + index];
        if (color & (1u << plane)) {
            *byte |= mask;
        } else {
            *byte &= (uint8_t)~mask;
        }
    }
}

/* Paint a rectangular region, applying optional checkerboard dithering. */
static void fill_region(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                        uint8_t color, uint8_t alt)
{
    for (uint16_t row = 0; row < height; ++row) {
        for (uint16_t column = 0; column < width; ++column) {
            uint8_t pixel = color;
            if (alt != NO_DITHER) {
                pixel = (((x + column) ^ (y + row)) & 1u) ? alt : color;
            }
            fb_set_pixel(x + column, y + row, pixel);
        }
    }
}

static void clear_framebuffer(void)
{
    uint8_t background = background_colors[background_selection];
    memset(framebuf, (background & 1u) ? 0xFF : 0x00, PLANE_SIZE_BYTES);
    memset(framebuf + PLANE_SIZE_BYTES, (background & 2u) ? 0xFF : 0x00, PLANE_SIZE_BYTES);
    memset(framebuf + 2 * PLANE_SIZE_BYTES, (background & 4u) ? 0xFF : 0x00, PLANE_SIZE_BYTES);
}

/* The baked glyph tables are MSB-first (bit 7 is the leftmost pixel) while the
 * RGB111 framebuffer is LSB-first (bit 0 is the leftmost pixel). Bits must be
 * mirrored when a glyph byte is copied straight into a framebuffer byte. */
static inline uint8_t mirror_bits(uint8_t value)
{
    value = (uint8_t)((value & 0xF0u) >> 4 | (value & 0x0Fu) << 4);
    value = (uint8_t)((value & 0xCCu) >> 2 | (value & 0x33u) << 2);
    value = (uint8_t)((value & 0xAAu) >> 1 | (value & 0x55u) << 1);
    return value;
}

/* Locate the glyph bytes for a character in the given character set.
 *
 * Returns NULL when there is no glyph, which is how control codes and the
 * undefined parts of a charset end up drawing as blank cells. */
static const uint8_t *glyph_for(uint8_t character, uint8_t charset)
{
    const sFONT *font = active_font;
    uint16_t height = font->Height;
    uint16_t stride = (uint16_t)((font->Width + 7) / 8);

    if (charset == CHARSET_SPECIAL &&
        character >= VT100_SPECIAL_FIRST &&
        character < VT100_SPECIAL_FIRST + VT100_SPECIAL_COUNT) {
        return &active_extra->special[
            (uint32_t)(character - VT100_SPECIAL_FIRST) * height * stride];
    }
    /* Only a few positions differ in the UK set; everything else it does not
     * cover falls through to the ASCII glyph. */
    if (charset == CHARSET_UK &&
        character >= VT100_UK_FIRST &&
        character < VT100_UK_FIRST + VT100_UK_COUNT) {
        return &active_extra->uk[
            (uint32_t)(character - VT100_UK_FIRST) * height * stride];
    }
    if (character >= 0x20 && character <= 0x7e) {
        return &font->table[(uint32_t)(character - 0x20) * height * stride];
    }
    return NULL;
}

/* Draw resolved glyph bytes into one cell.
 *
 * `glyph` is NULL for a blank cell. Bold and underline are applied here rather
 * than being baked into the font, so they compose with any colour and with the
 * charset in use. */
static void draw_glyph(uint16_t column, uint16_t row, const uint8_t *glyph,
                       uint8_t fg, uint8_t fg_alt, uint8_t bg, uint8_t bg_alt,
                       bool bold, bool underline)
{
    const sFONT *font = active_font;
    uint16_t height = font->Height;
    uint16_t x = (uint16_t)(column * cell_width);
    uint16_t y = (uint16_t)(row * cell_height);
    uint16_t stride = (uint16_t)((font->Width + 7) / 8);

    if (cell_width == 8 && bg_alt == NO_DITHER && fg_alt == NO_DITHER) {
        /* Fast path: one glyph byte and one framebuffer byte per plane. The
         * glyph row is mirrored because the two bit orders differ. */
        uint32_t byte_column = x >> 3;
        for (uint16_t gy = 0; gy < height; ++gy) {
            uint8_t bits = glyph ? mirror_bits(glyph[gy]) : 0x00;
            if (bold && bits) {
                /* RGB111 has no intensity to raise, so bold is rendered as
                 * extra stroke weight: every set pixel also lights the pixel
                 * to its left. Shifting towards bit 0 keeps the smear inside
                 * this byte, so it can never spill into the neighbouring cell
                 * the way a rightward shift would. */
                bits |= (uint8_t)(bits >> 1);
            }
            if (glyph && underline && gy == height - 1) {
                bits = 0xFF;
            }
            uint32_t base = (uint32_t)(y + gy) * (FRAME_WIDTH / 8) + byte_column;
            for (uint8_t plane = 0; plane < 3; ++plane) {
                uint8_t value = (bg & (1u << plane)) ? 0xFF : 0x00;
                if (bits) {
                    uint8_t ink = (fg & (1u << plane)) ? 0xFF : 0x00;
                    value = (uint8_t)((value & ~bits) | (ink & bits));
                }
                framebuf[plane * PLANE_SIZE_BYTES + base] = value;
            }
        }
        return;
    }

    /* General path: needed for dithered colours and non-byte-aligned cells. */
    fill_region(x, y, cell_width, height, bg, bg_alt);
    if (glyph == NULL) return;

    for (uint16_t gy = 0; gy < height; ++gy) {
        for (uint16_t gx = 0; gx < font->Width; ++gx) {
            uint8_t bits = glyph[gy * stride + (gx >> 3)];
            if (!(bits & (0x80 >> (gx & 7)))) continue;

            uint8_t pixel = fg;
            if (fg_alt != NO_DITHER) {
                pixel = (((x + gx) ^ (y + gy)) & 1u) ? fg_alt : fg;
            }
            fb_set_pixel(x + gx, y + gy, pixel);
            if (bold && gx > 0) {
                /* Same leftward thicken as the fast path, clamped so a glyph
                 * starting at column 0 cannot reach into the cell before it. */
                fb_set_pixel(x + gx - 1, y + gy, pixel);
            }
        }
    }

    if (underline) {
        for (uint16_t gx = 0; gx < cell_width; ++gx) {
            fb_set_pixel(x + gx, y + height - 1, fg);
        }
    }
}

/* Resolve the colours and glyph for one buffered cell and draw it.
 *
 * This is the single place that decides how a cell looks, so every path -
 * normal output, a scroll repaint, erasing a line - renders identically and
 * attributes cannot be lost by one path forgetting about them.
 *
 * `cursor_block` paints the cell as a block cursor. The cursor is not written
 * into the character buffer at all; it is applied here, at draw time, so the
 * character underneath is never modified. That is what lets the cursor blink,
 * move or be switched off without needing to save and restore the cell. */
static void draw_cell_styled(uint16_t column, uint16_t row, bool cursor_block)
{
    uint8_t character = (uint8_t)screen[row][column];
    uint8_t attributes = screen_attributes[row][column];
    uint8_t fg = text_palette[color_selection].fg;
    uint8_t fg_alt = text_palette[color_selection].alt;
    uint8_t bg = background_colors[background_selection];
    uint8_t bg_alt = NO_DITHER;

    /* DECSCNM swaps the whole screen's foreground and background before any
     * per-character attribute is considered. */
    if (screen_reverse) {
        uint8_t temp = fg;
        fg = bg;
        bg = temp;
        bg_alt = fg_alt;
        fg_alt = NO_DITHER;
    }

    if (attributes & ATTR_REVERSE) {
        /* Reverse video swaps the text and background colours. */
        uint8_t temp = fg;
        fg = bg;
        bg = temp;
        bg_alt = fg_alt;
        fg_alt = NO_DITHER;
    }

    /* Invisible text occupies its cell but paints nothing, matching a real
     * VT100 where the characters are still there to be selected. The cursor
     * is exempt: a host that hides text must not make the cursor vanish with
     * it, or there would be no way to see where the prompt is. */
    if (!cursor_block && (attributes & ATTR_INVISIBLE)) {
        fg = bg;
        fg_alt = bg_alt;
    } else if (!cursor_block && (attributes & ATTR_DIM)) {
        /* There is no half-brightness in RGB111, so dim text falls back to a
         * thinner stroke: the glyph is drawn without the bold/underline
         * treatment and the cell background is left plain. */
        fg_alt = NO_DITHER;
    }

    /* The block cursor is the same swap as reverse video: on a real VT100 the
     * cursor cell is simply the character shown in inverse. Applying it last
     * means it sits on top of whatever the cell already resolved to, so a
     * blank cell becomes a solid block and a character stays readable. */
    if (cursor_block) {
        uint8_t temp = fg;
        fg = bg;
        bg = temp;
        bg_alt = fg_alt;
        fg_alt = NO_DITHER;
    }

    bool blinking_off = (attributes & ATTR_BLINK) && !blink_visible;
    const uint8_t *glyph = blinking_off ? NULL :
        glyph_for(character, (uint8_t)((attributes & ATTR_CHARSET_MASK)
                                       >> ATTR_CHARSET_SHIFT));

    draw_glyph(column, row, glyph, fg, fg_alt, bg, bg_alt,
               (attributes & ATTR_BOLD) != 0,
               (attributes & ATTR_UNDERLINE) != 0);
}

static void draw_cell_at_buffer(uint16_t column, uint16_t row)
{
    /* Redrawing a cell removes whatever overlay was on it. Any cursor block
     * painted there is therefore no longer on screen, so the tracking flag has
     * to be cleared or cursor_update() would think it is still showing and
     * skip repainting it. */
    if (cursor_painted && column == cursor_painted_x && row == cursor_painted_y) {
        cursor_painted = false;
    }
    draw_cell_styled(column, row, false);
}

/* True while the cursor is enabled and positioned on screen.
 *
 * Deliberately excludes the blink phase: this answers "should a cursor be
 * here at all", which is what decides whether the blink clock needs to run.
 * It must be stable across a blink cycle, or the clock would be torn down the
 * moment the cursor blinked off and the cursor would never actually blink. */
static bool cursor_active(void)
{
    return !menu_active && cursor_enabled && cursor_host_visible &&
           cursor_x < terminal_columns && cursor_y < terminal_rows;
}

/* True when the cursor should be painted right now, which additionally requires
 * the blink phase to be in its visible half. */
static bool cursor_wants_paint(void)
{
    return cursor_active() && cursor_phase_visible;
}

/* Which cell currently has the block cursor painted on it.
 *
 * The overlay is not part of the character buffer, so when the cursor moves the
 * cell it leaves behind must be repainted from the buffer to erase the block.
 * Tracking the painted cell is what makes that possible without repainting the
 * whole screen on every keystroke. */

/* Repaint whichever cursor cell changed, and only that.
 *
 * Called once per pass of the main loop rather than from each movement
 * primitive, so the many ways the cursor can move (CR, LF, tab, every CSI
 * command, a scroll) do not each need to know about the cursor. */
static void cursor_update(void)
{
    if (menu_active) {
        cursor_painted = false;
        return;
    }

    if (cursor_wants_paint()) {
        if (cursor_painted && cursor_painted_x == cursor_x &&
            cursor_painted_y == cursor_y) {
            return; /* already showing in the right place */
        }
        if (cursor_painted) {
            draw_cell_at_buffer(cursor_painted_x, cursor_painted_y);
        }
        draw_cell_styled(cursor_x, cursor_y, true);
        cursor_painted_x = cursor_x;
        cursor_painted_y = cursor_y;
        cursor_painted = true;
        return;
    }

    if (cursor_painted) {
        /* Cursor moved away, was switched off, or is in its blink-off phase. */
        draw_cell_at_buffer(cursor_painted_x, cursor_painted_y);
        cursor_painted = false;
    }
}

static void draw_screen(void)
{
    uint16_t rows = terminal_rows;
    if (rows > MAX_SCREEN_ROWS) rows = MAX_SCREEN_ROWS;

    clear_framebuffer();
    for (uint16_t row = 0; row < rows; ++row) {
        for (uint16_t column = 0; column < terminal_columns; ++column) {
            draw_cell_at_buffer(column, row);
        }
    }

    /* A full repaint wiped the overlay, so repaint it on top. */
    cursor_painted = false;
    cursor_update();
}

/* How long each half of the blink cycle lasts.
 *
 * The VT100 User Guide specifies the cursor as a "blinking block character or
 * blinking underline", driven by the video processor's own free-running
 * counter - it is NOT restarted or suspended when characters arrive. So the
 * cursor blinking while the host is printing is accurate hardware behaviour,
 * not a glitch: on a real terminal the block keeps blinking through a screen
 * full of BIOS output.
 *
 * The exact hardware rate is not documented in the material available here, so
 * this is a readable approximation rather than a figure quoted from DEC. */
#define BLINK_PERIOD_MS 600

/* Advance the blink phase and repaint the cursor.
 *
 * Called once per pass of the main loop, which covers every way the cursor can
 * have moved since the last pass: CR, LF, tab, a CSI command, a scroll, or a
 * character being written. Because the cursor is an overlay rather than part of
 * the character buffer, keeping it correct is just a matter of noticing that it
 * is somewhere new and repainting the one cell it left.
 *
 * The blink clock only runs while something actually needs it - blinking text
 * on screen, or a visible cursor - so an idle terminal with the cursor disabled
 * never repaints at all. */
static void blink_tick(void)
{
    static absolute_time_t next_toggle;
    static bool timer_running;

    /* Blinking text only needs the clock while any is actually on screen. The
     * cursor blinks whenever it is enabled, even on an otherwise idle terminal,
     * which is exactly how a real VT100 behaves. cursor_active() is used rather
     * than cursor_wants_paint() so that the cursor's own off-phase does not stop
     * the clock and freeze it in the hidden state. */
    bool need_clock = blink_attribute_used || cursor_active();

    if (!need_clock) {
        /* Nothing is blinking, so settle to the visible phase: text and the
         * cursor must never reappear stuck in the hidden half of the cycle. */
        blink_visible = true;
        cursor_phase_visible = true;
        timer_running = false;
        if (cursor_painted) cursor_update();
        return;
    }

    if (menu_active) {
        /* The menu owns the framebuffer while it is up, so there is nothing to
         * repaint underneath it. */
        timer_running = false;
        return;
    }

    if (!timer_running) {
        /* Start a fresh cycle in the visible phase, so the cursor appears
         * immediately rather than possibly waiting out half a period first
         * (which is what would happen after the menu hands the screen back). */
        cursor_phase_visible = true;
        blink_visible = true;
        next_toggle = make_timeout_time_ms(BLINK_PERIOD_MS);
        timer_running = true;
        return;
    }

    if (time_reached(next_toggle)) {
        next_toggle = make_timeout_time_ms(BLINK_PERIOD_MS);
        blink_visible = !blink_visible;
        cursor_phase_visible = blink_visible;
        /* Only a screen full of blinking text is worth a full repaint. The
         * cursor is a single cell, so it is handled precisely. */
        if (blink_attribute_used) {
            draw_screen();
        } else {
            cursor_update();
        }
    }
}

static void clear_line(uint16_t row)
{
    memset(screen[row], ' ', MAX_SCREEN_COLUMNS);
    memset(screen_attributes[row], 0, MAX_SCREEN_COLUMNS);
}

static void clear_line_range(uint16_t row, uint16_t first, uint16_t last)
{
    if (first < terminal_columns && last > first) {
        if (last > terminal_columns) last = terminal_columns;
        memset(&screen[row][first], ' ', last - first);
        memset(&screen_attributes[row][first], 0, last - first);
    }
}

static void clear_screen_buffer(void)
{
    memset(screen, ' ', sizeof(screen));
    memset(screen_attributes, 0, sizeof(screen_attributes));
}

/* Draw a string straight into the framebuffer, bypassing the character buffer.
 *
 * The setup menu is rendered this way so it never disturbs `screen`, which
 * keeps tracking the live CP/M output underneath. That is what allows the menu
 * to be opened over a running session and closed again without losing anything
 * the Z80 sent while it was up. */
static void draw_menu_text(uint16_t row, uint16_t column, const char *text, bool inverse)
{
    uint8_t fg = text_palette[color_selection].fg;
    uint8_t fg_alt = text_palette[color_selection].alt;
    uint8_t bg = background_colors[background_selection];

    if (inverse) {
        uint8_t swap = fg;
        fg = bg;
        bg = swap;
        fg_alt = NO_DITHER;
    }

    for (; column < terminal_columns && *text != '\0'; ++column, ++text) {
        /* The menu is plain text in the ASCII set, with no attributes. */
        draw_glyph(column, row, glyph_for((uint8_t)*text, CHARSET_ASCII),
                   fg, fg_alt, bg, NO_DITHER, false, false);
    }
}

/* Scroll the region between the margins up by one line. Rows outside the
 * region are left untouched. */
static void terminal_scroll(void)
{
    if (scroll_bottom > scroll_top) {
        memmove(screen[scroll_top], screen[scroll_top + 1],
                (scroll_bottom - scroll_top) * sizeof(screen[0]));
        memmove(screen_attributes[scroll_top], screen_attributes[scroll_top + 1],
                (scroll_bottom - scroll_top) * sizeof(screen_attributes[0]));
    }
    clear_line(scroll_bottom);
    cursor_y = scroll_bottom;
    /* A burst of scrolling should paint once, not once per line. */
    full_redraw_pending = true;
}

/* Move the active position down one line, scrolling the region only when it is
 * already AT the bottom margin. DEC keys the scroll to the margin itself: a
 * cursor parked in the frozen area below the region must simply advance (and
 * stop at the last row), not drag the whole region up with it. This is what
 * keeps a WordStar-style status line below a DECSTBM region from scrolling the
 * working area when a line feed lands on it. */
static void index_down(void)
{
    if (cursor_y == scroll_bottom) {
        terminal_scroll();
    } else if (cursor_y + 1 < terminal_rows) {
        ++cursor_y;
    }
}

/* Reverse index: up one line, scrolling the region down only when already at
 * the top margin. Above the region the cursor just climbs, stopping at row 0
 * rather than wrapping (the > 0 guard is what prevents an underflow there). */
static void index_up(void)
{
    if (cursor_y == scroll_top) {
        /* Scroll the region down by one line. */
        if (scroll_bottom > scroll_top) {
            memmove(screen[scroll_top + 1], screen[scroll_top],
                    (scroll_bottom - scroll_top) * sizeof(screen[0]));
            memmove(screen_attributes[scroll_top + 1],
                    screen_attributes[scroll_top],
                    (scroll_bottom - scroll_top) * sizeof(screen_attributes[0]));
        }
        clear_line(scroll_top);
        cursor_y = scroll_top;
        full_redraw_pending = true;
    } else if (cursor_y > 0) {
        --cursor_y;
    }
}

/* Clamp a row into the active region, honouring origin mode. */
static uint16_t clamp_row(int32_t row)
{
    if (origin_mode) {
        if (row < (int32_t)scroll_top) return scroll_top;
        if (row > (int32_t)scroll_bottom) return scroll_bottom;
        return (uint16_t)row;
    }
    if (row < 0) return 0;
    if (row >= (int32_t)terminal_rows) return (uint16_t)(terminal_rows - 1);
    return (uint16_t)row;
}

static void apply_font(uint8_t index)
{
    if (index >= VT100_FONT_COUNT) index = 0;
    font_selection = index;
    active_font = VT100_FONTS[index].font;
    active_extra = VT100_FONTS[index].extra;
    apply_geometry();
    cursor_x = 0;
    cursor_y = 0;
    /* A charset or attribute from the old font has no meaning in the new one,
     * and the screen is repainted from scratch anyway. */
    current_attributes = 0;
    charset_g0 = CHARSET_ASCII;
    charset_g1 = CHARSET_ASCII;
    charset_g1_active = false;
    screen_reverse = false;
    blink_attribute_used = false;
    pending_wrap = false;
}

/* Put every mode back to the power-on default. Used by RIS (ESC c) and by
 * entering terminal mode, so a reset genuinely resets rather than leaving a
 * stale mode behind from earlier output. */
static void reset_terminal_state(void)
{
    current_attributes = 0;
    saved_attributes = 0;
    charset_g0 = CHARSET_ASCII;
    charset_g1 = CHARSET_ASCII;
    charset_g1_active = false;
    screen_reverse = false;
    cursor_keys_application = false;
    keypad_application = false;
    /* Reset-to-initial-state means ANSI mode, so this also unwinds a stuck
     * VT52 session - the software way back for a host that entered VT52 and
     * then died without sending ESC <. */
    vt52_mode = false;
    vt52_graphics = false;
    vt52_state = VT52_STATE_GROUND;
    blink_attribute_used = false;
    blink_visible = true;
    /* RIS restores the cursor to visible, as power-on does; the user's menu
     * preference still decides whether it is drawn at all. */
    cursor_host_visible = true;
    cursor_phase_visible = true;
    cursor_painted = false;
    saved_cursor_x = 0;
    saved_cursor_y = 0;
}

/* RIS (ESC c): reset the terminal to its initial state. The screen is cleared,
 * the margins, tabs and modes all return to their defaults, and the cursor is
 * homed. */
static void terminal_reset(void)
{
    clear_screen_buffer();
    cursor_x = 0;
    cursor_y = 0;
    reset_terminal_state();
    pending_wrap = false;
    scroll_top = 0;
    scroll_bottom = (uint16_t)(terminal_rows - 1);
    origin_mode = false;
    tabs_reset();
    full_redraw_pending = true;
}

/* Map the final byte of a charset designation (ESC ( x) to a character set.
 *
 * Only the sets that can be rendered are distinguished; the rest - the
 * alternate ROM and the national replacement sets - fall back to ASCII, which
 * shows the right shape for everything except a handful of punctuation
 * positions rather than showing nothing at all. */
static uint8_t charset_from_final(uint8_t final_byte)
{
    switch (final_byte) {
    case '0': return CHARSET_SPECIAL; /* special graphics / line drawing */
    case 'A': return CHARSET_UK;      /* United Kingdom */
    default:  return CHARSET_ASCII;   /* 'B', '1', '2' and the rest */
    }
}

static void apply_color(uint8_t index)
{
    if (index >= TEXT_COLOR_COUNT) index = 0;
    color_selection = index;
}

static void apply_background(uint8_t index)
{
    if (index >= BACKGROUND_COUNT) index = 0;
    background_selection = index;
}

static void apply_autowrap(uint8_t index)
{
    autowrap_selection = index > 1 ? 1 : index;
    autowrap_enabled = autowrap_selection == 0;
    if (!autowrap_enabled) pending_wrap = false;
}

/* When enabled, LF implies CR: the cursor also returns to column 0. This is
 * what many CP/M setups expect, and it keeps the prompt at the left margin
 * even if the host only sends LF. */
static void apply_newline(uint8_t index)
{
    newline_selection = index > 1 ? 1 : index;
    newline_crlf = newline_selection == 0;
}

static void apply_cursor(uint8_t index)
{
    cursor_selection = index > 1 ? 1 : index;
    cursor_enabled = cursor_selection == 0;
}

/* The menu is a small state machine: the top level lists the settings, and
 * each entry opens a submenu of choices. Left/Right move between the top-level
 * entries and the current submenu's values; Enter opens a submenu or applies
 * the choice. */
enum {
    MENU_LEVEL_TOP = 0,   /* choosing which setting to edit */
    MENU_LEVEL_VALUES     /* choosing a value inside the setting */
};

static uint8_t menu_level = MENU_LEVEL_TOP;

/* Top-level entries, in display order. The last two are actions rather than
 * settings: they leave the menu, either storing the new selections or putting
 * back the ones that were in effect when it opened. */
enum {
    MENU_ITEM_FONT = 0,
    MENU_ITEM_COLOR,
    MENU_ITEM_BACKGROUND,
    MENU_ITEM_AUTOWRAP,
    MENU_ITEM_NEWLINE,
    MENU_ITEM_CURSOR,
    MENU_ITEM_EXIT_SAVE,
    MENU_ITEM_EXIT_DISCARD,
    MENU_ITEM_COUNT
};

static const char *const menu_item_names[MENU_ITEM_COUNT] = {
    "Font", "Colour", "Background", "Autowrap", "Newline", "Cursor",
    "Exit and save", "Exit without saving",
};

/* Action entries have no value submenu; selecting one performs an exit. */
static bool menu_item_is_action(uint8_t item)
{
    return item == MENU_ITEM_EXIT_SAVE || item == MENU_ITEM_EXIT_DISCARD;
}

static uint8_t menu_value_count(uint8_t item)
{
    switch (item) {
        case MENU_ITEM_FONT: return VT100_FONT_COUNT;
        case MENU_ITEM_COLOR: return TEXT_COLOR_COUNT;
        case MENU_ITEM_BACKGROUND: return BACKGROUND_COUNT;
        case MENU_ITEM_AUTOWRAP: return 2;
        case MENU_ITEM_NEWLINE: return 2;
        case MENU_ITEM_CURSOR: return 2;
        default: return 0;
    }
}

static uint8_t menu_value_at(uint8_t item)
{
    switch (item) {
        case MENU_ITEM_FONT: return font_selection;
        case MENU_ITEM_COLOR: return color_selection;
        case MENU_ITEM_BACKGROUND: return background_selection;
        case MENU_ITEM_AUTOWRAP: return autowrap_selection;
        case MENU_ITEM_NEWLINE: return newline_selection;
        case MENU_ITEM_CURSOR: return cursor_selection;
        default: return 0;
    }
}

/* Apply a new value to a setting. This only changes the live state; nothing is
 * written to flash here. Persisting is a deliberate, separate step so the
 * user controls when the display is disturbed - see menu_exit(). */
static void menu_set_value(uint8_t item, uint8_t value)
{
    switch (item) {
        /* A font change alters the terminal geometry, which the screen
         * contents cannot survive, so it is tracked separately. */
        case MENU_ITEM_FONT: apply_font(value); geometry_changed = true; break;
        case MENU_ITEM_COLOR: apply_color(value); break;
        case MENU_ITEM_BACKGROUND: apply_background(value); break;
        case MENU_ITEM_AUTOWRAP: apply_autowrap(value); break;
        case MENU_ITEM_NEWLINE: apply_newline(value); break;
        case MENU_ITEM_CURSOR: apply_cursor(value); break;
        default: break;
    }
}

/* The selections that were in effect when the menu was opened. "Exit without
 * saving" puts these back, so a menu opened by accident leaves the session
 * exactly as it was found. */
static struct {
    uint8_t font;
    uint8_t color;
    uint8_t background;
    uint8_t autowrap;
    uint8_t newline;
    uint8_t cursor;
} menu_saved_selections;

static void menu_snapshot_selections(void)
{
    menu_saved_selections.font = font_selection;
    menu_saved_selections.color = color_selection;
    menu_saved_selections.background = background_selection;
    menu_saved_selections.autowrap = autowrap_selection;
    menu_saved_selections.newline = newline_selection;
    menu_saved_selections.cursor = cursor_selection;
}

static void menu_restore_selections(void)
{
    /* apply_font() also recentres the cursor, so the font is only re-applied
     * when it really changed. Otherwise discarding a colour change would move
     * the cursor as a side effect. */
    if (font_selection != menu_saved_selections.font) {
        apply_font(menu_saved_selections.font);
    }
    apply_color(menu_saved_selections.color);
    apply_background(menu_saved_selections.background);
    apply_cursor(menu_saved_selections.cursor);
    apply_autowrap(menu_saved_selections.autowrap);
    apply_newline(menu_saved_selections.newline);
}

static void menu_step_value(int8_t direction)
{
    uint8_t count = menu_value_count(menu_item);
    if (count == 0) return;

    /* Step through the options, skipping any that would render the terminal
     * invisible. The entry stays listed in the menu so the choice is still
     * discoverable, it simply cannot be landed on. */
    uint8_t value = menu_value_at(menu_item);
    for (uint8_t attempt = 0; attempt < count; ++attempt) {
        if (direction > 0) {
            value = (uint8_t)((value + 1) % count);
        } else {
            value = (uint8_t)((value + count - 1) % count);
        }

        if (menu_item == MENU_ITEM_COLOR && !color_bg_visible(value, background_selection)) {
            continue;
        }
        if (menu_item == MENU_ITEM_BACKGROUND && !color_bg_visible(color_selection, value)) {
            continue;
        }
        menu_set_value(menu_item, value);
        return;
    }
}

static void menu_value_label(uint8_t item, uint8_t value, char *out, size_t size)
{
    switch (item) {
        case MENU_ITEM_FONT: {
            const vt100_font_t *entry = &VT100_FONTS[value];
            snprintf(out, size, "%s %s (%ux%u)", entry->family, entry->size,
                     terminal_columns, terminal_rows);
            break;
        }
        case MENU_ITEM_COLOR:
            snprintf(out, size, "%s", text_palette[value].name);
            break;
        case MENU_ITEM_BACKGROUND:
            snprintf(out, size, "%s", background_names[value]);
            break;
        case MENU_ITEM_AUTOWRAP:
            snprintf(out, size, "%s", value == 0 ? "On" : "Off");
            break;
        case MENU_ITEM_NEWLINE:
            snprintf(out, size, "%s", value == 0 ? "LF = CR+LF" : "LF only");
            break;
        case MENU_ITEM_CURSOR:
            snprintf(out, size, "%s", value == 0 ? "Shown" : "Hidden");
            break;
        default:
            snprintf(out, size, "?");
            break;
    }
}

/* Draw the setup menu. Top level lists the settings with their current values;
 * the value submenu shows every option for the selected setting.
 *
 * This paints the framebuffer directly and leaves `screen` untouched, so a live
 * terminal session keeps running underneath. */
static void draw_menu(void)
{
    uint16_t rows = terminal_rows;
    if (rows > MAX_SCREEN_ROWS) rows = MAX_SCREEN_ROWS;

    clear_framebuffer();

    uint16_t row = 0;
    if (row < rows) {
        draw_menu_text(row, 0, "RP2350 VT100 TERMINAL", true);
        ++row;
    }
    if (row < rows) {
        draw_menu_text(row, 0, hid_report_error ? "USB HID: REPORT ERROR" :
                       (keyboard_connected ? "USB keyboard: CONNECTED"
                                           : "USB keyboard: WAITING (connect to J2)"), false);
        ++row;
    }
    ++row; /* blank line */

    if (menu_level == MENU_LEVEL_TOP) {
        for (uint8_t item = 0; item < MENU_ITEM_COUNT && row + item < rows; ++item) {
            bool selected = item == menu_item;
            char label[64];
            if (menu_item_is_action(item)) {
                /* Action rows leave the menu rather than holding a value, so
                 * their name is the whole line. */
                snprintf(label, sizeof(label), "%s", menu_item_names[item]);
            } else {
                char value[48];
                menu_value_label(item, menu_value_at(item), value, sizeof(value));
                snprintf(label, sizeof(label), "%-11s %s", menu_item_names[item], value);
            }
            draw_menu_text(row + item, 1, selected ? ">" : " ", false);
            draw_menu_text(row + item, 3, label, selected);
        }
        row += MENU_ITEM_COUNT;

        if (row + 1 < rows) {
            draw_menu_text(row + 1, 0, menu_over_terminal
                ? "Up/Down select, Right/Enter opens. Esc = exit without saving."
                : "Up/Down select, Right/Enter opens. Esc starts.", false);
        }
    } else {
        uint8_t count = menu_value_count(menu_item);
        if (row < rows) {
            draw_menu_text(row, 0, menu_item_names[menu_item], true);
        }

        for (uint8_t index = 0; index < count && row + 1 + index < rows; ++index) {
            bool selected = index == menu_value_at(menu_item);
            char value[48];
            menu_value_label(menu_item, index, value, sizeof(value));
            draw_menu_text(row + 1 + index, 3, selected ? ">" : " ", false);
            draw_menu_text(row + 1 + index, 5, value, selected);
        }
        row += 1 + count;

        if (row + 1 < rows) {
            draw_menu_text(row + 1, 0, "Up/Down change, Left back, Enter accepts.", false);
        }
    }
}

/* Open the setup menu over a running session. The character buffer is
 * deliberately left alone so live output is not lost while the menu is up;
 * the screen is repainted from it when the menu closes. */
static void open_menu(void)
{
    menu_over_terminal = true;
    geometry_changed = false;
    menu_active = true;
    menu_level = MENU_LEVEL_TOP;
    menu_item = MENU_ITEM_FONT;
    /* The setup menu is the operator's recovery path: opening it takes the
     * terminal out of VT52 mode, so a host that switched to VT52 and then
     * died without sending ESC < cannot leave the screen unrecoverable. */
    vt52_mode = false;
    vt52_graphics = false;
    vt52_state = VT52_STATE_GROUND;
    menu_snapshot_selections();
    draw_menu();
}

/* Leave the setup menu, storing the selections only if asked.
 *
 * Having the two exits as menu entries means flash is written at a moment the
 * user chose, never as a side effect of changing a value. Discarding restores
 * the selections that were live when the menu opened, so the stored settings
 * and the visible ones can never drift apart. */
static void menu_exit(bool save)
{
    if (save) {
        settings_save();
    } else {
        menu_restore_selections();
        /* The font is back to what it was, so the buffered screen still
         * matches the geometry and there is nothing to throw away. */
        geometry_changed = false;
    }

    menu_active = false;
    menu_level = MENU_LEVEL_TOP;

    if (!menu_over_terminal) {
        /* Menu was the boot screen: start a fresh terminal. */
        begin_terminal_mode();
        return;
    }

    menu_over_terminal = false;

    if (geometry_changed) {
        /* The font changed, so the terminal is a different size now and the
         * previous screen content has no meaningful mapping. Start clean and
         * let the host repaint. */
        geometry_changed = false;
        clear_screen_buffer();
        cursor_x = 0;
        cursor_y = 0;
    }

    full_redraw_pending = false;
    draw_screen();
}

/* Switch from the setup menu to live terminal output.
 *
 * This deliberately does not touch flash. Entering terminal mode happens
 * automatically once the Z80 starts talking, so it must stay glitch free: an
 * automatic exit has no business rewriting stored settings. Persisting is a
 * deliberate act, done by menu_exit() from the "Exit and save" entry. */
static void begin_terminal_mode(void)
{
    menu_active = false;
    menu_over_terminal = false;
    geometry_changed = false;
    menu_level = MENU_LEVEL_TOP;
    clear_screen_buffer();
    apply_geometry();
    cursor_x = 0;
    cursor_y = 0;
    reset_terminal_state();
    pending_wrap = false;
    full_redraw_pending = false;
    draw_screen();
}

/* Restore the saved menu selections, falling back to the boot defaults when
 * the stored settings are missing or invalid. */
static void settings_load(void)
{
    const settings_t *stored = (const settings_t *)SETTINGS_ADDRESS;
    if (settings_valid(stored)) {
        apply_font(stored->font);
        apply_color(stored->color);
        apply_background(stored->background);
        apply_autowrap(stored->autowrap);
        apply_newline(stored->newline);
        apply_cursor(stored->cursor);
    } else {
        apply_font(DEFAULT_FONT_SELECTION);
        apply_color(DEFAULT_COLOR_SELECTION);
        apply_background(DEFAULT_BACKGROUND_SELECTION);
        apply_autowrap(DEFAULT_AUTOWRAP_SELECTION);
        apply_newline(DEFAULT_NEWLINE_SELECTION);
        apply_cursor(DEFAULT_CURSOR_SELECTION);
    }
}

/* Persist the current menu selections so the next power-up restores them.
 *
 * Interrupts are disabled on this core for the duration of the sector erase,
 * which is tens of milliseconds. If the Z80 happens to be streaming output at
 * that exact moment a few hundred bytes can be missed, so saving is left as an
 * explicit menu action rather than something that fires while you are typing.
 * At a CP/M prompt nothing is in flight and there is nothing to lose. */
static void settings_save(void)
{
    pending_settings.magic = SETTINGS_MAGIC;
    pending_settings.font = font_selection;
    pending_settings.color = color_selection;
    pending_settings.background = background_selection;
    pending_settings.autowrap = autowrap_selection;
    pending_settings.newline = newline_selection;
    pending_settings.cursor = cursor_selection;
    memset(pending_settings.reserved, 0, sizeof(pending_settings.reserved));
    pending_settings.checksum = settings_checksum(&pending_settings);

    flash_safe_execute(flash_write_callback, &pending_settings, 1000);
}

static uint16_t csi_parameter(uint8_t index, uint16_t default_value)
{
    if (index >= csi_count || csi_values[index] == 0) {
        return default_value;
    }
    return csi_values[index];
}

static uint16_t csi_raw_parameter(uint8_t index, uint16_t default_value)
{
    return index < csi_count ? csi_values[index] : default_value;
}

/* Answer a query from the host. Every ANSI response is a CSI sequence, so the
 * introducer is added here rather than at each call site. */
static void terminal_reply(const char *body)
{
    uart_putc(Z80_UART, 0x1b);
    uart_putc(Z80_UART, '[');
    while (*body != '\0') {
        uart_putc(Z80_UART, *body++);
    }
}

/* Send a raw response with no introducer.
 *
 * A VT52 reply is its own thing - ESC / Z, not a CSI sequence - so it cannot
 * go through terminal_reply(). */
static void terminal_reply_raw(const char *body)
{
    uart_putc(Z80_UART, 0x1b);
    while (*body != '\0') {
        uart_putc(Z80_UART, *body++);
    }
}

/* SGR: set character attributes. Parameters are applied in order, so a
 * sequence may both set and clear attributes. An empty parameter list, or a
 * leading 0, resets everything. */
static void terminal_sgr(void)
{
    uint8_t count = csi_count == 0 ? 1 : csi_count;
    for (uint8_t index = 0; index < count; ++index) {
        uint16_t value = index < csi_count ? csi_values[index] : 0;
        switch (value) {
        case 0:  current_attributes = 0; break;
        case 1:  current_attributes |= ATTR_BOLD; break;
        case 2:  current_attributes |= ATTR_DIM; break;
        case 4:  current_attributes |= ATTR_UNDERLINE; break;
        case 5:  current_attributes |= ATTR_BLINK; blink_attribute_used = true; break;
        case 7:  current_attributes |= ATTR_REVERSE; break;
        case 8:  current_attributes |= ATTR_INVISIBLE; break;
        /* Selective resets: each clears only its own attribute, leaving the
         * rest of the state alone. */
        case 22: current_attributes &= (uint8_t)~(ATTR_BOLD | ATTR_DIM); break;
        case 24: current_attributes &= (uint8_t)~ATTR_UNDERLINE; break;
        case 25: current_attributes &= (uint8_t)~ATTR_BLINK; break;
        case 27: current_attributes &= (uint8_t)~ATTR_REVERSE; break;
        case 28: current_attributes &= (uint8_t)~ATTR_INVISIBLE; break;
        default:
            /* Colours (30-37, 40-47, 39, 49) and anything else are accepted
             * and ignored: this is a monochrome-out-of-choice display, and
             * silently dropping them is better than emitting stray output. */
            break;
        }
    }
}

static void terminal_csi_command(char command, bool private_sequence)
{
    uint16_t amount;

    /* Any control sequence ends a pending wrap. */
    pending_wrap = false;

    /* DECSET (?h) and DECRST (?l) turn modes on and off. Modes that this
     * hardware cannot act on are accepted and ignored rather than rejected,
     * which is what a real VT100 does with modes it understands but that have
     * no visible effect here:
     *   3  DECCOLM column mode - the cell is 8 pixels, so 80 columns is the
     *              only width that fits and this cannot be honoured
     *   4  DECSCLM smooth vs jump scroll - scrolling is instantaneous
     *   8  DECARM auto-repeat - the host's keyboard repeat is not ours to set
     *   9  DECINLM interlace - not applicable to a DVI scanout
     */
    if (private_sequence && (command == 'h' || command == 'l')) {
        bool set = (command == 'h');
        for (uint8_t index = 0; index < csi_count; ++index) {
            switch (csi_values[index]) {
            case 1: /* DECCKM: cursor keys send SS3 instead of CSI */
                cursor_keys_application = set;
                break;
            case 2: /* DECANM: reset enters VT52 mode, set selects ANSI.
                     *
                     * Selecting ANSI also resets the character sets to ASCII,
                     * and leaving VT52 clears its graphics mode; otherwise a
                     * sequence sent in one mode could leave a character set
                     * latched that the other mode never asked for. */
                if (set) {
                    vt52_mode = false;
                    charset_g0 = CHARSET_ASCII;
                    charset_g1 = CHARSET_ASCII;
                    charset_g1_active = false;
                } else {
                    vt52_mode = true;
                    vt52_state = VT52_STATE_GROUND;
                    vt52_graphics = false;
                    charset_g0 = CHARSET_ASCII;
                    charset_g1 = CHARSET_ASCII;
                    charset_g1_active = false;
                }
                break;
            case 5: /* DECSCNM: reverse the whole screen */
                if (screen_reverse != set) {
                    screen_reverse = set;
                    full_redraw_pending = true;
                }
                break;
            case 6: /* DECOM: origin mode */
                origin_mode = set;
                /* The cursor is homed whenever origin mode changes. */
                cursor_y = set ? scroll_top : 0;
                cursor_x = 0;
                break;
            case 7: /* DECAWM: autowrap */
                autowrap_enabled = set;
                if (!set) pending_wrap = false;
                break;
            case 25: /* DECTCEM: show or hide the cursor.
                      * This is how CP/M software hides the cursor while it
                      * redraws, so honouring it matters for a tidy display. */
                cursor_host_visible = set;
                if (set) cursor_phase_visible = true;
                cursor_update();
                break;
            default:
                break;
            }
        }
        return;
    }

    /* LMN (?20h/?20l) has no private marker in the standard, so it arrives as
     * an ordinary CSI and is handled here. */
    if (!private_sequence && (command == 'h' || command == 'l')) {
        bool set = (command == 'h');
        for (uint8_t index = 0; index < csi_count; ++index) {
            if (csi_values[index] == 20) {
                newline_crlf = set;
            }
        }
        return;
    }

    switch (command) {
        case 'A':
            amount = csi_parameter(0, 1);
            cursor_y = clamp_row((int32_t)cursor_y - amount);
            break;
        case 'B':
            amount = csi_parameter(0, 1);
            cursor_y = clamp_row((int32_t)cursor_y + amount);
            break;
        case 'C':
            amount = csi_parameter(0, 1);
            cursor_x = cursor_x + amount >= terminal_columns ? terminal_columns - 1 : cursor_x + amount;
            break;
        case 'D':
            amount = csi_parameter(0, 1);
            cursor_x = amount > cursor_x ? 0 : cursor_x - amount;
            break;
        case 'G': /* CHA: absolute column */
            cursor_x = csi_parameter(0, 1) - 1;
            if (cursor_x >= terminal_columns) cursor_x = terminal_columns - 1;
            break;
        case 'd': /* VPA: absolute row */
            cursor_y = clamp_row(csi_parameter(0, 1) - 1);
            break;
        case 'H':
        case 'f': {
            /* In origin mode, row 1 is the top margin and the cursor cannot
             * leave the scrolling region. */
            int32_t row = (int32_t)csi_parameter(0, 1) - 1;
            if (origin_mode) row += scroll_top;
            cursor_y = clamp_row(row);
            cursor_x = csi_parameter(1, 1) - 1;
            if (cursor_x >= terminal_columns) cursor_x = terminal_columns - 1;
            break;
        }
        case 'r': { /* DECSTBM: set scrolling region */
            uint16_t top = csi_parameter(0, 1);
            uint16_t bottom = csi_parameter(1, (uint16_t)terminal_rows);
            if (top < 1) top = 1;
            if (bottom > terminal_rows) bottom = terminal_rows;
            if (top < bottom) {
                scroll_top = (uint16_t)(top - 1);
                scroll_bottom = (uint16_t)(bottom - 1);
                /* DECSTBM homes the cursor; origin mode decides where. */
                cursor_y = origin_mode ? scroll_top : 0;
                cursor_x = 0;
            }
            break;
        }
        case 'g': /* TBC: tab clear */
            switch (csi_raw_parameter(0, 0)) {
            case 0: /* clear the stop at the cursor */
                if (cursor_x < terminal_columns) {
                    tab_stops[cursor_x >> 3] &= (uint8_t)~(1u << (cursor_x & 7));
                }
                break;
            case 3: /* clear all stops */
                tab_clear_all();
                break;
            default:
                break;
            }
            break;
        case 'J':
            switch (csi_raw_parameter(0, 0)) {
            case 0:
                clear_line_range(cursor_y, cursor_x, terminal_columns);
                for (uint16_t row = cursor_y + 1; row < terminal_rows; ++row) {
                    clear_line(row);
                }
                full_redraw_pending = true;
                break;
            case 1:
                for (uint16_t row = 0; row < cursor_y; ++row) {
                    clear_line(row);
                }
                clear_line_range(cursor_y, 0, cursor_x + 1);
                full_redraw_pending = true;
                break;
            case 2:
                for (uint16_t row = 0; row < terminal_rows; ++row) {
                    clear_line(row);
                }
                /* DEC is explicit that ED 2 erases the display WITHOUT moving
                 * the cursor. Homing it here would break a host that clears
                 * the screen and then continues writing where it was. */
                full_redraw_pending = true;
                break;
            default:
                break;
            }
            break;
        case 'K':
            switch (csi_raw_parameter(0, 0)) {
            case 0:
                clear_line_range(cursor_y, cursor_x, terminal_columns);
                break;
            case 1:
                clear_line_range(cursor_y, 0, cursor_x + 1);
                break;
            case 2:
                clear_line(cursor_y);
                break;
            default:
                break;
            }
            full_redraw_pending = true;
            break;
        case 'm':
            terminal_sgr();
            break;
        case 'n': /* DSR: device status report */
            switch (csi_raw_parameter(0, 0)) {
            case 5: /* "are you there?" */
                terminal_reply("0n");
                break;
            case 6: { /* CPR: report the cursor position, 1-based.
                      * DEC numbers the lines relative to the top margin while
                      * origin mode is set, so the row is offset by the margin.
                      * There are no left/right margins on a VT100, so the
                      * column is unaffected. */
                unsigned report_row = (unsigned)(cursor_y + 1);
                if (origin_mode) {
                    report_row = (unsigned)(cursor_y - scroll_top) + 1u;
                }
                char body[16];
                snprintf(body, sizeof(body), "%u;%uR",
                         report_row, (unsigned)(cursor_x + 1));
                terminal_reply(body);
                break;
            }
            default:
                break;
            }
            break;
        case 'c': /* DA: primary device attributes.
                   * The request is CSI c or CSI 0 c, and the reply identifies
                   * a VT100 with the advanced video option, which is what this
                   * emulation provides (attributes and reverse video). */
            terminal_reply("?1;2c");
            break;
        case 'y': /* DECTST: confidence test - accepted and ignored */
        case 'q': /* DECLL: set keyboard LEDs - no LEDs to drive */
            break;
        default:
            break;
    }
}

/* ---------------------------------------------------------------------------
 * VT52 mode
 *
 * A VT52 sequence is ESC followed by a single final character, so the parser
 * is deliberately tiny: one character of lookahead, except for ESC Y which
 * takes two coordinate bytes.
 *
 * The coordinates of ESC Y are the ones worth being careful about. Each is
 * sent as a single printable byte in which the value is the position plus 32
 * (0x1F), giving a 1-based row and column on the wire that maps onto our
 * 0-based cursor directly. A real VT52 ignored the top three bits, and the
 * value is clamped rather than wrapped so a malformed coordinate parks the
 * cursor at the margin instead of on the opposite edge of the screen.
 * ------------------------------------------------------------------------- */

/* Both parsers share the character-writing core, which is defined further
 * down alongside the ANSI state machine it serves. */
static void terminal_putc_selected(uint8_t value, uint8_t charset);
static void ansi_putc(uint8_t value);

/* Turn a VT52 coordinate byte into a 0-based column or row index.
 *
 * The byte is position + 0x1F, so subtracting 0x1F yields the 1-based value
 * and then one more gives the index. Values below 0x1F would underflow, and
 * anything past the far edge would run off the screen, so the result is
 * clamped into range. */
static uint16_t vt52_coordinate(uint8_t value, uint16_t limit)
{
    if (value < 0x20) return 0;
    uint16_t position = (uint16_t)(value - 0x1f);
    if (position == 0) return 0;          /* position 0 means "1st" */
    if (position > limit) return (uint16_t)(limit - 1);
    return (uint16_t)(position - 1);
}

static void vt52_execute(uint8_t command)
{
    switch (command) {
    case 'A': /* cursor up */
        if (cursor_y > 0) --cursor_y;
        break;
    case 'B': /* cursor down */
        if (cursor_y + 1 < terminal_rows) ++cursor_y;
        break;
    case 'C': /* cursor right */
        if (cursor_x + 1 < terminal_columns) ++cursor_x;
        break;
    case 'D': /* cursor left */
        if (cursor_x > 0) --cursor_x;
        break;
    case 'F': /* enter graphics mode */
        vt52_graphics = true;
        break;
    case 'G': /* exit graphics mode */
        vt52_graphics = false;
        break;
    case 'H': /* cursor to home position */
        cursor_x = 0;
        cursor_y = 0;
        break;
    case 'I': /* reverse line feed. A VT52 has no margins, so the whole screen
               * is the region and scrolling at the top is equivalent to
               * scrolling the full screen down by one line. */
        index_up();
        break;
    case 'J': /* erase from the cursor to the end of the screen */
        clear_line_range(cursor_y, cursor_x, terminal_columns);
        for (uint16_t row = cursor_y + 1; row < terminal_rows; ++row) {
            clear_line(row);
        }
        full_redraw_pending = true;
        break;
    case 'K': /* erase from the cursor to the end of the line */
        clear_line_range(cursor_y, cursor_x, terminal_columns);
        full_redraw_pending = true;
        break;
    case 'Z': /* identify: report "I am a VT52 emulated by a VT100" */
        terminal_reply_raw("/Z");
        break;
    case '=': /* enter alternate keypad mode */
        keypad_application = true;
        break;
    case '>': /* exit alternate keypad mode */
        keypad_application = false;
        break;
    case '<': /* leave VT52 mode, enter ANSI/VT100 mode */
        vt52_mode = false;
        vt52_graphics = false;
        break;
    case 'c': /* RIS: reset to initial state. DEC lists ESC c as an ANSI-mode
               * control, but what it does is restore the power-on state, which
               * is ANSI mode - so honouring it in VT52 mode gives the host a
               * way back from a stuck VT52 session without a power cycle. */
        terminal_reset();
        break;
    /* The printer and autoprint controls (ESC V, ESC W, ESC X, ESC ^, ESC _,
     * ESC ], ESC Z's autoprint relatives) describe hardware this terminal does
     * not have. They are consumed so they cannot leak onto the screen. */
    default:
        break;
    }
}

static void vt52_putc(uint8_t value)
{
    switch (vt52_state) {
    case VT52_STATE_ESC:
        vt52_state = VT52_STATE_GROUND;
        if (value == 'Y') {
            vt52_state = VT52_STATE_Y_ROW;
            return;
        }
        vt52_execute(value);
        return;

    case VT52_STATE_Y_ROW:
        vt52_row = vt52_coordinate(value, terminal_rows);
        vt52_state = VT52_STATE_Y_COLUMN;
        return;

    case VT52_STATE_Y_COLUMN:
        vt52_state = VT52_STATE_GROUND;
        cursor_y = vt52_row;
        cursor_x = vt52_coordinate(value, terminal_columns);
        return;

    default:
        break;
    }

    if (value == 0x1b) {
        vt52_state = VT52_STATE_ESC;
        pending_wrap = false;
        return;
    }

    /* Everything below is the same on a VT52 as on a VT100, so the shared
     * handling in terminal_putc() is reused rather than duplicated. */
    if (value == 0x0e) {
        charset_g1_active = true;
        return;
    }
    if (value == 0x0f) {
        charset_g1_active = false;
        return;
    }
    if (value >= 0x20 && value < 0x7f) {
        /* In graphics mode the printable range is reinterpreted as the DEC
         * line-drawing set, which is why ESC F/ESC G exist. */
        terminal_putc_selected(value,
                               vt52_graphics ? CHARSET_SPECIAL : CHARSET_ASCII);
        return;
    }

    ansi_putc(value);
}

/* Route a byte to whichever protocol is active. */
static void terminal_putc(uint8_t value)
{
    if (vt52_mode) {
        vt52_putc(value);
    } else {
        ansi_putc(value);
    }
}

/* Store one printable character at the cursor, handling the deferred wrap and
 * the scroll that a wrap can force, then advance the cursor.
 *
 * `charset` is latched into the cell rather than kept as global state, so text
 * already on screen keeps the character set it was written with. Both the
 * ANSI and the VT52 parsers come through here, which is what keeps their
 * wrapping and scrolling behaviour identical. */
static void terminal_putc_selected(uint8_t value, uint8_t charset)
{
    /* A pending wrap is resolved by the arrival of this character: advance
     * to the next row first, then store the character there. The same margin
     * rule as every other downward move applies, so the row change is done by
     * index_down() rather than a private copy. */
    if (pending_wrap) {
        pending_wrap = false;
        cursor_x = 0;
        index_down();
    }

    screen[cursor_y][cursor_x] = (char)value;
    screen_attributes[cursor_y][cursor_x] =
        (uint8_t)(current_attributes |
                  (uint8_t)(charset << ATTR_CHARSET_SHIFT));

    if (menu_active) {
        /* The menu currently owns the framebuffer, so only the character
         * buffer is updated. The screen is repainted when the menu closes,
         * which is what keeps the Z80's output from being lost while the
         * menu is open. */
        full_redraw_pending = true;
    } else {
        /* Repaint the cell from the buffer, which also drops any cursor
         * block that was overlaying it. The cursor is put back on its new
         * position by the next cursor_update() pass. */
        draw_cell_at_buffer(cursor_x, cursor_y);
    }

    if (cursor_x + 1 >= terminal_columns) {
        if (autowrap_enabled) {
            /* Hold at the last column; the next character wraps. */
            pending_wrap = true;
        }
        /* With autowrap disabled the cursor simply stays on the last
         * column and further characters overwrite that cell. */
    } else {
        ++cursor_x;
    }
}

/* Escape sequence parsing states. VT100 sequences are ESC + optional
 * intermediate bytes (0x20-0x2F) + a final byte (0x30-0x7E), or ESC [ followed
 * by parameters, optional intermediates, and a final byte. Tracking the
 * intermediates matters because sequences such as ESC ( B (select charset)
 * would otherwise leak the trailing B onto the screen and shift the CP/M
 * prompt to the right. */
enum {
    ESC_NONE = 0,
    ESC_INTRODUCER,  /* saw ESC, awaiting the sequence body */
    ESC_INTERMEDIATE, /* saw ESC plus intermediate bytes, awaiting the final */
    CSI_ENTRY,        /* saw ESC [, collecting parameters */
    CSI_INTERMEDIATE  /* saw ESC [ ... plus intermediate bytes */
};

/* The ANSI escape state machine. In VT52 mode this is still used for the
 * control characters the two protocols share (CR, LF, BS, TAB, SO and SI);
 * vt52_putc() handles everything else and never sets escape_state, so the
 * ANSI parser stays in ESC_NONE and cannot mistake VT52 bytes for a sequence
 * it should assemble. */
/* Execute one C0 control character. Shared by the ground state and by a
 * control character embedded inside a sequence, because DEC specifies that an
 * embedded control character is executed as soon as it is seen and the
 * sequence then continues with the next byte. */
static void terminal_control_character(uint8_t value)
{
    if (value == '\r') {
        cursor_x = 0;
        pending_wrap = false;
    } else if (value == '\n' || value == 0x0b || value == 0x0c) {
        /* DEC: VT (0x0B) and FF (0x0C) are both interpreted as LF. CP/M
         * software uses FF to advance the page, so treating it as a plain
         * line feed is required, not merely tidy. */
        pending_wrap = false;
        /* With LF-implies-CR enabled, also return to the left margin so a
         * host that sends only LF still starts the next line at column 0. */
        if (newline_crlf) cursor_x = 0;
        /* Scrolling only happens when already AT the bottom margin, and only
         * the region between the margins moves. */
        index_down();
    } else if (value == '\b') {
        pending_wrap = false;
        if (cursor_x > 0) --cursor_x;
    } else if (value == '\t') {
        pending_wrap = false;
        tab_forward();
    } else if (value == 0x0e) {
        /* SO (Shift Out): map the G1 set onto the keyboard-in/display-out
         * side, which is how CP/M software switches to the line-drawing set
         * after designating it with ESC ) 0. */
        charset_g1_active = true;
        pending_wrap = false;
    } else if (value == 0x0f) {
        /* SI (Shift In): back to G0. */
        charset_g1_active = false;
        pending_wrap = false;
    } else if (value == 0x00 || value == 0x7f) {
        /* NUL and DEL are ignored on input, so they do not even disturb a
         * pending wrap. */
    } else {
        /* Bell and the remaining unhandled codes have no displayable effect,
         * but they do end a pending wrap. */
        pending_wrap = false;
    }
}

static void ansi_putc(uint8_t value)
{
    /* Control characters are excluded from the syntax of a control sequence,
     * but DEC specifies that one embedded inside a sequence is EXECUTED as
     * soon as it is seen, after which the sequence continues with the next
     * byte. That is why a CR arriving mid-sequence still returns the cursor to
     * the left margin, and why the rest of the sequence is not swallowed.
     *
     * Two codes are the exceptions: CAN (0x18) and SUB (0x1A) abort the
     * sequence outright, and ESC abandons it and starts a new one. */
    bool is_control = (value < 0x20) || (value == 0x7f);
    if (is_control && value != 0x1b) {
        if (value == 0x18 || value == 0x1a) {
            escape_state = ESC_NONE;
            return;
        }
        if (escape_state != ESC_NONE) {
            /* Run the control character, then keep collecting the sequence. */
            terminal_control_character(value);
            return;
        }
    }

    switch (escape_state) {
    case ESC_INTRODUCER:
        if (value == '[') {
            escape_state = CSI_ENTRY;
            csi_count = 0;
            csi_value = 0;
            csi_private = false;
            return;
        }
        if (value >= 0x20 && value <= 0x2f) {
            /* Intermediate byte: keep consuming until the final byte. The
             * value is remembered because it distinguishes whole families of
             * sequences - ESC ( B is charset selection while ESC # 8 is the
             * alignment test, and both are "ESC + intermediate + final". */
            esc_intermediate = value;
            escape_state = ESC_INTERMEDIATE;
            return;
        }
        if (value >= 0x30 && value <= 0x7e) {
            escape_state = ESC_NONE;
            switch (value) {
            case '7': /* DECSC: save cursor position, attributes and charset */
                saved_cursor_x = cursor_x;
                saved_cursor_y = cursor_y;
                saved_attributes = current_attributes;
                saved_charset_g0 = charset_g0;
                saved_charset_g1 = charset_g1;
                saved_charset_g1_active = charset_g1_active;
                break;
            case '8': /* DECRC: restore cursor position, attributes and charset.
                      * DEC also restores the character set, which is easy to
                      * overlook and shows up as text drawn in the wrong set
                      * after a save/restore pair. */
                cursor_x = saved_cursor_x;
                cursor_y = saved_cursor_y;
                current_attributes = saved_attributes;
                charset_g0 = saved_charset_g0;
                charset_g1 = saved_charset_g1;
                charset_g1_active = saved_charset_g1_active;
                break;
            case 'D': /* IND: index (down one line, scrolling at the margin) */
                index_down();
                break;
            case 'E': /* NEL: next line */
                cursor_x = 0;
                index_down();
                break;
            case 'H': /* HTS: set a tab stop at the cursor */
                if (cursor_x < terminal_columns) tab_set(cursor_x);
                break;
            case 'M': /* RI: reverse index (up one line, scrolling backwards) */
                index_up();
                break;
            case 'c':
                terminal_reset();
                break;
            case 'Z': /* DECID: identify terminal. Obsolete form of DA; DEC
                       * documents the response as identical to CSI c. */
                terminal_reply("?1;2c");
                break;
            case '=': /* DECKPAM: keypad sends application sequences */
                keypad_application = true;
                break;
            case '>': /* DECKPNM: keypad sends digits */
                keypad_application = false;
                break;
            case 'N': /* SS2: shift the next character to G2, which is ASCII */
            case 'O': /* SS3: shift the next character to G3, which is ASCII */
                break;
            default:
                break;
            }
            return;
        }
        escape_state = ESC_NONE;
        break;

    case ESC_INTERMEDIATE:
        if (value >= 0x20 && value <= 0x2f) return;
        if (value >= 0x30 && value <= 0x7e) {
            escape_state = ESC_NONE;
            switch (esc_intermediate) {
            case '(': /* designate the G0 character set */
                charset_g0 = charset_from_final(value);
                break;
            case ')': /* designate the G1 character set */
                charset_g1 = charset_from_final(value);
                break;
            case '#':
                /* DECALN (ESC # 8) fills the screen with 'E' for alignment.
                 * DECDHL/DECDWL (ESC # 3-6) ask for double-width or
                 * double-height lines, which an 8-pixel cell cannot render;
                 * they are accepted and ignored. */
                if (value == '8') {
                    for (uint16_t row = 0; row < terminal_rows; ++row) {
                        memset(screen[row], 'E', MAX_SCREEN_COLUMNS);
                        memset(screen_attributes[row], 0, MAX_SCREEN_COLUMNS);
                    }
                    cursor_x = 0;
                    cursor_y = 0;
                    full_redraw_pending = true;
                }
                break;
            default:
                break;
            }
            return;
        }
        escape_state = ESC_NONE;
        break;

    case CSI_ENTRY:
        if (value >= '0' && value <= '9') {
            if (csi_value < 0x1000) {
                csi_value = (uint16_t)(csi_value * 10 + (value - '0'));
            }
            return;
        }
        if (value == ';') {
            if (csi_count < MAX_CSI_PARAMS) csi_values[csi_count++] = csi_value;
            csi_value = 0;
            return;
        }
        if (value == '?' || value == '>' || value == '!' || value == '=') {
            csi_private = true;
            return;
        }
        if (value >= 0x20 && value <= 0x2f) {
            escape_state = CSI_INTERMEDIATE;
            return;
        }
        if (value >= 0x40 && value <= 0x7e) {
            if (csi_count < MAX_CSI_PARAMS) csi_values[csi_count++] = csi_value;
            terminal_csi_command((char)value, csi_private);
            escape_state = ESC_NONE;
            return;
        }
        escape_state = ESC_NONE;
        break;

    case CSI_INTERMEDIATE:
        if (value >= 0x20 && value <= 0x2f) return;
        escape_state = ESC_NONE;
        return;

    default:
        break;
    }

    if (value == 0x1b) {
        escape_state = ESC_INTRODUCER;
        pending_wrap = false;
    } else if (value >= 0x20 && value < 0x7f) {
        /* The charset is resolved now and latched into the cell, so later
         * selecting a different set cannot retroactively restyle this
         * character - which is what a real VT100 does. */
        terminal_putc_selected(value, charset_g1_active ? charset_g1 : charset_g0);
    } else {
        terminal_control_character(value);
    }
}

/* Translate a HID usage ID to the ASCII character produced with Ctrl held.
 * The VT100 and CP/M convention is the familiar control codes: Ctrl+A is 0x01
 * through Ctrl+Z as 0x1A, plus the punctuation forms such as Ctrl+[ = ESC and
 * Ctrl+? = DEL. */
static uint8_t control_character(uint8_t keycode)
{
    switch (keycode) {
        /* Letters A-Z occupy 0x04-0x1D in HID order. */
        case 0x04: return 0x01; /* A */
        case 0x05: return 0x02; /* B */
        case 0x06: return 0x03; /* C */
        case 0x07: return 0x04; /* D */
        case 0x08: return 0x05; /* E */
        case 0x09: return 0x06; /* F */
        case 0x0A: return 0x07; /* G */
        case 0x0B: return 0x08; /* H */
        case 0x0C: return 0x09; /* I */
        case 0x0D: return 0x0A; /* J */
        case 0x0E: return 0x0B; /* K */
        case 0x0F: return 0x0C; /* L */
        case 0x10: return 0x0D; /* M */
        case 0x11: return 0x0E; /* N */
        case 0x12: return 0x0F; /* O */
        case 0x13: return 0x10; /* P */
        case 0x14: return 0x11; /* Q */
        case 0x15: return 0x12; /* R */
        case 0x16: return 0x13; /* S */
        case 0x17: return 0x14; /* T */
        case 0x18: return 0x15; /* U */
        case 0x19: return 0x16; /* V */
        case 0x1A: return 0x17; /* W */
        case 0x1B: return 0x18; /* X */
        case 0x1C: return 0x19; /* Y */
        case 0x1D: return 0x1A; /* Z */
        /* The remaining control codes available on a standard keyboard. */
        case HID_KEY_SPACE: return 0x00;      /* Ctrl+Space = NUL */
        case HID_KEY_BACKSPACE: return 0x08;  /* Ctrl+H */
        case HID_KEY_TAB: return 0x09;        /* Ctrl+I */
        case HID_KEY_ENTER: return 0x0A;      /* Ctrl+J */
        case HID_KEY_ESCAPE: return 0x1B;
        case HID_KEY_BRACKET_LEFT: return 0x1B;   /* Ctrl+[ = ESC */
        case HID_KEY_BACKSLASH: return 0x1C;      /* Ctrl+\ = FS */
        case HID_KEY_BRACKET_RIGHT: return 0x1D;  /* Ctrl+] = GS */
        case HID_KEY_MINUS: return 0x1F;          /* Ctrl+_ = US */
        case HID_KEY_SLASH: return 0x7F;          /* Ctrl+? = DEL */
        case HID_KEY_2: return 0x00;              /* Ctrl+@ = NUL */
        default: break;
    }

    /* Fall back to the unshifted ASCII letter for any other printable key, so
     * Ctrl+<key> always produces a control code rather than nothing. */
    if (keycode < 128) {
        uint8_t plain = keycode2ascii[keycode][0];
        if (plain >= 'a' && plain <= 'z') return (uint8_t)(plain - 'a' + 1);
        if (plain >= 'A' && plain <= 'Z') return (uint8_t)(plain - 'A' + 1);
    }
    return 0;
}

static bool key_in_report(hid_keyboard_report_t const *report, uint8_t keycode)
{
    for (uint8_t index = 0; index < 6; ++index) {
        if (report->keycode[index] == keycode) return true;
    }
    return false;
}

/* Handle a key press while the setup menu is showing. */
static void menu_handle_key(uint8_t keycode)
{
    switch (keycode) {
        case HID_KEY_ARROW_UP:
            if (menu_level == MENU_LEVEL_TOP) {
                menu_item = (uint8_t)((menu_item + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT);
            } else {
                menu_step_value(-1);
            }
            break;
        case HID_KEY_ARROW_DOWN:
            if (menu_level == MENU_LEVEL_TOP) {
                menu_item = (uint8_t)((menu_item + 1) % MENU_ITEM_COUNT);
            } else {
                menu_step_value(1);
            }
            break;
        case HID_KEY_ARROW_RIGHT:
            /* Move to the next setting. Enter (or Right) opens a submenu, but
             * the exit entries have no submenu, so Right is inert on them. */
            if (menu_level == MENU_LEVEL_VALUES) {
                menu_level = MENU_LEVEL_TOP;
                menu_item = (uint8_t)((menu_item + 1) % MENU_ITEM_COUNT);
            } else if (menu_item_is_action(menu_item)) {
                return;
            } else {
                menu_level = MENU_LEVEL_VALUES;
            }
            break;
        case HID_KEY_ARROW_LEFT:
            /* Left backs out of a submenu, or steps to the previous setting. */
            if (menu_level == MENU_LEVEL_VALUES) {
                menu_level = MENU_LEVEL_TOP;
            } else {
                menu_item = (uint8_t)((menu_item + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT);
            }
            break;
        case HID_KEY_ENTER:
        case HID_KEY_KEYPAD_ENTER:
            if (menu_level == MENU_LEVEL_TOP) {
                if (menu_item == MENU_ITEM_EXIT_SAVE) {
                    menu_exit(true);
                    return;
                }
                if (menu_item == MENU_ITEM_EXIT_DISCARD) {
                    menu_exit(false);
                    return;
                }
                menu_level = MENU_LEVEL_VALUES;
            } else {
                /* Value chosen: back out, leaving it applied. */
                menu_level = MENU_LEVEL_TOP;
            }
            break;
        case HID_KEY_ESCAPE:
            /* Esc always leaves the menu, and always without saving. It is the
             * one keystroke that cannot surprise you with a flash write. */
            menu_exit(false);
            return;
        default:
            return;
    }
    draw_menu();
}

/* Translate one key press into the bytes to send to the host, applying the
 * active modifiers. Returns the number of bytes written to `out`. */
static uint8_t key_to_bytes(uint8_t keycode, uint8_t modifier, uint8_t *out, uint8_t capacity)
{
    bool ctrl = (modifier & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) != 0;
    bool shift = (modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;
    bool alt = (modifier & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)) != 0;
    (void)alt; /* AltGr and Alt are passed through as the underlying character. */

    if (capacity == 0) return 0;

    /* Ctrl+key produces a control code. This is what CP/M programs such as
     * WordStar rely on for commands like Ctrl+K, Ctrl+Q, and Ctrl+X. */
    if (ctrl) {
        uint8_t control = control_character(keycode);
        /* Ctrl+Enter, Ctrl+Tab and friends keep their usual control meaning. */
        if (keycode == HID_KEY_ENTER) control = 0x0A;
        if (keycode == HID_KEY_TAB) control = 0x09;
        if (keycode == HID_KEY_BACKSPACE) control = 0x08;

        if (control != 0 || keycode == HID_KEY_SPACE || keycode == HID_KEY_2) {
            out[0] = control;
            return 1;
        }
        /* Ctrl with a key that has no control code sends nothing. */
        return 0;
    }

    uint8_t character = keycode < 128 ? keycode2ascii[keycode][shift ? 1 : 0] : 0;
    /* LNM also governs the RETURN key, not just incoming LF: in newline mode
     * the key sends CR followed by LF, and in the default line-feed mode it
     * sends CR alone. This is opt-in - a host has to ask for newline mode - so
     * it cannot disturb the default CP/M behaviour. */
    if (character == '\r' && newline_crlf) {
        if (capacity < 2) return 0;
        out[0] = '\r';
        out[1] = '\n';
        return 2;
    }

    /* VT52 mode changes what the special keys SEND, not just how the screen is
     * driven. This has to be tested BEFORE the plain-character path, because
     * the keypad keys also have ASCII digits: in VT52 keypad application mode
     * the 5 key must send ESC ? u, not '5', and the character table would
     * otherwise swallow it. DECCKM does not apply here either - DEC documents
     * it as only effective when DECANM is set to ANSI - so the cursor keys
     * always send the single-character VT52 codes. */
    if (vt52_mode) {
        const char *vt52 = NULL;
        switch (keycode) {
        /* NOTE: the octal \033 form is required here, not \x1b. In C a hex
         * escape is greedy, so "\x1bA" is the single character 0x1BA - the
         * letters A-F and the digits 0-9 continue the escape rather than
         * ending it. These four codes are the ones where that bites; the
         * others below start with a non-hex letter and are safe either way. */
        case HID_KEY_ARROW_UP:    vt52 = "\033A"; break;
        case HID_KEY_ARROW_DOWN:  vt52 = "\033B"; break;
        case HID_KEY_ARROW_RIGHT: vt52 = "\033C"; break;
        case HID_KEY_ARROW_LEFT:  vt52 = "\033D"; break;
        /* PF1-PF4 are the same in both modes. */
        case HID_KEY_F1: vt52 = "\x1bP"; break;
        case HID_KEY_F2: vt52 = "\x1bQ"; break;
        case HID_KEY_F3: vt52 = "\x1bR"; break;
        case HID_KEY_F4: vt52 = "\x1bS"; break;
        /* The auxiliary keypad, but only in its application form. In numeric
         * mode these must fall through and send the engraved character, so
         * each case is conditional rather than returning outright. */
        case HID_KEY_KEYPAD_0:        vt52 = keypad_application ? "\x1b?p" : NULL; break;
        case HID_KEY_KEYPAD_1:        vt52 = keypad_application ? "\x1b?q" : NULL; break;
        case HID_KEY_KEYPAD_2:        vt52 = keypad_application ? "\x1b?r" : NULL; break;
        case HID_KEY_KEYPAD_3:        vt52 = keypad_application ? "\x1b?s" : NULL; break;
        case HID_KEY_KEYPAD_4:        vt52 = keypad_application ? "\x1b?t" : NULL; break;
        case HID_KEY_KEYPAD_5:        vt52 = keypad_application ? "\x1b?u" : NULL; break;
        case HID_KEY_KEYPAD_6:        vt52 = keypad_application ? "\x1b?v" : NULL; break;
        case HID_KEY_KEYPAD_7:        vt52 = keypad_application ? "\x1b?w" : NULL; break;
        case HID_KEY_KEYPAD_8:        vt52 = keypad_application ? "\x1b?x" : NULL; break;
        case HID_KEY_KEYPAD_9:        vt52 = keypad_application ? "\x1b?y" : NULL; break;
        case HID_KEY_KEYPAD_SUBTRACT: vt52 = keypad_application ? "\x1b?m" : NULL; break;
        case HID_KEY_KEYPAD_COMMA:    vt52 = keypad_application ? "\x1b?l" : NULL; break;
        case HID_KEY_KEYPAD_DECIMAL:  vt52 = keypad_application ? "\x1b?n" : NULL; break;
        case HID_KEY_KEYPAD_ENTER:    vt52 = keypad_application ? "\x1b?M" : NULL; break;
        default: break;
        }
        if (vt52 != NULL) {
            uint8_t length = 0;
            while (length < capacity && vt52[length] != '\0') {
                out[length] = (uint8_t)vt52[length];
                ++length;
            }
            return length;
        }
    }

    /* The numeric keypad behaves differently in the two keypad modes. In
     * application mode (DECKPAM) each key sends its own escape sequence; in
     * numeric mode (DECKPNM) it sends the digit or punctuation.
     *
     * This is tested BEFORE the plain-character path for the same reason as
     * the VT52 case above: the keypad keys carry ASCII digits and a full stop,
     * so in application mode they would otherwise be sent as '5' and '.' and
     * the ESC O forms could never be reached. */
    if (keypad_application) {
        const char *keypad = NULL;
        switch (keycode) {
        case HID_KEY_KEYPAD_0: keypad = "\x1bOp"; break;
        case HID_KEY_KEYPAD_1: keypad = "\x1bOq"; break;
        case HID_KEY_KEYPAD_2: keypad = "\x1bOr"; break;
        case HID_KEY_KEYPAD_3: keypad = "\x1bOs"; break;
        case HID_KEY_KEYPAD_4: keypad = "\x1bOt"; break;
        case HID_KEY_KEYPAD_5: keypad = "\x1bOu"; break;
        case HID_KEY_KEYPAD_6: keypad = "\x1bOv"; break;
        case HID_KEY_KEYPAD_7: keypad = "\x1bOw"; break;
        case HID_KEY_KEYPAD_8: keypad = "\x1bOx"; break;
        case HID_KEY_KEYPAD_9: keypad = "\x1bOy"; break;
        case HID_KEY_KEYPAD_SUBTRACT: keypad = "\x1bOm"; break;
        case HID_KEY_KEYPAD_COMMA: keypad = "\x1bOl"; break;
        case HID_KEY_KEYPAD_DECIMAL: keypad = "\x1bOn"; break;
        case HID_KEY_KEYPAD_ENTER: keypad = "\x1bOM"; break;
        default: break;
        }
        if (keypad != NULL) {
            uint8_t length = 0;
            /* The bound is tested first so the string is never indexed past
             * its terminator when it is longer than the buffer. */
            while (length < capacity && keypad[length] != '\0') {
                out[length] = (uint8_t)keypad[length];
                ++length;
            }
            return length;
        }
    }

    if (character != 0) {
        out[0] = character;
        return 1;
    }

    /* Anything left in VT52 mode - the editing keypad, Page Up/Down and so on -
     * did not exist on a VT52, so there is no code to send. */
    if (vt52_mode) return 0;

    /* Non-printing keys are sent as VT100 escape sequences. Arrows and
     * Home/End follow DECCKM: application mode sends SS3 (ESC O x) and normal
     * mode sends CSI (ESC [ x). */
    const char *sequence = NULL;
    switch (keycode) {
        case HID_KEY_ARROW_UP:
            sequence = cursor_keys_application ? "\x1bOA" : "\x1b[A"; break;
        case HID_KEY_ARROW_DOWN:
            sequence = cursor_keys_application ? "\x1bOB" : "\x1b[B"; break;
        case HID_KEY_ARROW_RIGHT:
            sequence = cursor_keys_application ? "\x1bOC" : "\x1b[C"; break;
        case HID_KEY_ARROW_LEFT:
            sequence = cursor_keys_application ? "\x1bOD" : "\x1b[D"; break;
        case HID_KEY_HOME:
            sequence = cursor_keys_application ? "\x1bOH" : "\x1b[H"; break;
        case HID_KEY_END:
            sequence = cursor_keys_application ? "\x1bOF" : "\x1b[F"; break;
        case HID_KEY_DELETE: sequence = "\x1b[3~"; break;
        case HID_KEY_PAGE_UP: sequence = "\x1b[5~"; break;
        case HID_KEY_PAGE_DOWN: sequence = "\x1b[6~"; break;
        case HID_KEY_INSERT: sequence = "\x1b[2~"; break;
        case HID_KEY_F1: sequence = "\x1bOP"; break;
        case HID_KEY_F2: sequence = "\x1bOQ"; break;
        case HID_KEY_F3: sequence = "\x1bOR"; break;
        case HID_KEY_F4: sequence = "\x1bOS"; break;
        default: break;
    }
    if (sequence == NULL) return 0;

    uint8_t length = 0;
    while (length < capacity && sequence[length] != '\0') {
        out[length] = (uint8_t)sequence[length];
        ++length;
    }
    return length;
}

static void process_keyboard_report(hid_keyboard_report_t const *report)
{
    static hid_keyboard_report_t previous = { 0 };

    for (uint8_t index = 0; index < 6; ++index) {
        uint8_t keycode = report->keycode[index];
        /* A key already present in the previous report is still held. */
        if (keycode == 0 || key_in_report(&previous, keycode)) continue;

        /* Ctrl+Alt+M opens the setup menu. CP/M and WordStar only ever see
         * single control codes, never a Ctrl+Alt chord, so this cannot collide
         * with an application command. The key is consumed either way so the
         * chord is never forwarded to the Z80. */
        bool ctrl_held = (report->modifier &
                          (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) != 0;
        bool alt_held = (report->modifier &
                         (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)) != 0;
        if (ctrl_held && alt_held && keycode == HID_KEY_M) {
            if (!menu_active) open_menu();
            continue;
        }

        if (menu_active) {
            menu_handle_key(keycode);
            continue;
        }

        uint8_t bytes[4];
        uint8_t count = key_to_bytes(keycode, report->modifier, bytes, sizeof(bytes));
        for (uint8_t i = 0; i < count; ++i) {
            uint8_t out = bytes[i];
            if (out == '\n') out = '\r';
            uart_putc(Z80_UART, (char)out);

            /* Preview locally only until the host starts echoing. */
            if (!uart_data_seen) {
                if (out == '\r') {
                    terminal_putc('\r');
                    terminal_putc('\n');
                } else {
                    terminal_putc(out);
                }
            }
        }
    }
    previous = *report;
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const *desc_report, uint16_t desc_len)
{
    (void)desc_report;
    (void)desc_len;
    keyboard_connected = true;
    usb_status_changed = true;
    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD) {
        if (!tuh_hid_receive_report(dev_addr, instance)) {
            hid_report_error = true;
            usb_status_changed = true;
        }
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    (void)dev_addr;
    (void)instance;
    keyboard_connected = false;
    usb_status_changed = true;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const *report, uint16_t len)
{
    (void)len;
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        queue_try_add(&keyboard_report_queue, report);
    }
    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD &&
        !tuh_hid_receive_report(dev_addr, instance)) {
        hid_report_error = true;
        usb_status_changed = true;
    }
}

static void core1_main(void)
{
    /* Flash writes are declared not to affect core 1 (see
     * PICO_FLASH_ASSUME_CORE1_SAFE in CMakeLists.txt), because all of this
     * code runs from RAM. Registering as a lockout victim is kept as a
     * fallback so core 0 could still pause us safely if that ever changed. */
    multicore_lockout_victim_init();

    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);
    while (true) {
        for (uint y = 0; y < FRAME_HEIGHT; ++y) {
            uint32_t *tmdsbuf;
            queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
            for (uint component = 0; component < 3; ++component) {
                tmds_encode_1bpp(
                    (const uint32_t *)&framebuf[y * FRAME_WIDTH / 8 + component * PLANE_SIZE_BYTES],
                    tmdsbuf + component * FRAME_WIDTH / DVI_SYMBOLS_PER_WORD,
                    FRAME_WIDTH);
            }
            queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
        }
    }
}

int main(void)
{
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

    setup_default_uart();
    uart_set_baudrate(Z80_UART, UART_BAUD);
    gpio_pull_up(UART_RX_PIN);
    uart_rx_init();

    pio_set_gpio_base(DVI_DEFAULT_SERIAL_CONFIG.pio, 16);
    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    memset(screen, ' ', sizeof(screen));
    memset(screen_attributes, 0, sizeof(screen_attributes));
    Paint_NewImage(framebuf, FRAME_WIDTH, FRAME_HEIGHT, 0, RGB111_WHITE);
    Paint_SetScale(3);
    display_ready = true;
    settings_load();
    queue_init(&keyboard_report_queue, sizeof(hid_keyboard_report_t), 16);
    multicore_launch_core1(core1_main);

    /* If the Z80 is already transmitting, skip the setup menu and go straight
     * to the VT100 emulation using the saved settings. */
    for (uint16_t attempt = 0; attempt < 250 && !uart_data_seen; ++attempt) {
        if (uart_rx_available()) {
            uart_data_seen = true;
            break;
        }
        sleep_ms(1);
    }

    if (uart_data_seen) {
        /* The Z80 is already talking: go straight to the terminal. This path
         * must not write flash, so power-up cannot stall the display. */
        begin_terminal_mode();
    } else {
        menu_level = MENU_LEVEL_TOP;
        menu_item = MENU_ITEM_FONT;
        /* Record what the menu opened with, so "Exit without saving" can put
         * it back. The runtime menu does this in open_menu(); the boot menu
         * never goes through open_menu(). */
        menu_snapshot_selections();
        draw_menu();
    }

    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pio_tx_num = 1;
    pio_cfg.pio_rx_num = 2;
    pio_cfg.tx_ch = 6;
    pio_cfg.sm_rx = 2;
    pio_cfg.sm_eop = 3;
    tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(1);

    while (true) {
        tuh_task();
        if (usb_status_changed) {
            usb_status_changed = false;
            /* Only the setup menu shows USB state; the live terminal keeps the
             * whole screen for output. */
            if (menu_active) draw_menu();
        }
        hid_keyboard_report_t report;
        while (queue_try_remove(&keyboard_report_queue, &report)) {
            process_keyboard_report(&report);
        }

        /* Apply every buffered byte, then redraw once. Redrawing per character
         * is far too slow at 115200 baud, and skipping bytes loses CR/LF - the
         * cause of a stray prompt and missing rows. */
        while (uart_rx_available()) {
            if (!uart_data_seen) {
                uart_data_seen = true;
                /* The Z80 has started talking, so leave the boot menu and show
                 * the terminal output even if Enter was never pressed.
                 *
                 * This uses begin_terminal_mode() rather than menu_exit() on
                 * purpose: the Z80 can start transmitting at any moment, and an
                 * automatic exit must never rewrite the stored settings. */
                if (menu_active && !menu_over_terminal) begin_terminal_mode();
            }
            terminal_putc(uart_rx_get());
        }
        if (full_redraw_pending) {
            full_redraw_pending = false;
            /* While the menu is open it owns the framebuffer; the pending
             * repaint is deferred until the menu closes. */
            if (!menu_active) draw_screen();
        }

        /* Keep the block cursor under the cursor position. This runs after the
         * output has been applied and any full repaint has happened, so it sees
         * the final cursor position for this pass however it came to move. */
        cursor_update();
        blink_tick();
    }
}