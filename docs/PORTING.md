# Porting MiSTer Monitor to new hardware

MiSTer Monitor supports multiple display targets. The MiSTer-side
server is hardware-agnostic — porting only touches the firmware
sketch.

## Pick a baseline

Two existing sketches; choose by **panel size**, not by MCU:

- **Small panels (~320x240)** → start from `mister_monitor_CYD28R/`.
  Full-panel UI: artwork on top, button footer at bottom, no
  decorative frame.
- **Large panels (5”+ / 800x480+)** → start from
  `mister_monitor_Tab5/`. Decorative full-screen frame bitmaps with
  the monitor centered inside a scaled sub-region, plus a side
  button column.

Both share the same 320x240 logical design grid for the monitor
pages, so most rendering code is identical between targets.

## Steps

1. Create `mister_monitor_<TARGET>/` at the repo root and copy the
   baseline (`.ino`, `AppConfig.h`, `mister_types.h`, plus
   `board_hal.{h,cpp}` from the CYD port). Rename the `.ino` to
   match the folder.
2. **Adapt `board_hal.{h,cpp}`** for your panel controller and
   touch chip. The CYD28R’s `board_hal` uses **LovyanGFX**, the
   library expected for new ports. It covers essentially every
   ESP32-family panel a porter will encounter (ILI9341, ST7789,
   ST7735, ILI9486, GC9A01, ST7262 RGB, etc.) over SPI or 8/16-bit
   parallel on ESP32 and ESP32-S3. The only realistic exception is
   ESP32-P4 MIPI-DSI panels — LovyanGFX support for that interface
   is still experimental. The Tab5 port handles this through
   M5Unified/M5GFX rather than LovyanGFX; other ESP32-P4 MIPI-DSI
   targets may need `esp_lcd` or Arduino_GFX directly.
3. **Set the panel constants** at the top of the `.ino`:
- `TARGET_WIDTH`, `TARGET_HEIGHT` — physical panel size.
- `IMAGE_AREA_HEIGHT` — vertical space reserved for artwork.
- `ScaledDisplay::SCALE_FACTOR`, `OFFSET_X`, `OFFSET_Y` — map
  the 320x240 logical design onto your panel. Use `1.0 / 0 / 0`
  for native 320x240; Tab5 uses `2.0` with
  `OFFSET_X=90, OFFSET_Y=200` for the 1280x720 panel.
4. Add a row to the Supported Hardware table below.

That’s the essence. Everything else — the `Board` facade, the
`ScaledDisplay` wrapper, monitor page rendering, server protocol,
JPEG handling, ScreenScraper integration — stays as-is.

## Supported hardware

|Target             |MCU          |Display              |Touch       |Baseline|Status |
|-------------------|-------------|---------------------|------------|--------|-------|
|M5Stack Tab5       |ESP32-P4 + C6|1280x720 IPS MIPI-DSI|I2C cap.    |—       |Stable |
|CYD ESP32-2432S028R|ESP32        |320x240 ILI9341      |XPT2046 res.|—       |Stable |
|8048S050C-I (5”)   |ESP32-S3     |800x480 ST7262 RGB   |GT911 cap.  |Tab5    |Planned|

## Build settings

### M5Stack Tab5

Arduino IDE → Tools:

- Board: `M5Tab5` (M5Stack board package)
- Library: `M5Unified`
- Other: defaults from the M5Tab board definition

### CYD ESP32-2432S028R

Arduino IDE → Tools:

- Board: `ESP32 Dev Module`
- Partition Scheme: `Huge APP (3MB No OTA/1MB SPIFFS)`
- Flash Size: `4MB (32Mb)`
- PSRAM: `Disabled`
- Upload Speed: `921600`
- Libraries: `LovyanGFX`, `XPT2046_Touchscreen`, `JPEGDEC`
