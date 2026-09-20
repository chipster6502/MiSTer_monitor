# Configuration

## Contents

- [SD Card content](#sd-card-content)
- [config.ini](#configini)
- [MiSTer IP address](#mister-ip-address)
- [Artwork download order](#artwork-download-order)
- [The image screen](#the-image-screen)
- [Standby and clock](#standby-and-clock)
- [Board-specific options](#board-specific-options)
- [Web interface](#web-interface)
- [All keys at a glance](#all-keys-at-a-glance)
- [RetroAchievements](#retroachievements)

## SD Card content

The microSD card must be formatted as **FAT32**. exFAT is not read by the
display, and cards larger than 32 GB are often shipped as exFAT, so reformat
before use if in doubt.

The repository includes a ready-to-use microSD card layout under
`SD_card_content/`. Choose the subfolder that matches your hardware,
copy its contents to the **root** of your microSD card, and edit
`config.ini` with your credentials before first boot.

All other core and game images are downloaded automatically from
ScreenScraper the first time that core or game is detected.
Alphabetical subfolders (`/cores/B/`, `/cores/C/`, …) are created
on demand by the firmware — you do not need to create them manually.

Two files in that layout are worth knowing about when you update a card that
is already in use, because the firmware looks for them by name:

| File | Boards | Purpose |
|---|---|---|
| `/cores/logo_standby.565` | all | The MiSTer logo shown in [standby](#standby-and-clock), as raw pixels. Without it the display falls back to a JPEG logo, which shows faint blotches in flat colours, or to plain text. |
| `/c6_firmware.bin` | Guition | Firmware for the WiFi coprocessor, used only when the one on the board is too old to connect — see [Guition](#guition-jc8012p4a1c). |

The [web interface](#web-interface) can upload both without taking the card
out.

## config.ini

All user configuration lives in a single file placed in the **root** of the
microSD card: `/config.ini`. The sketch reads it at boot before
connecting to WiFi, so no credentials need to be hardcoded in the source.
The same `config.ini` format is used by all hardware targets (Tab5, CYD,
and future ports).

```ini
[wifi]
ssid=YOUR_WIFI_SSID
password=YOUR_WIFI_PASSWORD

[mister]
; IP address of the MiSTer. Blank = auto-discover it on the LAN (recommended
; with a single MiSTer). Set = use that MiSTer and no other.
ip=

[screenscraper]
ss_user=YOUR_SS_USERNAME
ss_pass=YOUR_SS_PASSWORD
; Advanced: only to use your own developer account instead of the built-in one
;ss_dev_user=
;ss_dev_pass=
```

Any key that is absent keeps the built-in default, so a `config.ini` written
for an earlier version keeps working after a firmware update — new options
simply stay at their defaults until you add them. Every key is documented
inside `config.ini` itself, with a comment explaining each option, and
[summarised below](#all-keys-at-a-glance). To adopt new keys, copy their lines
from the `config.ini` under `SD_card_content/` for your board.

Section headers such as `[ui]` only organise the file: the display matches
keys by name, so a key works under whichever section it is written.

After the first boot you no longer need to take the card out to change a
setting: the [web interface](#web-interface) edits the file from a browser.
Settings are read once at startup, so a change takes effect after a reboot.

## MiSTer IP address

The display locates the MiSTer automatically at boot via UDP broadcast —
no static IP is needed on the MiSTer and no value is required in `config.ini`.

With `ip=` blank the display adopts the first MiSTer that answers the
broadcast. That is what you want with a single MiSTer, but with several on
the same network you cannot choose which one answers first.

Setting `ip=` pins the display to that MiSTer: it never adopts a different
one, even while the pinned MiSTer is powered off. Set it when:

- you have **several MiSTers** and want each display to follow a specific one;
- your router blocks UDP broadcast (uncommon in home networks).

```ini
[mister]
; example only — use your MiSTer's actual IP
ip=192.168.1.50
```

To keep that address stable across reboots, give the MiSTer a fixed IP:
reserve a DHCP lease for its MAC in your router's admin panel (simplest, no
changes on the MiSTer), or set a static address by editing
`/etc/dhcpcd.conf` on it.

## Artwork download order

The sketch downloads artwork from ScreenScraper for each core and game it
encounters. If an [artwork pack](installation.md#artwork-packs-optional) is
installed on the MiSTer, the pack image is used first for games and the
settings below only apply when the pack has none; core images always come
from ScreenScraper. You can control which image types are tried and in what
order via keys in the `[images]` section of `config.ini`:

| Key | Used for |
|---|---|
| `core_media_order` | System-level art (non-arcade cores) |
| `arcade_subsystem_media_order` | Arcade subsystem art (CPS1, Sega Classics…) |
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
| `screenshot` | In-game screenshot; falls back to the title screen when there is none |
| `titlescreen` | Title screen only |
| `photo` | Real photograph of the hardware |
| `illustration` | Hardware illustration |
| `mix1` | Recalbox composite mix, version 1 |
| `mix2` | Recalbox composite mix, version 2 (`mix` is accepted as an alias) |
| `screenmarquee` | **System-level only.** Marquee-style system image that fills the frame |
| `background` | **System-level only.** System wallpaper (not in the default order) |

Box and game artwork (`box3d`, `box2d`, `fanart`, `marquee`, `screenshot`,
`titlescreen`, `mix1`, `mix2`) exists per game, not per system, so those tokens
belong in `game_media_order` and `arcade_media_order`. Listing them in
`core_media_order` or `arcade_subsystem_media_order` only spends requests that
cannot succeed. The other way round, `screenmarquee` and `background` only
exist per system.

**Region order within each token** — the `region=` key in `[screenscraper]`
controls which regional variant is tried first. The remaining regions follow
in fixed order, then ScreenScraper's own `ss` region (what an upload carries
when the contributor set none, and often the only region a box exists in),
and the no-region generic variant is tried last. For example, with
`region=eu` and token `box3d` the sequence is:
`box-3D(eu)` → `box-3D(wor)` → `box-3D(us)` → `box-3D(jp)` → `box-3D(ss)` → `box-3D`.

Default orders applied out of the box:

```ini
core_media_order=wheel-steel,wheel-carbon,wheel,screenmarquee,illustration,photo

arcade_subsystem_media_order=wheel-steel,wheel-carbon,wheel,screenmarquee

arcade_media_order=fanart,marquee,wheel-carbon,wheel-steel,wheel,box3d,box2d,screenshot

game_media_order=box3d,box2d,wheel-carbon,wheel-steel,wheel,fanart,marquee,screenshot
```

## The image screen

While a game is loaded the display shows artwork full screen. These keys decide
what it shows and how:

| Key | Default | What it does |
|---|---|---|
| `image_mode` | `rotate` | `rotate` alternates game artwork and system artwork; `game` and `system` stay on one and never switch. A game with no artwork of its own still falls back to the system image. |
| `core_image_timeout` | `30000` | How long (ms) each slide stays up in `rotate` mode. |
| `system_image_timeout` | `0` | A separate dwell (ms) for the system slide. `0` means the same as `core_image_timeout`. |
| `info_in_rotation` | `false` | Adds the GAME INFO panel to the rotation as a third slide: game image → game info → system image. No effect with `image_mode=game` or `system`. |
| `image_upscale` | `false` | Grows artwork that is smaller than the display area until it fills it, aspect preserved. |
| `image_upscale_max` | `2.5` | Ceiling on that growth, as a multiplier (1.0–2.875). |
| `kiosk_mode` | `false` | Gives the whole panel to the artwork and hides the footer strip. |
| `kiosk_hide_delay_ms` | `5000` | How long the footer stays up after a tap in kiosk mode (500–600000). |

**Upscaling** is most visible on the Tab5 and the Guition, whose panels are
larger than the source art: a wheel is 600×300 whatever the game or system, so
filling a 1280-wide panel roughly doubles it and looks blocky up close, while a
1024×512 screen marquee only needs about a quarter more and holds up well. On
the CYD boards ScreenScraper already serves most art at or above the size the
panel asks for, so the setting only reaches the short sources. At the default
ceiling of 2.5 a 600 px wheel reaches the edge of a 1280-wide panel while a
320 px screenshot stops short of it and keeps its border.

**Kiosk mode** starts with the footer hidden. One tap brings it up over the
image for `kiosk_hide_delay_ms`; a second tap then works as usual (open the
monitor, GAME INFO, or the footer buttons on the CYD boards). Artwork is
requested from ScreenScraper at the full panel height in this mode, so images
already cached on the card were fetched shorter and show a black band above and
below until they are fetched again. To refresh them, set
`force_core_redownload` and `force_game_redownload` to `true` for one boot,
then back to `false` — or delete the images you care about from the
[web interface](#web-interface).

## Standby and clock

When the MiSTer is switched off there is nothing to show, so the display dims
to a standby screen. It leaves standby on its own as soon as the MiSTer answers
again or the core or game changes, and at a touch.

| Key | Default | What it does |
|---|---|---|
| `standby_screen` | `clock` | `clock`: a large seven-segment clock, the date, whether the MiSTer answers, and the core and game it is running. `minimal`: the MiSTer logo and a small state dot, cyan while the MiSTer answers and orange while it does not. `clock` needs a `timezone`; until one is set and the time has been fetched, `minimal` is shown. |
| `standby_when_offline` | `true` | Go to standby once the MiSTer has been unreachable for `standby_offline_min` minutes. `false` turns this off. |
| `standby_offline_min` | `3` | Minutes without an answer before the MiSTer counts as switched off (1–1440). A single answer restarts the count. |
| `standby_idle_min` | `0` | Also go to standby after this many minutes without a core change, a game change, a new achievement or a touch — with the MiSTer running too, so a long session on one game will dim the screen. `0` = never. |
| `standby_brightness` | `10` | Backlight level in standby, 0–255. `0` switches the backlight off. |
| `standby_dim` | `100` | Darkens everything the standby screen draws, as a percentage of full colour (5–100). For units whose backlight only switches on and off — see below. |

On the **Tab5 and the Guition** the logo breathes slowly at the centre of the
`minimal` screen, and the `clock` screen carries the logo too. On the **CYD
boards** the logo is fixed and the backlight does the breathing, with
`standby_brightness` as the top of the breath; their `clock` screen has no
logo.

On some Tab5 units the backlight does not dim gradually: `standby_brightness=0`
turns the screen off, but every other value looks the same. If that is what you
see, leave `standby_brightness` alone and lower `standby_dim` instead — `30` is
a good starting point. The Guition and CYD backlights dim properly, so leave
`standby_dim` at `100` there; lowering both darkens the screen twice.

The logo is `/cores/logo_standby.565` on the card: raw pixels, which reach the
screen exactly as drawn. Without it the display uses `/cores/logo_standby.jpg`
(and then `/cores/logo_mister.jpg` on the Tab5 and the Guition), though JPEG
decoding leaves faint blotches in flat colours; with none of them the name is
drawn as text. To use a logo of your own, save it as `logo_standby.jpg` — a
baseline JPEG on a black background, 400×200 at most on the Tab5 and the
Guition — and remove the `.565` file.

### Clock

The display has no clock of its own. It fetches the time from the internet
over WiFi, and it has to be told where it is:

| Key | Default | What it does |
|---|---|---|
| `timezone` | blank | One of the names below, or a POSIX TZ string. Blank: no time is fetched or shown anywhere. |
| `ntp_server` | `pool.ntp.org` | Time server. |
| `clock_24h` | `true` | `false` switches to a 12-hour clock with AM/PM. |

Pick any name whose countries share your time; daylight saving changes are
handled for the ones that have them.

| Name | Covers |
|---|---|
| `uk` | United Kingdom, Ireland |
| `portugal` | Portugal |
| `iceland` | Iceland, Ghana, Senegal, Mali |
| `spain` | Spain, France, Germany, Italy, Netherlands, Poland, Sweden |
| `nigeria` | Nigeria, Algeria, Tunisia, Angola, Cameroon |
| `greece` | Greece, Finland, Romania, Bulgaria, Ukraine, Estonia |
| `southafrica` | South Africa, Zimbabwe, Zambia, Botswana |
| `turkey` | Turkey, Saudi Arabia, Kenya, Russia (Moscow) |
| `dubai` | UAE, Oman, Georgia, Armenia, Azerbaijan |
| `india` | India, Sri Lanka |
| `thailand` | Thailand, Vietnam, Cambodia, Indonesia (Jakarta) |
| `china` | China, Taiwan, Hong Kong, Singapore, Malaysia, Philippines |
| `japan` | Japan, South Korea |
| `sydney` | Australia (Sydney, Melbourne, Hobart) |
| `newzealand` | New Zealand |
| `argentina` | Argentina, Uruguay, Brazil (São Paulo, Rio) |
| `venezuela` | Venezuela, Bolivia, Paraguay, Dominican Republic |
| `colombia` | Colombia, Peru, Ecuador |
| `useastern` | USA (New York, Miami), Canada (Toronto) |
| `mexico` | Mexico, Guatemala, Costa Rica, El Salvador |
| `uscentral` | USA (Chicago, Dallas), Canada (Winnipeg) |
| `usmountain` | USA (Denver), Canada (Edmonton) |
| `uspacific` | USA (Los Angeles, Seattle), Canada (Vancouver) |

Anywhere not listed, write a POSIX TZ string instead. Its sign is the opposite
of the usual UTC offset — UTC+1 is written `-1`:

```ini
[ui]
timezone=CET-1CEST,M3.5.0,M10.5.0/3
```

## Board-specific options

### M5Stack Tab5

The Tab5 is the only board with a speaker.

| Key | Default | What it does |
|---|---|---|
| `sound` | `true` | `false` silences every tone: the boot jingle and the touch-button beeps. |
| `sound_volume` | `128` | Speaker volume, 0–255. |

### Guition JC8012P4A1C

**Panel revision.** Guition ships this 10.1" board with two different LCD
panels behind the same controller, and the two cannot be told apart by the
firmware. Units from **batch 2624 onward need `panel_rev=v2`**; earlier ones
need `v1`, the default. The batch is the figure in parentheses after the SKU on
the sticker at the back of the device: `101153001-V2 (2631)` is batch 2631, so
`v2`. With the wrong value the screen shows horizontal colour banding instead
of the interface — so this edit has to happen on a computer, in the card's
`config.ini`:

```ini
[ui]
panel_rev=v2
```

**WiFi coprocessor.** The ESP32-P4 has no radio of its own: WiFi runs on an
ESP32-C6 on the same board, with its own firmware. Some units ship with a
version too old to work with the display firmware — it lists your network at
full strength and then never connects. Copy `c6_firmware.bin` from
`SD_card_content/Guition/` to the **root** of the card: at boot the display
compares versions, and when the coprocessor is behind it updates it from that
file and restarts. The image is written to the coprocessor's spare slot and
only made active at the end, so a power cut mid-update leaves the old firmware
in place and the check runs again at the next boot. On a board that is already
up to date the file is ignored.

## Web interface

Once the display is on your network it serves a small web interface of its own
on **port 8080**. This is the display's own IP address, not the MiSTer's — the
display shows it on the boot screen once it connects, and on the **Network**
page it is listed as **Monitor IP**, right below the MiSTer's.

| URL | What it does |
|---|---|
| `http://<display-ip>:8080/config` | Edits `config.ini` on the microSD card in a plain text editor, comments and all. Saving keeps the previous file as `config.ini.bak` on the same card, and the page offers a **Reboot** button — settings are read once at startup, so a restart is what applies them. |
| `http://<display-ip>:8080/files` | Browses the microSD card: open folders, download files, upload new ones, create folders, and delete what you no longer need. Deleting a game's artwork makes it be fetched again the next time you load it; uploading your own replaces it. |
| `http://<display-ip>:8080/` | The screenshot page, on the boards whose panel supports readback (2.8" CYD, Tab5, Guition). On the 3.5" boards this is a landing page instead. |

Artwork uploaded through `/files` must be **baseline** JPEG: progressive JPEGs
are rejected with a message, because the display's decoder (JPEGDEC) cannot
read them. Most image editors offer the choice when exporting. Deleting covers
files and empty folders — empty a folder before removing it.

Two keys in the `[ui]` section control the interface:

```ini
[ui]
; Disable the whole web interface. Default: true
web_config=true

; Optional password. Blank means no password, which is the usual choice on a
; home network. If set, the browser asks for it, with the username "admin".
web_password=
```

The interface is served over plain HTTP on your LAN, like the MiSTer's own
Samba and FTP shares. Note that the editor shows `config.ini` as it is, so
anyone who can reach the page can read your WiFi and ScreenScraper passwords —
set `web_password`, or `web_config=false`, if that matters on your network.

## All keys at a glance

Defaults are what the firmware uses when the key is absent. Keys apply to all
six boards unless noted.

| Section | Key | Default | What it does |
|---|---|---|---|
| `[wifi]` | `ssid`, `password` | — | Your WiFi network. No quotes around the values. |
| `[mister]` | `ip` | blank | Blank: auto-discover the MiSTer. Set: follow that MiSTer and no other — see [MiSTer IP address](#mister-ip-address). |
| `[screenscraper]` | `ss_user`, `ss_pass` | — | Your free ScreenScraper member account. |
| | `ss_dev_user`, `ss_dev_pass` | built in | Advanced: your own developer account instead of the built-in one. |
| | `region` | `wor` | Preferred artwork region, tried first: `wor`, `us`, `eu` or `jp`. |
| | `timeout` | `30000` | HTTP timeout (ms) for API requests. |
| | `retries` | `2` | Retry attempts when ScreenScraper is overloaded. |
| | `use_https` | `false` | Only enable if plain HTTP stops working. |
| `[gameinfo]` | `info_lang` | `en` | Language for synopsis and genre: `en es pt fr de it`. Falls back to English. |
| | `info_synopsis_max` | `2000` | Maximum synopsis characters stored per game (200–2000). A memory limit, not a reading length. |
| | `info_scroll_step_ms` | `2000` | Synopsis auto-scroll speed, ms per line (200–8000). Higher is slower. |
| | `info_scroll_auto` | `true` | `false` shows the synopsis still and holds the panel for 15 s. |
| | `info_in_rotation` | `false` | GAME INFO as a third slide — see [The image screen](#the-image-screen). |
| `[images]` | `base_path`, `default_image` | `/cores`, `/cores/menu.jpg` | Where artwork lives on the card, and the default image. |
| | `core_image_timeout`, `system_image_timeout` | `30000`, `0` | Slide dwell times — see [The image screen](#the-image-screen). |
| | `image_mode` | `rotate` | `rotate`, `game` or `system`. |
| | `image_upscale`, `image_upscale_max` | `false`, `2.5` | Grow small artwork to fill the display area. |
| | `alphabetical_folders` | `true` | Organise images in lettered subfolders (`/cores/A/`, `/cores/B/`, …). |
| | `auto_download` | `true` | Download missing artwork from ScreenScraper. |
| | `max_image_size` | `500000` | Largest image (bytes) the display will download. |
| | `download_timeout` | `30000` | HTTP timeout (ms) for image downloads. |
| | `force_core_redownload`, `force_game_redownload` | `false` | Fetch again even when a cached file exists. Set for one boot, then back to `false`. |
| | `core_media_order`, `arcade_subsystem_media_order`, `arcade_media_order`, `game_media_order` | see above | Which image types are tried, and in what order — see [Artwork download order](#artwork-download-order). |
| `[ui]` | `flip_display` | `false` | Rotate the display 180°, touch included, for cases that hold the board upside down. |
| | `kiosk_mode`, `kiosk_hide_delay_ms` | `false`, `5000` | Artwork on the whole panel, footer on tap. |
| | `standby_screen`, `standby_when_offline`, `standby_offline_min`, `standby_idle_min`, `standby_brightness`, `standby_dim` | `clock`, `true`, `3`, `0`, `10`, `100` | See [Standby and clock](#standby-and-clock). |
| | `timezone`, `ntp_server`, `clock_24h` | blank, `pool.ntp.org`, `true` | The clock. |
| | `sound`, `sound_volume` | `true`, `128` | **Tab5 only.** |
| | `panel_rev` | `v1` | **Guition only.** `v2` for batch 2624 onward. |
| | `scroll_speed_ms` | `300` | Footer text scroll, ms per character step. Lower is faster. |
| | `scroll_pause_start_ms`, `scroll_pause_end_ms` | `2000`, `3000` | Pause before a scroll starts and before it resets. |
| | `web_config`, `web_password` | `true`, blank | See [Web interface](#web-interface). |
| `[debug]` | `debug` | `false` | Verbose output on the serial monitor. |

## RetroAchievements

RetroAchievements is configured **on the MiSTer**, not on the display's
microSD card: the server does the hashing and talks to the RA API, and the
display only renders what the server reports.

### Credentials

The first time the server starts it creates an editable template at:

```
/media/fat/Scripts/.config/mister_monitor/ra_credentials.ini
```

Open it and fill in your RetroAchievements username and your **Web API key**
(retroachievements.org → *Settings* → *Applications*):

```ini
[retroachievements]
username=YourRAUsername
api_key=YourWebAPIKey
```

An existing file is never overwritten by updates, so your configuration
survives every new server version. Without the file the RetroAchievements
page simply reports that it is not configured; everything else works as
before.

### What you get, tier by tier

Each tier is optional and degrades gracefully to the one below it:

| Setup | What the display shows |
|---|---|
| Credentials only, stock MiSTer | The game's achievement set, your cloud progress, points and hardcore breakdown, and the full trophy list — **view-only**: nothing you do on the MiSTer is recorded, and the page says so. |
| [odelot/Main_MiSTer](https://github.com/odelot/Main_MiSTer) RetroAchievements fork | Unlocks are **earned and recorded** while you play on `RA_`-prefixed cores. The display detects the fork and shows a READY FOR RETROACHIEVEMENTS banner when a matched game loads. |
| Fork + `debug=1` in `/media/fat/retroachievements.cfg` | Unlock popups become **instant** (under a second, with title and description), and CD systems the server cannot hash locally — PlayStation, Saturn, Mega CD — resolve through the fork's own hash. |

With the fork but without `debug=1`, unlocks still reach the display within a
few seconds through cloud polling; `log_file_entry=1` in `MiSTer.ini`
shortens that further. The fork's own installation and core setup are
documented in its repository.
