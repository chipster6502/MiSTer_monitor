# Installation

This guide walks through installing the MiSTer Monitor **server** on your
MiSTer, the **firmware** on your display target (M5Stack Tab5 or any of the
Cheap Yellow Display variants), and creating the free ScreenScraper account
used for artwork retrieval.

## Contents

- [MiSTer side](#mister-side)
  - [Recommended: MiSTer Downloader database](#recommended-mister-downloader-database)
  - [Alternative: manual install](#alternative-manual-install)
- [Display side](#display-side)
  - [Recommended: Quick install (web flasher)](#recommended-quick-install-web-flasher)
  - [Alternative: Building from source (advanced)](#alternative-building-from-source-advanced)
- [Creating a ScreenScraper account](#creating-a-screenscraper-account)
  - [Advanced: using your own developer account](#advanced-using-your-own-developer-account)

## MiSTer side

Two installation methods are available. The **MiSTer Downloader** method
is recommended: it installs the server and keeps it updated automatically,
and a one-time setup script takes care of all the system configuration for
you. A manual install is documented afterwards as a fallback.

### Recommended: MiSTer Downloader database

This installs the server through the MiSTer Downloader and configures
everything with a single setup step.

1. Download the drop-in `.ini` file:

   [`downloader_chipster6502_MiSTer_monitor_DB.zip`](https://raw.githubusercontent.com/chipster6502/MiSTer_monitor_DB/db/downloader_chipster6502_MiSTer_monitor_DB.zip)

2. Extract the `.ini` from that ZIP and place it in the **root of your
   MiSTer SD card** (`/media/fat/`).
3. Run *Update All* or *Downloader* from your MiSTer Scripts menu.
   The Downloader installs these files automatically:
   - `/media/fat/Scripts/start_monitor.sh`
   - `/media/fat/Scripts/MiSTer_Monitor_setup.sh`
   - `/media/fat/Scripts/MiSTer_Monitor_uninstall.sh`
   - `/media/fat/Scripts/.config/mister_monitor/mister_status_server.py`
4. Back in the Scripts menu, run **`MiSTer_Monitor_setup`** once.
   This makes the launcher executable, enables auto-start on boot, ensures
   `log_file_entry=1` in `MiSTer.ini`, and starts the server. You can run it
   again at any time; it is safe to repeat.

That's it. Future updates to the server are picked up automatically whenever
you run *Update All* or *Downloader* — no need to run the setup again unless
you have deactivated the monitor. Database repository:
[chipster6502/MiSTer_monitor_DB](https://github.com/chipster6502/MiSTer_monitor_DB).

#### Uninstalling (Downloader method)

Run **`MiSTer_Monitor_uninstall`** from the Scripts menu. This *deactivates*
the monitor: it stops the server and removes the auto-start entry, but leaves
the program files in place so you can re-enable it later by running
`MiSTer_Monitor_setup` again — no re-download needed.

To remove MiSTer Monitor **completely**, after running the uninstall script
also delete the drop-in file
`downloader_chipster6502_MiSTer_monitor_DB.ini` from the root of your SD
card (`/media/fat/`), so the Downloader stops tracking and updating it.

> `log_file_entry=1` in `MiSTer.ini` is left untouched by the uninstall
> script, because other tools may rely on it. Set it back to `0` manually if
> you want.

### Alternative: manual install

If you prefer not to use the MiSTer Downloader, you can install everything
by hand.

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

## Display side

The display firmware can be installed two ways: the **web flasher** (easiest,
recommended) or **building from source** with the Arduino IDE.

### Recommended: Quick install (web flasher)

The fastest way to install the firmware. No Arduino IDE, no compiling.

1. Use **Google Chrome or Microsoft Edge on a desktop computer** (the flasher
   relies on Web Serial, which mobile browsers and Firefox/Safari do not
   support).
2. Connect the display to the computer with a USB cable.
3. Open the flasher page:
   [https://chipster6502.github.io/MiSTer_monitor/flasher/](https://chipster6502.github.io/MiSTer_monitor/flasher/)
4. Click the **Connect** button for your display, select the serial port in
   the browser dialog, and let it flash. The page lists every supported board
   and explains how to tell the CYD variants apart (2.8" 1-USB vs 2-USB, and
   3.5" capacitive vs resistive). It installs the latest released firmware,
   with the ScreenScraper credentials already built in.
5. Prepare the display's microSD card with `config.ini` and the asset images
   (see [`configuration.md`](configuration.md)), then insert it and power on.

### Alternative: Building from source (advanced)

Only needed if you can't use the web flasher — a browser other than Chrome or
Edge on desktop, Linux, or you want to modify the firmware yourself. Most
users should use the web flasher above.

<details>
<summary><strong>Show Arduino IDE build instructions (Tab5 and CYD)</strong></summary>

#### Tab5 side

##### Installing M5Stack board support in Arduino IDE

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

##### Uploading the sketch

No credentials need to be hardcoded in the source. All configuration is read
at boot from `config.ini` on the microSD card (see
[`configuration.md`](configuration.md) for details).

1. Open `mister_monitor_Tab5/mister_monitor_Tab5.ino` in Arduino IDE.
2. Install required libraries via **Tools → Manage Libraries…**:
   - M5Unified
   - JPEGDEC

3. Select the M5Stack Tab5 board and upload.

#### CYD side

All Cheap Yellow Display variants share the same Espressif ESP32 board
package and the same Tools settings — they differ only in which sketch folder
you open (one per panel and touch combination).

##### Installing ESP32 board support in Arduino IDE

The CYD uses the standard Espressif ESP32 board package, not the M5Stack one.

1. Open Arduino IDE and go to **File → Preferences**.
2. In the **Additional boards manager URLs** field, add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Click **OK** to close Preferences.
4. Go to **Tools → Board → Boards Manager…**
5. Search for **esp32** and install the package named **esp32 by Espressif Systems**.
6. Once installed, go to **Tools → Board → ESP32 Arduino** and select
   **ESP32 Dev Module**.
7. Connect the CYD via USB, select the correct port under **Tools → Port**,
   and configure the remaining Tools menu options:
   - Partition Scheme: `Huge APP (3MB No OTA/1MB SPIFFS)`
   - Flash Size: `4MB (32Mb)`
   - PSRAM: `Disabled`
   - Upload Speed: `921600`

##### Uploading the sketch

1. Open the sketch folder for your board variant in Arduino IDE:

   | Board | Sketch folder |
   |---|---|
   | CYD 2.8" 1-USB (ESP32-2432S028R, ILI9341) | `mister_monitor_CYD28R/mister_monitor_CYD28R.ino` |
   | CYD 2.8" 2-USB (ESP32-2432S028, ST7789) | `mister_monitor_CYD28R_2USB/mister_monitor_CYD28R_2USB.ino` |
   | CYD 3.5" capacitive (ESP32-3248S035C, GT911) | `mister_monitor_CYD35C/mister_monitor_CYD35C.ino` |
   | CYD 3.5" resistive (ESP32-3248S035R, XPT2046) | `mister_monitor_CYD35R/mister_monitor_CYD35R.ino` |

   The 2.8" variants are told apart by USB-port count (1 = ILI9341, 2 =
   ST7789). The 3.5" variants share the same panel and PCB; if touch does not
   respond after flashing one, flash the other. Note that screenshot capture
   over HTTP is **not** available on the 3.5" (ST7796) boards, whose panel has
   no SPI readback.
2. Install required libraries via **Tools → Manage Libraries…**:
   - LovyanGFX
   - XPT2046_Touchscreen
   - JPEGDEC
3. Copy `config.ini` to the root of the CYD's microSD card and fill in
   your values (see [`configuration.md`](configuration.md)).
4. Copy the required asset images to the microSD card (see the
   [Asset images](configuration.md#asset-images) section in
   `configuration.md`).
5. Select the ESP32 Dev Module board and upload.

</details>

## Creating a ScreenScraper account

The display downloads artwork from the ScreenScraper API. The app provides
its own developer credentials, so you only need a **free member account** —
no developer account or manual approval required.

1. Register for free at [https://www.screenscraper.fr](https://www.screenscraper.fr).
   Account creation is instant.
2. Take note of your username and password.
3. Enter them in `config.ini` under the `[screenscraper]` section as
   `ss_user` and `ss_pass`. See [`configuration.md`](configuration.md) for
   the configuration reference.

### Advanced: using your own developer account

If you already have your own ScreenScraper developer account and prefer to
use it (for its own quota and identity), set `ss_dev_user` and `ss_dev_pass`
in `config.ini`. When present, they override the app's built-in credentials.
