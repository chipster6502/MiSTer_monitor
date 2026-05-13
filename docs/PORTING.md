# Porting MiSTer Monitor to new hardware

This project supports multiple display targets. Each target lives in
its own sketch folder at the repo root (e.g. `mister_monitor_Tab5/`,
`mister_monitor_CYD/`). The server-side (MiSTer) code and the
`config.ini` template are shared across all targets.

## Adding a new port

1. Create a new folder `mister_monitor_<TARGET>/` at the repo root.
2. Copy `mister_monitor_Tab5/{*.ino, AppConfig.h, mister_types.h}`
   into it as a starting baseline. Tab5 is currently the reference
   port.
3. Rename the `.ino` to match the folder name.
4. Replace the hardware bootstrap (display init, touch init, SD init)
   with code for your target. Keep the `Lcd` drawing object API
   compatible so the rest of the sketch works unchanged.
5. Add a row to the Supported Hardware table below.
6. The MiSTer-side server is hardware-agnostic — no server changes
   should be needed for a pure firmware port.

## Hardware abstraction notes

- All drawing is in **logical 320x240 coordinates**. The Tab5 port
  uses a `ScaledDisplay` wrapper to scale 2x onto its 1280x720 panel.
  Native 320x240 displays (CYD, M5Core Basic) use the LCD library
  directly without scaling.
- Touch input must be translated into the 320x240 logical space
  before checking UI hitboxes.
- Targets without PSRAM must use streaming JPEG decode rather than
  loading full files into RAM. See the CYD port for the reference
  low-memory implementation.

## Supported Hardware

| Target | MCU | Display | Touch | PSRAM | Status |
|---|---|---|---|---|---|
| M5Stack Tab5 | ESP32-P4 | 1280x720 IPS | I2C cap. | yes | Stable |
| CYD ESP32-2432S028R | ESP32 | 320x240 ILI9341 | XPT2046 res. | no | WIP |
