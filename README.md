# MiSTer FPGA Monitor for M5Stack Tab5

A status monitor for the MiSTer FPGA platform, running on an
Tab5 ESP32 device from M5Stack. Displays the currently loaded game artwork, system
information, storage status, and network details in real time.

## Features

- Real-time game and core artwork display via ScreenScraper API
- Automatic game and system detection from OSD, MiSTer Remote web app and Super Attract Mode (SAM)
- Reliable game load detection using nanosecond-precision filesystem timestamps when loading cores and games through the on-screen menu (OSD)
- Automatic Arcade subsystem detection
- Manual RESCAN GAME button on the image screen for cases where the CRC could not be detected automatically
- Smart SCAN button on monitor pages with global state refresh and forced ROM details rescan
- System monitor (CPU, memory, uptime, storage, network, and connected USB devices panels)
- Touch-based navigation
- Screenshot capture of the Tab5 display, downloadable over the local network via HTTP

## Hardware Requirements

- M5Stack Tab5 (ESP32-P4, 1280×720 display)
- MiSTer FPGA with network connectivity
- microSD card for the Tab5 (image storage)

## Software Requirements

- Arduino IDE (for compiling and uploading the Tab5 firmware)
- ScreenScraper developer account (free) at screenscraper.fr
- A standard MiSTer setup with Python 3 and `inotifywait`, both
  preinstalled on official MiSTer images

## Installation

### MiSTer Side

#### Recommended: automated install

1. Copy the entire `MiSTer/` folder from this repository to a temporary
   location on your MiSTer (e.g. `/tmp/`). You can use `wget`, SFTP, or
   simply copy it from an SD card.
2. SSH into your MiSTer (or open a terminal on it) and run:
```bash
   bash /tmp/MiSTer/install.sh
```
3. Verify that `log_file_entry=1` is set in `/media/fat/MiSTer.ini`
   under the `[MiSTer]` section. The installer warns you if this line
   is missing — without it, core and game detection will not work.

The installer copies all files to their canonical locations, configures
auto-start in `user-startup.sh`, and launches the server immediately.
It is idempotent: running it again on an existing installation simply
updates the files and restarts the server.

To uninstall:
```bash
bash /tmp/MiSTer/uninstall.sh
```

#### Manual install (alternative)

If you prefer to install manually, follow these steps:

1. Copy `MiSTer/Scripts/.config/mister_monitor/mister_status_server.py` to
   `/media/fat/Scripts/.config/mister_monitor/` on your MiSTer (create the
   directory if it doesn't exist).
2. Copy `MiSTer/Scripts/start_monitor.sh` to `/media/fat/Scripts/`.
3. Make the script executable:
```bash
   chmod +x /media/fat/Scripts/start_monitor.sh
```
4. Add the following line to `/media/fat/linux/user-startup.sh` to launch
   the server automatically on boot:
```bash
   /media/fat/Scripts/start_monitor.sh start
```
5. Enable log_file_entry in `/media/fat/MiSTer.ini` by setting (under
   the `[MiSTer]` section):
```ini
log_file_entry=1
```
This is required for the Python server to detect which core and game are
currently loaded.

#### Screenshot HTTP Server

The Tab5 runs a lightweight HTTP server on port 8080 that captures its own display
on demand and streams it as a standard 24-bit BMP file directly to the requester.
To view or download the latest screenshot, open a browser on any device on the same
network and navigate to:

```
http://<Tab5-IP>:8080/screenshot.jpg
```

The root page at `http://<Tab5-IP>:8080/` also provides a live preview that auto-refreshes
every 5 seconds and a download button. Replace `<Tab5-IP>` with the IP address shown in the
Tab5 Network Terminal panel.

### Tab5 Side

#### Installing M5Stack Board Support in Arduino IDE

Before opening the sketch you need to register the M5Stack board package
with Arduino IDE so the Tab5 target appears in the board selector.

1. Open Arduino IDE and go to **File → Preferences**.
2. In the **Additional boards manager URLs** field, add the following URL
   (click the icon to the right of the field if you need to add it to an
   existing list):
   ```
   https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/arduino/package_m5stack_index.json
   ```
3. Click **OK** to close Preferences.
4. Go to **Tools → Board → Boards Manager…**
5. Search for **M5Stack** and install the package named **M5Stack by M5Stack**.
   The installation may take a few minutes as it downloads the ESP32 toolchain.
6. Once installed, go to **Tools → Board → M5Stack** and select **M5Stack Tab5**.
7. Connect the Tab5 via USB-C, select the correct port under **Tools → Port**,
   and you are ready to upload.

#### Uploading the Sketch

No credentials need to be hardcoded in the source. All configuration is read
at boot from `config.ini` on the microSD card (see the SD Card Setup section
below).

1. Open `mister_monitor_Tab5/mister_monitor_Tab5.ino` in Arduino IDE.
2. Install required libraries via **Tools → Manage Libraries…**:
   - M5Unified
   - JPEGDEC
   - ArduinoJson
3. Select the M5Stack Tab5 board and upload.

### Tab5 SD Card Setup

#### config.ini

All user configuration lives in a single file placed in the **root** of the
microSD card: `/config.ini`. The sketch reads it at boot before connecting to
WiFi, so no credentials ever need to be hardcoded in the source.

Copy `config.ini` from the repository root to the SD card and fill in your
values:

```ini
[wifi]
ssid=YOUR_WIFI_SSID
password=YOUR_WIFI_PASSWORD

[mister]
ip=192.168.1.100          ; must be a static IP — see note below

[screenscraper]
ss_user=YOUR_SS_USERNAME
ss_pass=YOUR_SS_PASSWORD
ss_dev_user=YOUR_SS_USERNAME
ss_dev_pass=YOUR_SS_DEV_PASSWORD
```

Any key that is absent keeps the built-in default. The full list of available
keys is documented inside `config.ini` itself with comments explaining each
option.

**Static MiSTer IP address** — the Tab5 connects to the MiSTer by IP, so a
static address is required. Edit `/etc/dhcpcd.conf` on the MiSTer and add:

```
interface eth0
static ip_address=192.168.0.XX/24
static routers=192.168.0.X
static domain_name_servers=192.168.0.X 8.8.8.8
```

Use `interface wlan0` instead of `eth0` for a wireless connection. The `/24`
netmask covers the most common home network setup; adjust if your router uses
a different subnet.

#### Artwork download order

The sketch downloads artwork from ScreenScraper for each core and game it
encounters. You can control which image types are tried and in what order via
keys in the `[images]` section of `config.ini`:

| Key | Used for |
|---|---|
| `core_media_order` | System-level art (non-arcade cores) |
| `arcade_subsystem_media_order` | Arcade subsystem art (CPS1, Neo Geo, Sega Classics…) |
| `arcade_media_order` | Arcade game ROMs |
| `game_media_order` | Non-arcade game ROMs (consoles, computers) |

Each value is a comma-separated list of tokens tried left to right until one
download succeeds. Available tokens:

| Token | Description |
|---|---|
| `wheel-steel` | Steel/metallic background logo wheel |
| `wheel-carbon` | Carbon fibre background logo wheel |
| `wheel` | Plain/transparent background logo wheel |
| `box3d` | 3-D rendered box art |
| `box2d` | 2-D flat box scan |
| `fanart` | Fan-made promotional art (often landscape format) | 
| `marquee` | Arcade cabinet marquee header |
| `screenshot` | In-game title screenshot |
| `photo` | Real photograph of hardware or cartridge |
| `illustration` | Promotional illustration or poster |

**Region order within each token** — the `region=` key in `[screenscraper]`
controls which regional variant is tried first. The remaining regions follow
in fixed order, and the no-region generic variant is tried last. For example,
with `region=eu` and token `box3d` the sequence is:
`box-3D(eu)` → `box-3D(wor)` → `box-3D(us)` → `box-3D(jp)` → `box-3D`.
Two exceptions: `box2d` has no generic variant in the API; `marquee` tries
the generic variant first as it is the most common one in ScreenScraper's
database.

Default orders applied out of the box:

```ini
; System/core artwork: steel wheel first (cleanest on the HUD background)
core_media_order=wheel-steel,wheel-carbon,wheel,photo,illustration,box3d,box2d,marquee,fanart,screenshot

; Arcade subsystems (CPS1, Neo Geo, Konami 573, ...): wheels only
arcade_subsystem_media_order=wheel-steel,wheel-carbon,wheel

; Arcade game ROMs: logo art before boxes (most titles have no physical box)
arcade_media_order=fanart,marquee,wheel-carbon,wheel-steel,wheel,box3d,box2d,screenshot

; Non-arcade game ROMs: physical box art first
game_media_order=box3d,box2d,wheel-carbon,wheel-steel,wheel,fanart,marquee,screenshot
```

#### Asset Images

The repository includes a set of needed images in the
`assets/` folder. Copy them to the microSD card as follows:

- `frame01.jpg`, `frame02.jpg`, `logomister.jpg` and `menu.jpg` must be placed inside
  the `/cores/` folder.
- `Arcade.jpg` and `Arcade_75.jpg` must be placed inside `/cores/A/`.

Core and game images that are missing will be downloaded automatically from
ScreenScraper the first time that core/game is detected.

## Getting a ScreenScraper Developer Account

The ScreenScraper API requires a **developer account** in addition to a
regular user account. The process has two steps:

### 1. Create a regular user account

Register for free at [https://www.screenscraper.fr](https://www.screenscraper.fr).
Take note of your username and password — you will enter them in the sketch.

### 2. Request a developer account

Developer accounts are granted manually by the ScreenScraper team via their
forum. Once logged in:

1. Go to [https://www.screenscraper.fr/forum.php](https://www.screenscraper.fr/forum.php).
2. Find the thread titled **"ScreenScraper WebAPI"**.
3. Post a reply in that thread requesting a developer account. In your
   message, briefly explain what you intend to build with the API — for
   example: *"I want to use an open-source game artwork monitor for the
   MiSTer FPGA platform running on an ESP32-based display."*
4. The team will review your request and enable the developer role on your
   account, typically within a few days.

Once your developer account is active, you will receive a response in the forum
with confirmation and instructions for its use. Enter your credentials in
`config.ini` under the `[screenscraper]` section.

## Architecture

The system has two components that work together:

- **`mister_status_server.py`** — HTTP server on port 8081 running on
  the MiSTer. Watches `/tmp/` state files in real time using `inotify`,
  maintains an in-memory state, and exposes core, game, system and
  network data as JSON/text endpoints. Distinguishes actual game loads
  from OSD navigation by comparing `FILESELECT` and `CURRENTPATH`
  filesystem timestamps at nanosecond precision — no external helper
  scripts are needed.
- **Tab5 sketch** — Polls the server every few seconds, downloads
  artwork from ScreenScraper, and renders the HUD interface. Also runs
  its own HTTP server on port 8080 for screenshot capture.

## Screenshots

![Screensaver](images/screenshot01_menu.png)
![System Monitor - CPU and Memory](images/screenshot02_cpu_memory_status.png)
![System Monitor - Storage](images/screenshot02_storage.png)
![System Monitor - USB devices](images/screenshot03_usb_devices.png)
![System Monitor - Arcade system artwork](images/screenshot04_arcade.png)
![System Monitor - Arcade subsystem artwork](images/screenshot05_arcade_subsystem.png)
![System Monitor - Arcade game artwork](images/screenshot06_arcade_game.png)
![System Monitor - Console core artwork](images/screenshot07_console.png)
![System Monitor - Console game artwork](images/screenshot08_console_game.png)
![System Monitor - Computer core artwork](images/screenshot09_computer.png)
![System Monitor - Computer game artwork](images/screenshot10_computer_game.png)

## To Do

- **MiSTer Downloader integration (in progress)** — Custom database for
  the MiSTer Downloader ecosystem, so the server-side scripts can be
  installed and updated through the standard MiSTer Downloader workflow.
- **M5Stack Core Basic support** — Port the interface to the original M5Stack
  Core Basic (ESP32, 320×240 display, physical buttons). The ScaledDisplay
  wrapper and layout system are designed to support multiple resolutions, so
  this should be achievable with board-specific coordinate profiles and button
  mappings.
- **M5Stack Core S3 support** — Add a target for the Core S3 (ESP32-S3,
  320×240 display, touchscreen). This device shares the touch interface with
  the Tab5 but runs at the lower resolution, making it a natural intermediate
  target between the two existing hardware profiles.
- **Other ESP32-based displays support**
- Ensure it works with other detection sources like NFC readers and other web-based launchers.
- Apply scaling for small images (really necessary in few cases)

## License

MIT License — see LICENSE file for details.
