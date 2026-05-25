# MiSTer FPGA Monitor

A status monitor for the MiSTer FPGA platform. Displays the currently loaded
game artwork, system information, storage status, and network details in real time.

## Demo videos

[![Boot, game loading and artwork display](https://img.youtube.com/vi/2N9_1_c8_N4/maxresdefault.jpg)](https://youtu.be/2N9_1_c8_N4)

*Boot interface, loading an arcade game from the on-screen menu, and loading
console and computer games via the MiSTer Remote web application.*

[![Real-time system statistics](https://img.youtube.com/vi/x4C5lWDXwtM/maxresdefault.jpg)](https://youtu.be/x4C5lWDXwtM)

*Navigating through the real-time system statistics screens.*

## Features

- Real-time game and core artwork display via ScreenScraper API
- Automatic game and system detection from OSD, MiSTer Remote web app and Super Attract Mode (SAM)
- Reliable game load detection using nanosecond-precision filesystem timestamps when loading cores and games through the on-screen menu (OSD)
- Automatic Arcade subsystem detection
- Manual SCAN game button on the image screen for cases where the CRC could not be detected automatically
- System monitor (CPU, memory, uptime, storage, network, and connected USB devices panels)
- Touch-based navigation
- Screenshot capture of the display, downloadable over the local network via HTTP

## Supported Hardware

| Target | Status |
|---|---|
| M5Stack Tab5 (ESP32-P4) | Stable — reference implementation |
| Cheap Yellow Display (ESP32-2432S028R) | Stable |

See `docs/PORTING.md` for porting guidelines.

## Requirements

**Hardware**
- Supported ESP32-based display
- MiSTer FPGA with network connectivity
- microSD card for the display (image storage)

**Software**
- Arduino IDE (for compiling and uploading the display firmware)
- [ScreenScraper](https://www.screenscraper.fr) developer account (free)
- A standard MiSTer setup

## Installation

Installation has two parts: the server component on the MiSTer and the
firmware on the ESP32-based display. The recommended method on the MiSTer side is the
**MiSTer Downloader database** for automatic updates.

See **[`docs/installation.md`](docs/installation.md)** for the complete step-by-step
procedure, the Arduino IDE setup for the display,
and how to request a ScreenScraper developer account.

After installation, configure WiFi, MiSTer IP and ScreenScraper credentials
via `config.ini` on the display microSD card. See **[`docs/configuration.md`](docs/configuration.md)**
for the full reference of available settings, artwork download order, and
asset placement.

## 3D-printable stand

A printable stand for the M5Stack Tab5 is included in the
[`3d-printing/`](3d-printing/) folder. GitHub renders STL files in the
browser, so you can preview the model before downloading.

You can find the model files for the (truly) 2,8" Cheap Yellow Display (horizontal stand) [here](https://www.printables.com/model/708127-cheap-yellow-display-cyd-horizontal-stand-m36-self/files)

The Tab5 STL file is ["M5Stack Tab5 Simple Stand"](https://makerworld.com/en/models/1403228-m5stack-tab5-simple-stand)
by [hkawakami](https://makerworld.com/es/@hkawakami), licensed under
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).

## Architecture

The system has two components that work together:

- **`mister_status_server.py`** — HTTP server on port 8081 running on
  the MiSTer. Watches `/tmp/` state files in real time using `inotify`,
  maintains an in-memory state, and exposes core, game, system and
  network data as JSON/text endpoints. Distinguishes actual game loads
  from OSD navigation by comparing `FILESELECT` and `CURRENTPATH`
  filesystem timestamps at nanosecond precision — no external helper
  scripts are needed.
- **display sketch** — Polls the server every few seconds, downloads
  artwork from ScreenScraper, and renders the HUD interface. Also runs
  its own HTTP server on port 8080 for screenshot capture, accessible
  from any device on the local network at `http://<Display-IP>:8080`.

## M5STack Tab5 Screenshots (1280x720)

![Screensaver](images/Tab5/screenshot01_menu.png)
![System Monitor - CPU and Memory](images/Tab5/screenshot02_cpu_memory_status.png)
![System Monitor - Storage](images/Tab5/screenshot02_storage.png)
![System Monitor - USB devices](images/Tab5/screenshot03_usb_devices.png)
![System Monitor - Arcade system artwork](images/Tab5/screenshot04_arcade.png)
![System Monitor - Arcade subsystem artwork](images/Tab5/screenshot05_arcade_subsystem.png)
![System Monitor - Arcade game artwork](images/Tab5/screenshot06_arcade_game.png)
![System Monitor - Console core artwork](images/Tab5/screenshot07_console.png)
![System Monitor - Console game artwork](images/Tab5/screenshot08_console_game.png)
![System Monitor - Computer core artwork](images/Tab5/screenshot09_computer.png)
![System Monitor - Computer game artwork](images/Tab5/screenshot10_computer_game.png)

## 2,8" Cheap Yellow Display (CYD) Screenshots (320x240)

![Screensaver](images/CYD-2432S028R/01_menu.png)
![System Monitor - CPU and Memory](images/CYD-2432S028R/08_main_HUD.png)
![System Monitor - Storage](images/CYD-2432S028R/10_storage.png)
![System Monitor - Arcade subsystem artwork](images/CYD-2432S028R/03.2_arcade_game_subsystem.png)
![System Monitor - Arcade game artwork](images/CYD-2432S028R/02_arcade_game_fanart.png)
![System Monitor - Console game artwork](images/CYD-2432S028R/04_console_game_3dboxart.png)
![System Monitor - Computer game artwork](images/CYD-2432S028R/06_computer_game_3dboxart.png)

## To Do

### Hardware support

- ~**Cheap Yellow Display (CYD)** — Port to the widely available ESP32-2432S028R family.~
- **5" CYD variant** - Port to the ESP32-S3-8048S050C-I family (800x480 touchscreen).
- **M5Stack Core Basic support** — Port to the original Core Basic (ESP32, 320×240, physical buttons).
- **M5Stack Core S3 support** — Port to the Core S3 (ESP32-S3, 320×240 touchscreen).

### Data and content enrichment

- **RetroAchievements integration** — Show unlocked achievements and progress, building on [odelot/Main_MiSTer](https://github.com/odelot/Main_MiSTer).
- **Enriched game metadata screen** — "Now Playing" view with synopsis, year, publisher, developer, genre.
- **Game Manuals access** — Show manuals for the system or running game from the ⁠Game Manuals Databases by *Moondandy*.
- **Regional cover comparison** — Show EU/US/JP versions of the same game's artwork.
- **Multilanguage descriptions** — Info in the user's preferred language via ScreenScraper.

### AI-powered context layer

- **Historical curiosities** — AI-generated trivia about the loaded game, cached locally.
- **Retro conversation mode** — Activatable AI speech-to-text chat about the loaded game.

### Personal stats and history

- **Playtime tracking** — Hours per game and core, sessions, streaks, most played.
- **Internal achievements** — Device-specific achievements independent of RetroAchievements.

### Touch interaction and control

- **Favorite marking and MGL generation** — Mark the current game as favorite or create an MGL shortcut.
- **Visual core selector** — Touch grid of core and/or game artwork; tap to launch.

### Ambient and connected presence

- **Idle screensaver mode** — Cycle random covers with Ken Burns effect when MiSTer is at menu.
- **MiSTer screenshot reception** — Display native MiSTer screenshots as per-game galleries.
- **External launcher integration** — Show artwork on launches triggered by Zapparoo NFC tags or other web-based launchers.
- **Zapparoo launcher integration** — Show artwork on launches triggered by Zapparoo Launcher.
- **QR codes for expanded information** — On-screen QR linking to MobyGames database.

### Other

- Apply scaling for small images (rarely needed).

## License

MIT License — see LICENSE file for details.
