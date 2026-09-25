# RP2350 PiZero VT100 Terminal

A hardware VT100 terminal for the **Waveshare RP2350-PiZero**, with 640x480 DVI
output over HDMI and a USB keyboard, intended as a display and console for a
Z80-MBC2 (or any other machine with a serial port).

The terminal gives an 80x24 character area, which is what CP/M and WordStar
expect, plus extra space to the right of the text for startup and hardware
status messages.

A **prebuilt `vt100_terminal.uf2`** is included in `uf2/`, so you can flash the
board without installing any toolchain. Building from source is covered
[further down](#building-from-source).

---

## Contents

- [Hardware](#hardware)
- [Installation](#installation)
- [Using the terminal](#using-the-terminal)
- [Setup menu](#setup-menu)
- [Keyboard shortcuts](#keyboard-shortcuts)
- [Wiring](#wiring)
- [Building from source](#building-from-source)
- [Fonts](#fonts)
- [Escape code coverage](#escape-code-coverage)
- [Repository layout](#repository-layout)
- [Licences](#licences)

---

## Hardware

| Item | Notes |
| --- | --- |
| Waveshare RP2350-PiZero | The target board. Other RP2350 boards work if the DVI pin mapping is changed. |
| HDMI display | Any monitor or TV that accepts 640x480p at 60 Hz. |
| USB keyboard | Full-size or compact; a hub can be used to attach a keyboard and mouse. |
| Z80-MBC2 | Or any host with a 115200 baud serial port. |
| Level shifter | The Z80-MBC2 UART is 5 V. See [Wiring](#wiring). |

---

## Installation

You only need the UF2 file and a USB cable. No compiler, no SDK.

1. **Get the firmware.** Use `uf2/vt100_terminal.uf2` from this repository, or
   build it yourself (see [Building from source](#building-from-source)).

2. **Enter BOOTSEL mode.** Hold down the **BOOTSEL** button on the RP2350-PiZero
   while you plug the board into your computer with a USB cable. Release the
   button once the board is connected.

3. **Flash it.** A USB mass-storage drive named `RPI-RP2` appears.
   Copy `vt100_terminal.uf2` onto that drive.

   - **Windows / macOS / Linux (file manager):** drag the file onto the drive.
   - **Linux (command line):**
     ```bash
     cp vt100_terminal.uf2 /media/$USER/RPI-RP2/
     ```
   - **macOS (command line):**
     ```bash
     cp vt100_terminal.uf2 /Volumes/RPI-RP2/
     ```

4. **Done.** The drive dismounts and the board reboots into the terminal.

The firmware is stored in flash, so it survives power cycling. To update it,
just repeat the steps above.

> **Note:** flashing replaces the whole firmware. Your saved settings — font,
> colour, background, autowrap, newline and cursor — live in the last flash
> sector and are **erased by a reflash**. The terminal falls back to
> white-on-black with the DEC VT220 font after an update; set them again from
> the menu if you had changed them.

---

## Using the terminal

Power the board with the HDMI display connected. You should see either the
terminal screen or the setup menu, depending on whether the host is already
transmitting.

- If a **host is already sending data**, the board goes straight into VT100
  emulation using the saved settings. This is the normal case once the board is
  wired into a running system, and it means the Z80 BIOS startup output is never
  missed.
- If the **host is idle or absent**, the board has nothing to display, and the
  menu is the only way to change settings.

Type on the USB keyboard and the characters go to the host over UART. Anything
the host sends is rendered on screen.

---

## Setup menu

The terminal boots with the DEC VT220 font in white on black. The menu has these
entries:

| Entry | Values |
| --- | --- |
| `FONT` | DEC VT220, Modern, Terminal |
| `COLOUR` | Amber, Phosphor, White, Green, Cyan, Magenta, Red, Blue, Black |
| `BACKGROUND` | Black or White |
| `AUTOWRAP` | On or Off |
| `NEWLINE` | LF = CR+LF, or LF only |
| `CURSOR` | Shown or Hidden |
| `EXIT AND SAVE` | Store the selections and leave the menu |
| `EXIT WITHOUT SAVING` | Restore the selections that were live when the menu opened, and leave |

**Controls**

| Key | Action |
| --- | --- |
| Up / Down | Change the value |
| Right, or Enter | Open a submenu |
| Left | Back out |
| Enter | Accept the value |
| Esc | Leave the menu without saving |

### Leaving the menu

The two exits are menu entries, so that writing to flash is always something you
asked for and never a side effect of adjusting a value:

- **Exit and save** stores the selections. Power-cycle afterwards to confirm
  the change is permanent.
- **Exit without saving**, or **Esc**, puts back whatever was in effect when the
  menu opened, so opening the menu by accident costs nothing.

Changing a value never writes flash. A flash erase suspends interrupts for tens
of milliseconds, so doing one mid-typing could drop a burst of characters from
the host. Saving only at an explicit exit keeps it predictable.

Selections are written to the last flash sector and restored on the next
power-up. The first boot, or any power-up with invalid stored data, falls back
to white-on-black with the DEC VT220 font.

### Colours that would be invisible

The menu will not let you select a text colour that matches the background —
Black on Black, or White on White — because the terminal would become
invisible, including the menu itself. Up/Down simply skips past those
combinations, and a corrupted settings sector containing one falls back to the
defaults rather than starting invisible.

**Amber** is rendered as solid yellow and **Phosphor** as a dither between
green and yellow. The framebuffer is RGB111 (eight colours), so a true 75 %
green amber cannot be expressed; see the notes in [Fonts](#fonts) for why solid
yellow is used undithered.

---

## Keyboard shortcuts

| Shortcut | Action |
| --- | --- |
| **Ctrl+Alt+M** | Open or close the setup menu over a running session |
| **Esc** | Leave the menu without changing anything |

The menu is drawn straight to the framebuffer while the character buffer keeps
tracking live host output, so **nothing is lost** if the host prints while the
menu is open. Flash is only written on an explicit exit, so the picture stays
steady while you browse.

Changing the **font** returns you to a cleared screen, because the terminal
geometry changes and the old contents have no meaningful mapping. Colour,
background, autowrap and newline changes all preserve the screen.

CP/M and WordStar only ever see single control codes, never a Ctrl+Alt chord,
so the hotkey cannot collide with an application command. It is consumed either
way and never forwarded to the host.

---

## Wiring

### UART to the host

UART0 at **115200 baud, 8 data bits, no parity, 1 stop bit**:

| RP2350 PiZero | Signal | Z80-MBC2 |
| --- | --- | --- |
| GPIO 0 | RP2350 TX | Z80 RX |
| GPIO 1 | RP2350 RX | Z80 TX |
| GND | Signal ground | GND |

> **Level shifting is required.** The Z80-MBC2 UART is a 5 V interface and the
> RP2350 is 3.3 V. Add external level conversion in **both** directions.
> **Never apply 5 V to an RP2350 GPIO** — it will damage the chip.

### USB keyboard

Connect the keyboard to the dedicated PIO-USB Type-C connector **J2**:

| Pin | Signal |
| --- | --- |
| GPIO 28 | USB D+ |
| GPIO 29 | USB D− |
| J2 VBUS | Board VBUS rail |
| J2 GND | Board ground |

The 22 ohm series resistors and USB-C CC pulldowns are already on the board.

> **Important:** the PIO USB driver only configures the data pins. It does
> **not** generate or enable 5 V VBUS. The board VBUS rail must be powered, or
> use an externally powered USB hub.
>
> USB TX uses **DMA channel 6**, because DVI claims channels 0-5.

### HDMI

HDMI uses the existing `pico_sock_cfg` DVI pin mapping. No configuration is
needed for the Waveshare RP2350-PiZero.

If you are using a different board, override the pin mapping at configure time:

```bash
cmake -S . -B build -G Ninja -DPICO_SDK_PATH=/path/to/pico-sdk \
      -DDVI_DEFAULT_SERIAL_CONFIG=picodvi_dvi_cfg
```

Available mappings are listed in
`third_party/PicoDVI/include/common_dvi_pin_configs.h`.

---

## Building from source

### Prerequisites

1. **Raspberry Pi Pico SDK 2.x** — <https://github.com/raspberrypi/pico-sdk>
   Clone it, including submodules, and note the path:

   ```bash
   git clone --recurse-submodules https://github.com/raspberrypi/pico-sdk.git
   ```

2. **ARM GCC toolchain** — `arm-none-eabi-gcc`.
   - Windows: install the [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
   - Linux: `sudo apt install gcc-arm-none-eabi`
   - macOS: `brew install --cask gcc-arm-embedded`

3. **CMake** (3.13 or newer) and **Ninja**.

4. **Python 3 with Pillow** — only needed if you want to re-bake the fonts.

Everything else the firmware needs is vendored in `third_party/`, so no network
access is required at build time.

### Set `PICO_SDK_PATH`

Point it at your SDK checkout:

```bash
# Linux / macOS
export PICO_SDK_PATH=/path/to/pico-sdk
```

```powershell
# Windows PowerShell
$env:PICO_SDK_PATH = "C:\path\to\pico-sdk"
```

### Build

**Windows** — the wrapper finds CMake and Ninja on `PATH`:

```powershell
.\build.ps1
```

**Linux / macOS:**

```bash
./build.sh
```

Both scripts accept an explicit SDK path if the environment variable is not set:

```powershell
.\build.ps1 -PicoSdkPath C:\path\to\pico-sdk
```

```bash
./build.sh /path/to/pico-sdk
```

**Manually, without the wrapper:**

```bash
cmake -S . -B build -G Ninja -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build
```

The result is `build/vt100_terminal.uf2`. Flash it with the steps in
[Installation](#installation).

### Useful build options

| Option | Default | Meaning |
| --- | --- | --- |
| `PICO_SDK_PATH` | — | Path to the Pico SDK. Required. |
| `DVI_DEFAULT_SERIAL_CONFIG` | `pico_sock_cfg` | DVI pin mapping. |
| `PICO_BOARD` | `waveshare_rp2350_pizero` | Set in `CMakeLists.txt`. |

### Notes for maintainers

- `PICO_COPY_TO_RAM=1` places the whole image in RAM so that core 1's DVI
  scanout never fetches from flash. This is why
  `PICO_FLASH_ASSUME_CORE1_SAFE=1` is safe: it lets `flash_safe_execute()` skip
  the multicore lockout, so saving settings does not blank the display. **If
  you ever make code run from flash again, these two must be changed together**
  or scanout will be corrupted.
- `-Wall -Wextra` are enabled. Please keep the build warning-free.

### A note on the console

The terminal uses UART0 and the PIO-USB port directly, so the SDK's own stdio
drivers are deliberately disabled (`pico_enable_stdio_uart/usb(... 0)`). There
is no `printf` console; if you need one while debugging, enable it explicitly in
`CMakeLists.txt` and pick pins that do not collide with the ones above.

---

## Fonts

Three fonts are baked in and selectable from the setup menu:

| Menu entry | Source | Type |
| --- | --- | --- |
| DEC VT220 | `tools/fonts/DEC-VT220-10x20.bdf` | native bitmap |
| Modern | Consolas | outline (TTF) |
| Terminal | Cascadia Mono | outline (TTF) |

Every face is 8 pixels wide, so exactly 80 columns fill the 640-pixel DVI width.

The **DEC VT220** face is the authentic one: native 10x20 bitmap glyphs dumped
from 1986 VT220 ROM firmware (DEC part `23-178E6`). Its ink is exactly 8 pixels
wide, so it maps onto the terminal cell with no scaling at all and gives a
classic **80x24** screen. See `tools/fonts/DEC-Fonts-NOTICE.md` for provenance —
the glyph designs are historical DEC material.

Each font also carries the two alternate character sets a VT100 could select:

- **DEC Special Graphics** (`ESC ( 0`, enabled with `SO`) — the line drawing set
  CP/M software uses to draw boxes and rules. All 31 glyphs are baked, including
  every box corner, tee and crossing, the scan lines, and the symbols for
  tab/return/newline.
- **UK** (`ESC ( A`) — identical to ASCII except that `#` displays as `£`.

The character set is resolved and **latched into the cell when the character is
written**, not when it is drawn, so a program that switches to the line drawing
set, draws a box, and switches back will not have its earlier text restyle
itself.

### Re-baking the fonts

1. Edit `tools/fonts.json`. Each family needs an `id`, `name`, `type` (`bdf` or
   `ttf`), `path`, and `sizes`. Relative paths resolve from the repository root.
   The TTF entries use `${FONT_DIR}` and `${FONT_SUFFIX}` so the same file works
   on Windows and Linux; override them with the `FONT_DIR` / `FONT_SUFFIX`
   environment variables if your fonts live elsewhere.
2. Run `bake-fonts.bat` (Windows) or `./bake-fonts.sh` (Linux / macOS). This
   bakes the tables into `src/fonts_vt100.c` and regenerates
   `src/fonts_vt100.h`.
3. Rebuild.

Useful check — decode a baked table back to ASCII art, showing exactly what the
firmware will display:

```bash
python tools/show_baked.py src/fonts_vt100.c FontVt22020 "AWgj@ A>"
```

`tools/font_preview.py` compares candidate TTF faces at each cell size before
you commit to one.

> **Bitmap and outline faces need opposite treatment,** which the converter
> handles automatically. BDF glyphs are copied one-to-one with no interpolation,
> because resampling a bitmap face destroys the very thing that makes it
> authentic. TTF glyphs are fitted to the cell in both axes and rendered at 4x
> before being box-filtered down, because rasterising straight to 8xN aliases
> badly.

---

## Escape code coverage

The implementation is checked against the classic *VT100 escape codes*
reference. The following are supported:

| Group | Codes |
| --- | --- |
| Cursor movement | CUU/CUD/CUF/CUB, CUP/HVP, CHA, VPA |
| Scrolling and lines | IND, RI, NEL, DECSTBM |
| Save/restore | DECSC/DECRC, including attributes |
| Tabs | HTS, TBC (`0g`, `3g`) |
| Erase | EL `0`/`1`/`2`, ED `0`/`1`/`2` |
| Attributes | SGR `0` `1` `2` `4` `5` `7` `8`, resets `22` `24` `25` `27` `28` |
| Modes | DECOM, DECAWM, DECSCNM, LMN (`?20h`/`?20l`), DECTCEM (`?25h`/`?25l`) |
| Charsets | `ESC ( x` / `ESC ) x`, SO/SI, UK, DEC Special Graphics |
| Keyboard output | DECCKM, DECKPAM/DECKPNM, keypad codes |
| Host queries | DSR `5n`/`6n`, DA (`ESC [ c`), DECID (`ESC Z`) |
| Reset / test | RIS, DECALN (`ESC # 8`) |
| VT52 mode | the full VT52 set, entered with `CSI ?2l`, left with `ESC <` |

### Attribute rendering

The framebuffer is RGB111, so SGR attributes are realised as best that palette
allows rather than being ignored:

- **Reverse** (7) and **reverse screen** (DECSCNM) both swap foreground and
  background, and compose with each other the way hardware does.
- **Bold** (1) adds stroke weight by lighting the pixel to the left of every set
  pixel. RGB111 has no half-intensity to raise, so extra weight is the only way
  to show emphasis; it is smeared leftwards so it can never spill into the
  neighbouring cell.
- **Underline** (4) draws a rule along the bottom glyph row.
- **Invisible** (8) paints nothing while still occupying the cell.
- **Dim** (2) drops the dither and bold treatment.
- **Blink** (5) repaints the screen at roughly 1.6 Hz. The timer only starts
  once something has actually used it, so an idle terminal never repaints
  needlessly.

### Deliberately ignored

A few codes describe hardware this terminal genuinely does not have. They are
accepted and discarded rather than treated as errors, which is what real VT100
firmware does with modes it understands but cannot act on:

- `DECCOLM` (`?3h`/`?3l`) — 132-column mode. A cell is 8 pixels, so only 80
  columns fit in the 640-pixel frame.
- `DECSCLM` smooth scrolling, `DECINLM` interlace, `DECARM` auto-repeat.
- `DECLL` keyboard LEDs, `DECTST` confidence tests.
- `DECDHL`/`DECWL`/`DECSWL` (`ESC # 3`–`ESC # 6`) — double width/height lines,
  impossible with a fixed 8-pixel bitmap cell.

SGR colour codes (`30`–`37`, `40`–`47`, `39`, `49`) are parsed and ignored: the
colour scheme is a deliberate user choice from the setup menu, not something a
host program should override.

### Cursor

The terminal shows a blinking block cursor at the cursor position, which can be
turned off with the `CURSOR` menu entry.

The cursor is drawn as an **overlay**, not written into the character buffer.
The character underneath is therefore never modified, so the cursor can blink,
move or be switched off without anything needing to be saved and restored. When
it moves, only the one cell it left is repainted. Like a real VT100, the cursor
cell is shown in inverse video, so a blank cell becomes a solid block.

Two independent things can hide the cursor:

- The `CURSOR` menu setting, which is a user preference saved to flash.
- `DECTCEM` (`ESC[?25h` / `ESC[?25l`), which is the host's request. CP/M software
  hides the cursor while it redraws the screen, and honouring that keeps the
  display tidy. `RIS` (`ESC c`) restores it to visible.

The host's request outranks the menu setting, so a program that hides and shows
the cursor keeps working even with the menu option set to `Shown`.

**The cursor blinks while output is being printed, and that is correct.** The
VT100 User Guide specifies the cursor as a *blinking* block driven by the video
processor's own free-running counter. It is not restarted or suspended when
characters arrive, so on real hardware the block keeps blinking through a screen
full of BIOS output. `CURSOR: Hidden` in the menu turns it off if the effect is
distracting.

### VT52 compatibility mode

A VT52 is a different protocol rather than a dialect of ANSI, so it gets its own
parser. Enter it with `CSI ?2l` (`DECANM` reset) and leave it with `ESC <`. The
complete VT52 set is supported: `ESC A`/`B`/`C`/`D` cursor movement, `ESC F`/`G`
graphics mode on/off, `ESC H` home, `ESC I` reverse line feed, `ESC J`/`K` erase
to end of screen/line, `ESC Y` direct cursor addressing, `ESC Z` identify,
`ESC =`/`>` alternate keypad mode on/off, and `ESC <` back to ANSI.

Two details are easy to get wrong and are handled deliberately:

- **`ESC Y` coordinates** arrive as position + 0x1F, so `' '` is position 1.
  Out-of-range values are clamped to the margin rather than wrapping round to
  the opposite edge of the screen.
- **`ESC F` graphics mode** uses the same DEC line-drawing set the VT100 reaches
  through `ESC ( 0`, so it reuses the same baked glyphs.

While in VT52 mode, `ESC [` has no special meaning, and the ANSI parser is kept
dormant so it cannot mistake VT52 bytes for a sequence it should assemble. The
control characters the two protocols share (CR, LF, BS, TAB, SO, SI) are handled
by one shared routine, so wrapping and scrolling behave identically in both
modes.

VT52 mode also changes **what the keyboard sends**: the cursor keys always send
`ESC A`/`B`/`C`/`D` (DEC documents `DECCKM` as only effective when `DECANM` is
set to ANSI), the auxiliary keypad sends the `ESC ? x` set in application mode,
PF1-PF4 send `ESC P`–`ESC S`, and keys that did not exist on a VT52 — the
editing keypad, Page Up/Down — send nothing rather than a made-up sequence.

**Leaving VT52 mode.** The documented way out is `ESC <`, but a host that
switches to VT52 and then dies without sending it can leave the screen showing
raw parameters (`HJ0m`, `01;01H`) because `ESC [` is no longer CSI. Two software
ways back are provided, so this never needs a power cycle:

- `ESC c` (`RIS`) restores the power-on state, which is ANSI mode.
- Opening the setup menu (**Ctrl+Alt+M**) always returns to ANSI mode.

Note that **Ctrl+C does not leave VT52 mode**. `0x03` is a host-level signal
(ETX), not a terminal command, so it cannot recover the terminal on its own.

---

## Repository layout

```
.
├── CMakeLists.txt              Build definition for the firmware
├── build.ps1 / build.sh        Portable build wrappers
├── bake-fonts.bat / .sh        Re-bake the font tables
├── boards/
│   └── waveshare_rp2350_pizero.h   Board definition
├── src/
│   ├── main.c                  Terminal, parser, menu, keyboard
│   ├── fonts_vt100.c/.h        Baked font tables (generated)
│   └── usb_config.h            TinyUSB host configuration
├── tools/
│   ├── gen_fonts.py            Bakes fonts from tools/fonts.json
│   ├── show_baked.py           Decodes a baked table to ASCII art
│   ├── font_preview.py         Compares candidate outline faces
│   └── fonts/                  DEC VT220 BDF and its licence notice
├── third_party/
│   ├── PicoDVI/                DVI output library (BSD-3, Luke Wren)
│   └── Pico-PIO-USB/           USB host library (MIT, sekigon-gonnoc)
├── uf2/
│   └── vt100_terminal.uf2      Prebuilt firmware
└── THIRD-PARTY-NOTICES.md      Provenance and licence boundaries
```

---

## Licences

- **This project** — MIT. See `LICENSE`.
- **PicoDVI** — BSD 3-Clause, Copyright (c) 2021 Luke Wren.
- **Pico-PIO-USB** — MIT, Copyright (c) 2021 sekigon-gonnoc.
- **Pico SDK / TinyUSB** — BSD 3-Clause / MIT, supplied by the user at build time.
- **DEC VT220 font data** — historical DEC material; the MIT grant does not
  relicense the glyph designs. Read `tools/fonts/DEC-Fonts-NOTICE.md`.

See `THIRD-PARTY-NOTICES.md` for the full breakdown before redistributing.
