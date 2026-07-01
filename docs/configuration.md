# Configuration

## SD Card content

The repository includes a ready-to-use microSD card layout under
`SD_card_content/`. Choose the subfolder that matches your hardware,
copy its contents to the **root** of your microSD card, and edit
`config.ini` with your credentials before first boot.

All other core and game images are downloaded automatically from
ScreenScraper the first time that core or game is detected.
Alphabetical subfolders (`/cores/B/`, `/cores/C/`, …) are created
on demand by the firmware — you do not need to create them manually.

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
; IP address of the MiSTer. LEAVE BLANK to auto-discover it on the LAN (recommended).
; Set it only as a fallback if your router blocks UDP broadcast.
ip=

[screenscraper]
ss_user=YOUR_SS_USERNAME
ss_pass=YOUR_SS_PASSWORD
; Advanced: only to use your own developer account instead of the built-in one
;ss_dev_user=
;ss_dev_pass=
```

Any key that is absent keeps the built-in default. The full list of available
keys is documented inside `config.ini` itself with comments explaining each
option.

## MiSTer IP address

The display locates the MiSTer automatically at boot via UDP broadcast —
no static IP is needed on the MiSTer and no value is required in `config.ini`.

If your router blocks UDP broadcast (uncommon in home networks), set a
fallback IP manually:

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
encounters. You can control which image types are tried and in what order via
keys in the `[images]` section of `config.ini`:

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
| `screenshot` | In-game title screenshot |
| `photo` | Real photograph of hardware or cartridge |
| `illustration` | Promotional illustration or poster |

**Region order within each token** — the `region=` key in `[screenscraper]`
controls which regional variant is tried first. The remaining regions follow
in fixed order, and the no-region generic variant is tried last. For example,
with `region=eu` and token `box3d` the sequence is:
`box-3D(eu)` → `box-3D(wor)` → `box-3D(us)` → `box-3D(jp)` → `box-3D`.

Default orders applied out of the box:

```ini
core_media_order=wheel-steel,wheel-carbon,wheel,photo,illustration,box3d,box2d,marquee,fanart,screenshot

arcade_subsystem_media_order=wheel-steel,wheel-carbon,wheel

arcade_media_order=fanart,marquee,wheel-carbon,wheel-steel,wheel,box3d,box2d,screenshot

game_media_order=box3d,box2d,wheel-carbon,wheel-steel,wheel,fanart,marquee,screenshot
```

