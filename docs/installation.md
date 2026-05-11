# Installation

This guide walks through installing the MiSTer Monitor server component on
your MiSTer, the firmware on the M5Stack Tab5, and the ScreenScraper
developer account required for artwork retrieval.

## MiSTer side

Three installation methods are available. The MiSTer Downloader method is
recommended for automatic future updates.

### Recommended: MiSTer Downloader database

The easiest way to install the server-side component is through the
MiSTer Downloader. Add this database to your MiSTer:

1. Download the drop-in `.ini` file:

   [`downloader_chipster6502_MiSTer_monitor_DB.zip`](https://raw.githubusercontent.com/chipster6502/MiSTer_monitor_DB/db/downloader_chipster6502_MiSTer_monitor_DB.zip)

2. Extract the `.ini` from that ZIP and place it in the **root of your
   MiSTer SD card** (`/media/fat/`).
3. Run *Update All* or *Downloader* from your MiSTer scripts menu.
   The files are installed automatically into:
   - `/media/fat/Scripts/start_monitor.sh`
   - `/media/fat/Scripts/.config/mister_monitor/mister_status_server.py`
4. After the first install, do these one-time setup steps:
```bash
   chmod +x /media/fat/Scripts/start_monitor.sh
```
   Add this line to `/media/fat/linux/user-startup.sh`:
```bash
   /media/fat/Scripts/start_monitor.sh start
```
   And enable `log_file_entry=1` in `/media/fat/MiSTer.ini` (under the
   `[MiSTer]` section).
5. Reboot or run `/media/fat/Scripts/start_monitor.sh start`.

Future updates to the server are automatically picked up by *Update All*
or *Downloader*. Database repository:
[chipster6502/MiSTer_monitor_DB](https://github.com/chipster6502/MiSTer_monitor_DB).

### Alternative: install.sh

If you prefer not to use the MiSTer Downloader, the repository includes
an automated installer that does the same job in one step.

1. Copy the contents of the entire `MiSTer/` folder from this
   repository to your MiSTer's SD card (e.g. `/media/fat/MiSTer_monitor_install/`).
2. SSH into your MiSTer and run the installer:
```bash
   bash /media/fat/MiSTer_monitor_install/install.sh
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
bash /media/fat/Scripts/.config/mister_monitor/uninstall.sh
```

### Manual install

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

## Tab5 side

### Installing M5Stack board support in Arduino IDE

Before opening the sketch you need to register the M5Stack board package
with Arduino IDE so the Tab5 target appears in the board selector.

1. Open Arduino IDE and go to **File → Preferences**.
2. In the **Additional boards manager URLs** field, add the following URL
   (click the icon to the right of the field if you need to add it to an
   existing list):
   ```
   https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json 
   ```
3. Click **OK** to close Preferences.
4. Go to **Tools → Board → Boards Manager…**
5. Search for **M5Stack** and install the package named **M5Stack by M5Stack**.
   The installation may take a few minutes as it downloads the ESP32 toolchain.
6. Once installed, go to **Tools → Board → M5Stack** and select **M5Stack Tab5**.
7. Connect the Tab5 via USB-C, select the correct port under **Tools → Port**,
   and you are ready to upload.

### Uploading the sketch

No credentials need to be hardcoded in the source. All configuration is read
at boot from `config.ini` on the microSD card (see
[`configuration.md`](configuration.md) for details).

1. Open `mister_monitor_Tab5/mister_monitor_Tab5.ino` in Arduino IDE.
2. Install required libraries via **Tools → Manage Libraries…**:
   - M5Unified
   - JPEGDEC
   - ArduinoJson
3. Select the M5Stack Tab5 board and upload.

## Getting a ScreenScraper developer account

The ScreenScraper API requires a **developer account** in addition to a
regular user account. The process has two steps:

### 1. Create a regular user account

Register for free at [https://www.screenscraper.fr](https://www.screenscraper.fr).
Take note of your username and password — you will enter them in `config.ini`.

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

Once your developer account is active, you will receive a response in the
forum with confirmation and instructions for its use. Enter your credentials
in `config.ini` under the `[screenscraper]` section. See
[`configuration.md`](configuration.md) for the configuration reference.
