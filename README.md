# MiSTer FPGA Monitor for M5Tab

A status monitor for the MiSTer FPGA platform, running on an
Tab5 ESP32 device from M5Stack. Displays the currently loaded game artwork, system
information, storage status, and network details in real time.

## Features

- Real-time game and core artwork display via ScreenScraper API
- Automatic game and system detection from OSD, MiSTer Remote web app and Super Attract Mode (SAM)
- Automatic Arcade subsystem detection
- System monitor (CPU, memory, uptime)
- Storage, network, and USB device panels
- Touch-based navigation
- Screenshot capture of the Tab5 display, downloadable over the local network via HTTP

## Hardware Requirements

- M5Stack Tab5 (ESP32-P4, 1280×720 display)
- MiSTer FPGA with network connectivity
- microSD card for the Tab5 (image storage)

## Software Requirements

- Arduino IDE with M5Unified and M5GFX libraries
- Python 3 on the MiSTer (included in MiSTer Linux)
- ScreenScraper dev account (free) at screenscraper.fr

## Installation

### MiSTer Side

1. Copy `mister/mister_status_server.py` to `/media/fat/Scripts/mister_monitor/`
2. Copy `mister/detect_game_load.sh` to `/media/fat/Scripts/mister_monitor/`
3. Copy `mister/start_monitor.sh` to `/media/fat/Scripts/mister_monitor/`
4. Make the shell script executable:
```bash
   chmod +x /media/fat/Scripts/mister_monitor/detect_game_load.sh
```
4. Add the following lines to `/media/fat/linux/user-startup.sh` to launch
   both scripts automatically on boot:
```bash
   python3 /media/fat/Scripts/mister_monitor/mister_status_server.py &
   bash /media/fat/Scripts/mister_monitor/detect_game_load.sh &
```

#### Screenshot HTTP Server

The Tab5 can capture screenshots of its own display and make them available
for download over the local network. The `mister_status_server.py` script
includes a lightweight HTTP endpoint for this purpose.

Once the server is running on the MiSTer, any screenshot saved by the Tab5
to its microSD card is also accessible from a browser or `curl` on the same
network. To download the latest screenshot, open:

```
http://<Tab5-IP>:8080/screenshot.jpg
```

Replace `<Tab5-IP>` with the IP address shown in the Tab5 Network Terminal
panel. No extra software is required on the MiSTer side — the endpoint is
served directly by `mister_status_server.py`.

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

1. Open `mister_monitor_Tab5/mister_monitor_Tab5.ino` in Arduino IDE
2. Fill in your credentials at the top of the file:
```cpp
   #define MISTER_IP           "192.168.x.x"   // Your MiSTer IP address
   #define SCREENSCRAPER_USER  "your_username"
   #define SCREENSCRAPER_PASS  "your_password"
```
It is necessary to use a static MiSTer IP address.
How to configure a static MiSTer IP address in your MiSTer: 
You will have to modify the /etc/dhcpcd.conf file to setup a static ip.
Add something like this on the bottom of the file:
```cpp
interface eth0
static ip_address=192.168.0.XX/24
static routers=192.168.0.X
static domain_name_servers=192.168.0.X 8.8.8.8
```
For DNS, you don't need to Provide more than one but if you do, just leave a space between them.
The Static IP address has the /24 for the netmask, if you don't know what your network has, then leave it as /24 as that's the most common for home networks.

Use interface wlan0 for wireless lan

3. Install required libraries via Arduino Library Manager:
   - M5Unified
   - M5GFX
   - JPEGDEC
4. Select the M5Tab5 board and upload

### Tab5 SD Card Setup

Create a `/cores/` folder on the microSD card. Game artwork is downloaded
automatically and organized alphabetically (e.g. `/cores/S/SNES.jpg`).

#### Asset Images

The repository includes a set of needed images in the
`assets/` folder. Copy them to the microSD card as follows:

- `frame01.jpg`, `frame02.jpg`, `logomister.jpg` and `menu.jpg` must be placed inside
  the `/cores/` folder.
- `Arcade.jpg` must be placed inside
  `/cores/A/Arcade.jpg`.

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
with confirmation and instructions for its use.

## Architecture

The system has three components that work together:

- **`mister_status_server.py`** — HTTP server on port 8080. Reads MiSTer state
  files from `/tmp/` and exposes them as JSON/text endpoints.
- **`detect_game_load.sh`** — Shell script that monitors filesystem events to
  distinguish a real game load from cursor navigation in the OSD menu.
- **Tab5 sketch** — Polls the server every few seconds, downloads artwork from
  ScreenScraper, and renders the HUD interface.

## To Do

- **M5Stack Core Basic support** — Port the interface to the original M5Stack
  Core Basic (ESP32, 320×240 display, physical buttons). The ScaledDisplay
  wrapper and layout system are designed to support multiple resolutions, so
  this should be achievable with board-specific coordinate profiles and button
  mappings.
- **M5Stack Core S3 support** — Add a target for the Core S3 (ESP32-S3,
  320×240 display, touchscreen). This device shares the touch interface with
  the Tab5 but runs at the lower resolution, making it a natural intermediate
  target between the two existing hardware profiles.

## License

MIT License — see LICENSE file for details.
