# Configuration

All user configuration lives in a single file placed in the **root** of the
Tab5 microSD card: `/config.ini`. The sketch reads it at boot before
connecting to WiFi, so no credentials need to be hardcoded in the source.

Copy `config.ini` from the repository root to the SD card and fill in your
values.

## config.ini

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

## Static MiSTer IP address

The Tab5 connects to the MiSTer by IP, so a static address is required.
Edit `/etc/dhcpcd.conf` on the MiSTer and add:

```
interface eth0
static ip_address=192.168.0.XX/24
static routers=192.168.0.X
static domain_name_servers=192.168.0.X 8.8.8.8
```

Use `interface wlan0` instead of `eth0` for a wireless connection. The `/24`
netmask covers the most common home network setup; adjust if your router uses
a different subnet.

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

## Asset images

The repository includes a set of needed images in the `assets/` folder.
Copy them to the microSD card as follows:

- `frame01.jpg`, `frame02.jpg`, `logomister.jpg` and `menu.jpg` must be
  placed inside the `/cores/` folder.
- `Arcade.jpg` and `Arcade_75.jpg` must be placed inside `/cores/A/`.

Core and game images that are missing will be downloaded automatically from
ScreenScraper the first time that core/game is detected.
