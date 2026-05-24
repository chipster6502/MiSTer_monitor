# Porting MiSTer Monitor to new hardware

This project is planned to support multiple display targets. Each target lives in
its own sketch folder at the repo root (e.g. `mister_monitor_Tab5/`,
`mister_monitor_CYD/`). The server-side (MiSTer) code and the
`config.ini` template are shared across all targets.

## Choosing a baseline

The current ports differ in panel size and UI strategy, and that
choice — not the ESP32 variant — should drive which sketch you copy
when starting a new port.

**Use the CYD sketch as baseline when the target panel is around
320x240 native.** The UI fills the whole panel: artwork area on top,
button band as a footer at the bottom, no decorative bezel. The
`ScaledDisplay` wrapper runs 1:1 (`SCALE_FACTOR=1`, no offset). This
is the right choice for CYD-class boards (ESP32-2432S028R variants),
M5Stack Core Basic-style 320x240 panels, and similar low-resolution
targets.

**Use the Tab5 sketch as baseline when the target panel is
significantly larger than 320x240** (e.g. 5"/800x480, 7"/1024x600,
9"-10"/1280x800). The UI is composed of full-screen decorative
frame bitmaps with a central monitor sub-region (rendered
via the scaled `Lcd`) and a side column of touch buttons. The
`ScaledDisplay` wrapper applies a per-port `SCALE_FACTOR` plus
`OFFSET_X/OFFSET_Y` to position the logical 320x240 design inside
the panel. This is the right choice for ESP32-S3 RGB-parallel boards
(e.g. the 8048S050C-I 5" 800x480 with ST7262), ESP32-P4 MIPI-DSI
panels, and dual-MCU boards that pair an ESP32-P4 with an ESP32-C6
radio (the same pattern the M5Tab5 itself uses).

Both sketches share the same logical 320x240 design grid for the
monitor pages, so most of the page-rendering code is identical
between them. What differs is the surrounding chrome (frame
bitmaps yes/no), the button layout, and the scaling parameters.

## Adding a new port

1. Create a new folder `mister_monitor_<TARGET>/` at the repo root.
2. Copy the chosen baseline:
   - **From CYD**: `mister_monitor_CYD/{*.ino, AppConfig.h,
     mister_types.h, board_hal.h, board_hal.cpp}`. Rework
     `board_hal.{h,cpp}` for your panel driver, touch controller,
     and pinout. The existing file is a self-contained example:
     `LGFX_CYD` for the panel, bit-banged `xpt2046_*` for touch,
     and `BoardClass` (with nested `BoardTouch`, `BoardSpeaker`,
     `BoardConfig`) for the lifecycle facade.
   - **From Tab5**: `mister_monitor_Tab5/{*.ino, AppConfig.h,
     mister_types.h}`. If your board is **not** in the M5Unified
     ecosystem, also create a `board_hal.{h,cpp}` following the
     same `BoardClass` pattern as the CYD's, so the sketch's
     `Board.*` calls can be redirected to your panel/touch backend.
3. Rename the `.ino` to match the folder name.
4. Adjust the panel-related constants at the top of the sketch:
   - `TARGET_WIDTH`, `TARGET_HEIGHT`: physical panel size.
   - `IMAGE_AREA_HEIGHT`: vertical room reserved for artwork, leaving
     space for the footer / button column you use.
   - `ScaledDisplay::SCALE_FACTOR`, `OFFSET_X`, `OFFSET_Y`: map the
     logical 320x240 design onto your panel. For native 320x240,
     use `1.0 / 0 / 0`. For an 800x480 Tab5-style layout, a 2x
     scale (640x480) with `OFFSET_X=80, OFFSET_Y=0` is one
     reasonable starting point.
5. Keep two API surfaces compatible so the rest of the sketch
   compiles unchanged:
   - The **`Board` facade** (`BoardClass` in the CYD HAL, `M5` in
     the Tab5 via M5Unified): must expose `Board.begin(cfg)`,
     `Board.update()`, `Board.config()`, `Board.Display.*`,
     `Board.Touch.getDetail()` returning a `TouchDetail` with
     `wasPressed()`, `x`, `y`, and `Board.Speaker.*` (a no-op stub
     is fine if the board has no speaker).
   - The **`Lcd` (`ScaledDisplay`) drawing wrapper**: must expose
     `fillRect`, `drawRect`, `fillScreen`, `drawFastHLine`,
     `drawFastVLine`, `drawLine`, `fillCircle`, `drawCircle`,
     `setCursor`, `setTextColor`, `setTextSize`, `setTextWrap`,
     `print` / `println` / `printf`, and `pushImage`.
6. Re-lay out the touch buttons (`btnPrev`, `btnScan`, `btnNext`,
   plus any rescan/footer buttons) and any per-screen
   `PHYS_X/PHYS_Y` macros in physical panel coordinates for your
   panel.
7. If your baseline is Tab5, replace the decorative frame bitmaps
   (`frame02.jpg` and friends on the SD card) with versions sized to
   your panel if necessary. If your baseline is CYD, there are no decorative
   frames to replace.
8. Add a row to the Supported Hardware table below.
9. The MiSTer-side server is hardware-agnostic — no server changes
   should be needed for a pure firmware port.

## Hardware abstraction notes

### Two layers of abstraction

The codebase isolates hardware behind two cooperating objects:

- **`Board` facade** — abstracts the panel driver, touch controller,
  speaker, and lifecycle (`begin`, `update`, `config`). The Tab5
  binds it directly to `M5Unified` (the global `M5` object). The
  CYD implements the same interface through its own `BoardClass` in
  `board_hal.h`, backed by a `LovyanGFX` panel instance and a
  bit-banged XPT2046 reader. New non-M5Unified ports should follow
  the `BoardClass` pattern.
- **`Lcd` (`ScaledDisplay` wrapper)** — translates the *logical*
  drawing primitives used by the sketch (text, rectangles, lines,
  circles) into the *physical* coordinate space of the panel via
  `SCALE_FACTOR` + `OFFSET_X/OFFSET_Y`.

### Coordinate systems are not uniform across the sketch

The logical 320x240 design grid only applies to the **core monitor
pages** that go through the `Lcd` wrapper (text labels, panel
borders, status indicators). Other parts of the sketch are drawn in
**physical panel coordinates** and must be rewritten per-target:

- **Touch hitboxes / buttons** are defined directly in physical
  panel coordinates (e.g. `btnPrev` is at `(950, 135)` on the Tab5
  1280x720 panel and at `(0, 205)` on the CYD 320x240 panel). Touch
  input is *not* re-projected into the logical 320x240 space before
  hit testing — `touch.x/y` is compared against the button rect
  as-is. When porting, re-lay out the buttons for your panel.
- **`Lcd.pushImage()` is intentionally a pass-through** (no scaling).
  JPEG artwork is fetched from ScreenScraper sized to the panel's
  physical resolution and pushed directly. The image area dimensions
  (`TARGET_WIDTH`, `IMAGE_AREA_HEIGHT`) are physical, not logical.
- **The Tab5 mapping is not a simple uniform scale.** Its
  `ScaledDisplay` applies `SCALE_FACTOR=2.0` plus a constant offset
  `(OFFSET_X=90, OFFSET_Y=200)`, mapping the 320x240 logical area
  onto a 640x480 sub-region of the 1280x720 panel. The surrounding
  area is filled with full-screen decorative cyberpunk frame
  bitmaps drawn directly through `M5.Display.pushImage()`.

### Memory and JPEG decoding

Both current targets load each JPEG fully into a `malloc`'d buffer
and decode via `JPEGDEC::openRAM()`. There is no streaming-from-SD
decode path. For low-RAM targets the practical strategies are:

- Cap `max_image_size` in `config.ini` to fit comfortably within the
  available heap (account for WiFi / TLS / JSON buffers running
  concurrently). In practice, ScreenScraper media sized for a
  320x240 panel comes in well under 100 KB.
- Pick ScreenScraper media types whose native dimensions roughly
  match the panel (smaller display ⇒ smaller JPEGs ⇒ smaller
  buffers). The per-context priority lists in `config.ini`
  (`game_media_order`, `arcade_media_order`, etc.) are the lever
  for this — there is no need to change C++ code.
- On ESP32-P4 (Tab5), do **not** allocate large buffers with
  `heap_caps_malloc(MALLOC_CAP_SPIRAM)`:  Use plain `malloc()` and
  rely on the ~510 KB of internal heap free at boot. The wrapper
  `psramMalloc()` in the Tab5 sketch documents this in its header comment.

## Supported Hardware

| Target | MCU | Display | Touch | PSRAM | Baseline | Status |
|---|---|---|---|---|---|---|
| M5Stack Tab5 | ESP32-P4 + C6 | 1280x720 IPS MIPI-DSI | I2C cap. | yes (unused, see notes) | — | Stable |
| CYD ESP32-2432S028R | ESP32 | 320x240 ILI9341 | XPT2046 res. | no | — | Stable |
| 8048S050C-I (5") | ESP32-S3 | 800x480 ST7262 RGB | GT911 cap. | yes | Tab5 | Planned |

## Build settings per target

### M5Stack Tab5

Arduino IDE → Tools menu:

- Board: `M5Tab5` (from the M5Stack board package)
- Library: `M5Unified`
- Other settings: defaults from the M5Tab board definition

### CYD ESP32-2432S028R

Arduino IDE → Tools menu:

- Board: `ESP32 Dev Module`
- Partition Scheme: `Huge APP (3MB No OTA/1MB SPIFFS)`
- Flash Size: `4MB (32Mb)`
- PSRAM: `Disabled`
- Upload Speed: `921600`
- Libraries: `LovyanGFX`, `XPT2046_Touchscreen`, `JPEGDEC`,
  `ArduinoJson`
