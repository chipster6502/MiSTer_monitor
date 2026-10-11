# Porting MiSTer Monitor to new hardware

MiSTer Monitor supports multiple display targets. The MiSTer-side
server is hardware-agnostic — porting only touches the firmware
sketch.

## Contents

- [Pick a baseline](#pick-a-baseline)
- [Steps](#steps)
- [Supported hardware](#supported-hardware)
- [Build settings](#build-settings)

## Pick a baseline

Two existing sketches; choose by **panel size**, not by MCU:

- **Small to mid-size SPI panels (320x240, 480x320)** → start from
  `mister_monitor_CYD28R_ILI9341/`. Full-panel UI: artwork on top, button
  footer at bottom, no decorative frame. The 3.5" 480x320 ports
  (CYD35C / CYD35R) use this baseline.
- **Large panels (5”+ / 800x480+)** → start from
  `mister_monitor_Tab5/`. Decorative full-screen frame bitmaps with
  the monitor centered inside a scaled sub-region, plus a side
  button column.

Both share the same 320x240 logical design grid for the monitor
pages, so most rendering code is identical between targets.

## Steps

1. Create `mister_monitor_<TARGET>/` at the repo root and copy the whole
   baseline folder into it: besides the `.ino`, `AppConfig.h` and
   `mister_types.h`, the sketch needs the image decoders
   (`JpegImage`, `PngImage`), the web, update and weather modules
   (`WebConfig.h`, `WebFiles.h`, `FirmwareUpdate.h`, `Weather.h`) and
   `build_opt.h`. Starting from the Tab5, take `board_hal.{h,cpp}` from
   the CYD port as the model for your own. Rename the `.ino` to match the
   folder.
2. **Adapt `board_hal.{h,cpp}`** for your panel controller and
   touch chip. The CYD28R_ILI9341’s `board_hal` uses **LovyanGFX**, the
   library expected for new ports. It covers essentially every
   ESP32-family panel a porter will encounter (ILI9341, ST7789,
   ST7796, ST7735, ILI9486, GC9A01, ST7262 RGB, etc.) over SPI or
   8/16-bit parallel on ESP32 and ESP32-S3. The only realistic
   exception is ESP32-P4 MIPI-DSI panels — LovyanGFX support for
   that interface is still experimental. The Tab5 port handles this
   through M5Unified/M5GFX rather than LovyanGFX; other ESP32-P4
   MIPI-DSI targets may need `esp_lcd` or Arduino_GFX directly, as the
   Guition port does with its vendor `esp_lcd` driver.
3. **Set the panel constants** at the top of the `.ino`:
- `TARGET_WIDTH`, `TARGET_HEIGHT` — physical panel size.
- `IMAGE_AREA_HEIGHT` — vertical space reserved for artwork.
- `ScaledDisplay::SCALE_FACTOR`, `OFFSET_X`, `OFFSET_Y` — map
  the 320x240 logical design onto your panel. Use `1.0 / 0 / 0`
  for a natively-sized panel (320x240 or 480x320); Tab5 uses `2.0`
  with `OFFSET_X=90, OFFSET_Y=200` for the 1280x720 panel.
4. **Keep over-the-air updates working.** Build with a partition
   scheme that has two app slots: an update is written to the slot that
   is not running, so a single-slot layout cannot take one. Set
   `FIRMWARE_BOARD_ID` in the `.ino` to a key of its own; the release
   workflow publishes the board's update image as `<key>.bin` and lists
   it under that key in `firmware/manifest.json`.
5. **Add the board to `.github/workflows/release.yml`**: the credentials
   loop, a compile step with its FQBN, the flasher image, the OTA image,
   and a flasher manifest, plus its button on `docs/flasher/index.html`.
   Give it a folder under `SD_card_content/` with its `config.ini` and
   assets.
6. Add a row to the Supported Hardware table below.

That’s the essence. Everything else — the `Board` facade, the
`ScaledDisplay` wrapper, monitor page rendering, server protocol,
JPEG handling, ScreenScraper integration — stays as-is.

## Supported hardware

| Target | MCU | Display | Touch | Baseline | Status |
|---|---|---|---|---|---|
| M5Stack Tab5 | ESP32-P4 + C6 | 1280x720 IPS MIPI-DSI | I2C cap. | — | Stable |
| CYD 2.8" 1-USB (ESP32-2432S028) | ESP32 | 320x240 ILI9341 | XPT2046 res. | — | Stable |
| CYD 2.8" ST7789 (ESP32-2432S028) | ESP32 | 320x240 ST7789 | XPT2046 res. | CYD28R_ILI9341 | Stable |
| CYD 3.5" cap. (ESP32-3248S035) | ESP32 | 480x320 ST7796 | GT911 cap. | CYD28R_ILI9341 | Stable |
| CYD 3.5" res. (ESP32-3248S035) | ESP32 | 480x320 ST7796 | XPT2046 res. | CYD28R_ILI9341 | Stable |
| 8048S050C-I (5") | ESP32-S3 | 800x480 ST7262 RGB | GT911 cap. | Tab5 | Planned |
| Guition JC8012P4A1C (10.1") | ESP32-P4 + C6 | 800x1280 IPS MIPI-DSI | GSL3680 cap. | Tab5 | Stable |
| Waveshare ESP32-S3-Touch-LCD-2.8 V2 | ESP32-S3 | 320x240 ST7789 | CST3530 cap. | CYD28R_ST7789 | Beta (touch untested) |

## Build settings

### M5Stack Tab5

Arduino IDE → Tools:

- Board: `M5Tab5` (M5Stack board package)
- Libraries: `M5Unified`, `M5GFX`, `JPEGDEC`, `PNGdec`
- Other: defaults from the M5Tab board definition

The release workflow builds it with the M5Stack package 3.3.7 and this FQBN:

```
m5stack:esp32:m5stack_tab5
```

### Guition JC8012P4A1C (10.1")

Arduino IDE -> Tools, per the vendor's documented configuration plus the
Chip Variant the v3 units need:

- Board: `ESP32P4 Dev Module` (esp32 board package)
- Chip Variant: `Before v3.00`, or `v3.00 or newer` for the units with the
  newer ESP32-P4 (sticker SKU ending in `-V3`; esptool reports
  `revision v3.x`). An image built for one revision restarts in a loop on
  the other, so each gets its own build
- PSRAM: `Enabled` (required - the framebuffer alone is ~2 MB)
- Flash Mode: `QIO`
- Flash Frequency: `80MHz`
- Flash Size: `16MB (128Mb)`
- Partition Scheme: `16M Flash (3MB APP/9.9MB FATFS)`
- USB CDC On Boot: `Disabled` - with it enabled, `Serial` output goes to the
  native USB port instead of the UART bridge used for flashing, so the COM
  port you flash from shows only the ROM boot log
- Libraries: `M5GFX`, `JPEGDEC`, `PNGdec`

The release workflow builds it with esp32 core 3.3.12. The vendor
screenshot also shows a CPU Frequency of 360MHz; the core has no such menu
for the P4, and Chip Variant sets it instead: 360 MHz before v3.00,
400 MHz from v3.00. The sketch tells its two builds apart by that clock
(`FIRMWARE_BOARD_ID` is `guition10inch` or `guition10inch_v3`), so each
board is offered its own image over the air.

As arduino-cli FQBNs, the ones the release workflow uses:

```
esp32:esp32:esp32p4:ChipVariant=prev3,PSRAM=enabled,FlashMode=qio,FlashFreq=80,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB
esp32:esp32:esp32p4:ChipVariant=postv3,PSRAM=enabled,FlashMode=qio,FlashFreq=80,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB
```

The vendor `esp_lcd` JD9365 driver lives in the sketch's `src/lcd/` folder and
is used unmodified - Arduino compiles `src/` recursively, so it is picked up
automatically.

### CYD (all variants)

The same settings apply to every CYD variant (2.8" 1-USB and 2-USB, 3.5"
capacitive and resistive); only the sketch folder differs.

Arduino IDE → Tools:

- Board: `ESP32 Dev Module`
- Partition Scheme: `Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)` —
  two app slots, which the over-the-air updater needs. Boards flashed with
  `Huge APP` before 2.12 have a single slot and need one last update by
  cable
- Flash Size: `4MB (32Mb)`
- PSRAM: `Disabled`
- Upload Speed: `921600`
- Libraries: `LovyanGFX`, `XPT2046_Touchscreen`, `JPEGDEC`, `PNGdec`

As an arduino-cli FQBN, the one the release workflow uses (esp32 core
3.3.12):

```
esp32:esp32:esp32:PartitionScheme=min_spiffs,FlashSize=4M,PSRAM=disabled
```

### Waveshare ESP32-S3-Touch-LCD-2.8 V2

Port contributed by Rick van Soest, from the CYD28R_ST7789 sketch: same panel
controller and resolution, so `board_hal.h` is the only new file (LovyanGFX
ST7789 on SPI2, CST3530 capacitive touch on `Wire1`, and GPIO7 driven high so
the board does not switch itself off on battery). The sketch differs from the
ST7789 one in the SD and backlight pins and in three additions: `image_mode=cycle`,
`art_inset_left` and the menu-time idle clock.

Identify the V2 by its CST3530 at I2C address 0x58 on SDA=1/SCL=3. The V1, the
non-touch ESP32-S3-LCD-2.8 and the -2.8B (ST7701, 480x640 RGB) are different
boards. The panel has no MISO line, so `/screenshot.bmp` comes out black. The
touch mapping and the RetroAchievements row calibration were derived from the
panel geometry and have not been tapped on hardware yet, hence *Beta*.

Arduino IDE → Tools (esp32 board package, verified on core 3.3.12):

- Board: `ESP32S3 Dev Module`
- USB CDC On Boot: `Enabled` (the board only has the native USB port)
- PSRAM: `OPI PSRAM`
- Flash Size: `16MB (128Mb)`
- Partition Scheme: `16M Flash (3MB APP/9.9MB FATFS)` — two app slots, which the
  over-the-air updater needs
- Libraries: `LovyanGFX`, `JPEGDEC`, `PNGdec` (no `XPT2046_Touchscreen`)

As an arduino-cli FQBN, the one the release workflow uses:

```
esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB
```
