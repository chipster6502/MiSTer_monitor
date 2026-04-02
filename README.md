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

## Hardware Requirements

- M5Stack M5Tab (ESP32-P4, 1280×720 display)
- MiSTer FPGA with network connectivity
- microSD card for image storage

## Software Requirements

- Arduino IDE with M5Unified and M5GFX libraries
- Python 3 on the MiSTer (included in MiSTer Linux)
- ScreenScraper account (free) at screenscraper.fr

## Installation

### MiSTer Side

1. Copy `mister/mister_status_server.py` to `/media/fat/Scripts/mister_monitor/`
2. Copy `mister/detect_game_load.sh` to `/media/fat/Scripts/mister_monitor/`
3. Make the shell script executable:
```bash
   chmod +x /media/fat/Scripts/mister_monitor/detect_game_load.sh
```
4. Add the following lines to `/media/fat/linux/user-startup.sh` to launch
   both scripts automatically on boot:
```bash
   python3 /media/fat/Scripts/mister_monitor/mister_status_server.py &
   bash /media/fat/Scripts/mister_monitor/detect_game_load.sh &
```

### M5Tab Side

1. Open `Tab5/Tab5.ino` in Arduino IDE
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
4. Select the M5Tab board and upload

### SD Card Setup

Create a `/cores/` folder on the microSD card. Game artwork is downloaded
automatically and organized alphabetically (e.g. `/cores/S/SNES.jpg`).

## Architecture

The system has three components that work together:

- **`mister_status_server.py`** — HTTP server on port 8080. Reads MiSTer state
  files from `/tmp/` and exposes them as JSON/text endpoints.
- **`detect_game_load.sh`** — Shell script that monitors filesystem events to
  distinguish a real game load from cursor navigation in the OSD menu.
- **M5Tab sketch** — Polls the server every few seconds, downloads artwork from
  ScreenScraper, and renders the HUD interface.

## License

MIT License — see LICENSE file for details.