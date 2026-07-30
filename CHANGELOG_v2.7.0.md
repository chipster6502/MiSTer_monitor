# Changelog — v2.6.0 → v2.7.0

**104 commits**, 12–30 July 2026.
`git log v2.6.0..HEAD` · 93 files changed, 16 050 insertions, 1 015 deletions.

| Area | Files | Volume |
|---|---|---|
| Firmware (5 boards) | 21 | +8 960 / −679 |
| MiSTer server | 10 | +3 662 / −89 |
| Tooling (`tools/`) | 4 | +921 |
| CI workflows | 4 | +319 / −11 |
| Docs + README | 4 | +88 / −35 |
| SD card content | 10 | +11 |

**New files:** `ra_status.py`, `ra_hash.py`, `ra_credentials.ini.example`,
`audit_cores.py`, `check_server.py`, `propose_mapping.py`, `known_cores.json`,
`mister_types.h` (×2 renamed boards), three CI workflows, two licence texts,
`TRADEMARK.md`.
**New endpoints:** `/status/retroachievements`, `/status/unknown_cores`.
**New config key:** `info_in_rotation`.

---

## Contents

- [1. RetroAchievements](#1-retroachievements) — 27 commits
- [2. Neo Geo, Darksoft romsets and Neo Geo CD](#2-neo-geo-darksoft-romsets-and-neo-geo-cd) — 7
- [3. Core mapping and coverage](#3-core-mapping-and-coverage) — 23
- [4. Backwards-compatible cores](#4-backwards-compatible-cores) — 4
- [5. Artwork and detection edge cases](#5-artwork-and-detection-edge-cases) — 11
- [6. Game Info panel](#6-game-info-panel) — 3
- [7. Board ports and UI](#7-board-ports-and-ui) — 12
- [8. Ecosystem tooling (extracted)](#8-ecosystem-tooling-extracted) — 5
- [9. Licensing](#9-licensing) — 3
- [10. Naming and housekeeping](#10-naming-and-housekeeping) — 9
- [Observations](#observations)

---

## 1. RetroAchievements

The headline feature of the release, built in tiers over two weeks: server
resolver first, then the firmware page, then the live layer, then the states
that tell the user what is and is not being recorded.

### Server foundation (12–13 July)

| Commit | |
|---|---|
| `a28d3f3` | RetroAchievements progress endpoint with per-console hashing and on-demand index cache |
| `4a97f60` | Unlock polling, hardcore breakdown, corroborated `LastGameID` fallback |
| `60396c3` | Resolve `RA_`-prefixed cores from the RetroAchievements toolkit |
| `af98e3d` | Correct RA friendly-name table (Atari 7800, S32X) |
| `b6bae04` | Normalised RA core resolution, A78 hashing, Atari 2600 support |

### Live layer (14–15 July)

| Commit | |
|---|---|
| `3e678bf` | odelot log tailer, OSD trigger, instant unlock events |
| `4c6537a` | Trophy-list and event endpoints |
| `1a7fa07` | Achievement descriptions in the trophy-list payload |

Three tiers, each degrading to the one below: log tailer (sub-second, needs
`debug=1`) → OSD trigger (3–8 s) → 60 s cloud poll.

### Firmware page (12–15 July)

| Commit | |
|---|---|
| `10dc863` | RA page with live unlock popup (CYD28R) |
| `23433e7` | Same, CYD28R_2USB |
| `b8f875e` | Trophy subpages, 5 s event micro-poll, richer unlock popup |
| `d266228` | Service popup + counter poll above the screensaver early-return, so unlocks pop over fullscreen artwork |
| `f0ec72b` | Tap a trophy row to open a description overlay |
| `038ddd4` | Word-wrap unlock popup title and description instead of truncating |
| `a5b53a5` | READY FOR RETROACHIEVEMENTS footer banner on matched game load |

### States and honesty (25–27 July)

The subtlest work in the release: making the display say exactly what it
knows, instead of implying progress it cannot record.

| Commit | |
|---|---|
| `f9ac2f7` | SET NOT PUBLISHED YET when a resolved game has no achievement set |
| `891e6a2` | Corroborate titles across article transposition |
| `e569f31` | Only promise tracking on a core that actually records it |
| `a995752` | Ship an editable `ra_credentials.ini` instead of asking the user to create one |
| `2359568` | `forkLoadFailed` field in `RAStatus` |
| `67844f2` | romnom artwork lookup, view-only and no-set states |
| `14cb18e` | Detect when the fork has a different game loaded |
| `e82a3c4` | Extend view-only state to fork game mismatch |
| `b0ee2ef` | Clear the mismatch hash on entry so early returns cannot accuse |
| `c2f14e8` | Restore the SET NOT PUBLISHED YET branch, fix the unresolved copy |

### RA on Neo Geo

| Commit | |
|---|---|
| `5a69300` | Resolve Neo Geo games via arcade romset-name hashing |
| `8bae666` | Accept the `RA_` core prefix in the romset folder gate |

---

## 2. Neo Geo, Darksoft romsets and Neo Geo CD

A whole class of layouts that previously showed nothing, plus a second console
hiding behind the same core.

| Commit | |
|---|---|
| `17378c7` | Resolve NeoGeo romset ids, expose as `ss_romnom` |
| `c7f8d0c` | Send the server-resolved romnom to ScreenScraper; treat a 404 as definitive and skip the retry |
| `0f868e3` | Support unzipped Darksoft romset folders |
| `1232241` | Treat a Darksoft romset ZIP as the ROM itself |
| `ee6f763` | Show the title for bare-romset `.neo` files too |
| `1120d53` | Report Neo Geo CD discs as the Neo-Geo CD core |
| `ea538be` | Query Neo Geo CD discs as ScreenScraper system 70, with Neo Geo as the fallback |

The CD pair is worth a note. The NeoGeo core serves two consoles and writes
`NEOGEO` either way, so the disc side was invisible — yet a SAM rotation
already resolved it correctly, because SAM logs its own `neogeocd` mnemonic
and that maps straight to the CD console. Two fixes were needed to make a
manual load behave the same: the server names the console from the file
format, and the firmware corrects the queried system in `ssSystemForRom()`,
where the raw-first resolution chain would otherwise answer with the
cartridge platform.

---

## 3. Core mapping and coverage

Coverage stopped being a hand-maintained list and became a self-auditing
system.

### The engine

| Commit | |
|---|---|
| `4365953` | Map official cores using their own CONF_STR |
| `41f96a4` | (follow-up, 6 lines) |
| `9ea2b64` | Record unmappable cores, expose `/status/unknown_cores` (143 lines) |
| `582026b` | (follow-up, 2 lines) |
| `a5afc44` | Name the proposed core mappings |
| `0a55b75` | Resolve the unknown-core log through the same chain as the HUD |
| `ab051bf` | CI: validate `CORE_NAME_MAPPING` imports and parses on every PR |

### The weekly robot

| Commit | |
|---|---|
| `f85cab1` | Audit MiSTer cores, watch GitHub for unofficial ones |
| `b3dbde1` | Weekly ecosystem watch and mapping proposals |
| `9fca2f0` | Create the core-mapping label before opening the PR |
| `b3023fc` | Move the weekly jobs to Friday afternoon |
| `c7c134d`, `a53cdc1` | Merge bot PRs #8 and #10 |
| `47da1bf` | Update friendly names for various systems (bot) |

### Systems mapped this cycle

| Commit | |
|---|---|
| `368e1e5` | Unofficial z386 core → PC DOS |
| `5a05ffc` + `8971a62` | AmigaVision variants → Commodore Amiga / SS 64 |
| `0d64ee2` + `906486d` | Apple IIgs (+ added to `KNOWN_NON_ARCADE_SYSTEMS`) |
| `ef3a8c5` | Map and triage PapriumMD, Lynx 2P, MegaVGMDrive |
| `2924fa7` | Atari Lynx 2P → ScreenScraper 28, all boards |
| `675c88d` | Tomy Tutor, BK0011M, Jupiter Ace, Tamagotchi |
| `80e2b1f` | `KNOWN_NON_ARCADE_SYSTEMS` as a lowercase frozenset |

---

## 4. Backwards-compatible cores

A structural fix: the artwork and achievements now follow **the game's own
system**, not the core running it.

| Commit | |
|---|---|
| `a649348` | Resolve Atari 2600 games loaded through the 7800 core |
| `db72fb1` | Track the game's own system apart from the running core |
| `b6b7f69` | Query the predecessor system for backwards-compatible cores |
| `2909c0f` | Present Console (autoboot) boot ROMs as core-only |

---

## 5. Artwork and detection edge cases

| Commit | |
|---|---|
| `987ff48` | Skip name search for DOS container images |
| `94ba166` | Present container images as core-only |
| `bacbc6d` | Strip 0MHz variant markers and disc numbers from name-search queries |
| `afaf56e` | Skip CHD hashing on ao486 |
| `117e3c2` | Mark search exhausted on SS 404 to hide GAME INFO |
| `e2c79c0` | `names.txt` merge guard compares case-insensitively |
| `0204df6` | Evaluate SAM before the OSD-navigation guard (bundled four server fixes) |
| `6b8eddd` | UTF-8 output, SAM freshness, ACTIVEGAME gate, safety poll |
| `721edb7` | Guard SAM name-only entries, expose `no_rom_on_disk` |
| `d8c0ab4` | Run name search for server-confirmed name-only games, present its verdict |
| `4a581ef` | Map ScreenScraper system from raw CORENAME first |

---

## 6. Game Info panel

| Commit | |
|---|---|
| `97ada33` | Optional GAME INFO slide in the image rotation (`info_in_rotation`) |
| `d5d0934` | Clear page residency and restart the rotation clock on panel exit |
| `727b676` | Fold UTF-8 core/game names to ASCII for the GLCD display |

---

## 7. Board ports and UI

Every feature above was developed on CYD28R_ILI9341 and then ported.

| Commit | |
|---|---|
| `8b14f6e` | CYD28R_ST7789 synced with the ILI9341 baseline |
| `89384a4` | CYD35C/CYD35R: RA page, GAME INFO rotation, raw-first mapping, container/no-hash/romnom/no-rom handling |
| `be368cb` | Tab5: same feature set |
| `2fe173b` | Tab5: sidecar language stamp, stop ROM polling on no-hash, wire scroll keys, Amiga raw variants, media attempt counter |
| `2225dae` | CYD35: CRC-polling loop, forced ROM details path, media attempt counter |
| `36432ed` | Tab5: silence the expected footer-band clip in the JPEG callback |
| `c6ba930` | Tab5: stop the footer game name painting over the fullscreen nav strip |
| `a54a85f` | CYD28: achievement detail text at 1.5× |
| `9db7f59` | CYD28: trophy rows raised 9 px off the footer, touch bands recalibrated |
| `9685d6f` | Correct trophy-row touch calibration comments |
| `40a6c6d`, `e0dcb6f` | New gradient menu art, new generic arcade art |

---

## 8. Ecosystem tooling (extracted)

Built here, then moved to its own repositories to keep this one focused.

| Commit | |
|---|---|
| `c036a98` | Shared ecosystem taxonomy for the observatory |
| `e1c147a` | Historical GitHub backfill feeding the catalog |
| `deefd96` | Record backfill baseline and pending review queue |
| `8ecddbb` | Extract observatory → `MiSTer_ecosystem` |
| `6c4a943` | Move unofficial-cores discovery/distribution → `MiSTer_unofficial_cores` |

---

## 9. Licensing

| Commit | |
|---|---|
| `826b35e` | Relicense to GPL-3.0 (firmware) and AGPL-3.0 (server) |
| `5c18a28` | SPDX headers on sources |
| `ddd8962` | chipster6502 as copyright holder |

Releases up to and including v2.6.0 remain MIT.

---

## 10. Naming and housekeeping

| Commit | |
|---|---|
| `ceb6976` | Name the 2.8" sketches by panel controller, not USB port count |
| `1ab3546` | Document that the USB port count is a hint, not a rule |
| `15847f2` | Note that `0204df6` bundled four server fixes |
| `f24ce5b` | Refresh screenshots, add game info and RetroAchievements captures |
| `3ade437` | Update the screenshot galleries for both boards |
| `d603ec3` | Drop screenshots no longer shown in the README |
| `35a1c73` | Add CYD console core and game artwork captures |
| `ec8ee3e` | Reorder the CYD gallery, note the 2× upscale |
| `5b4147a` | Drop the CYD storage and arcade screenshots |

---

## Observations

Three things worth knowing before writing the release notes.

**Two pairs of commits share a message.** `9ea2b64`/`582026b`
("record unmappable cores…") and `4365953`/`41f96a4` ("map official cores using
their own CONF_STR") are not duplicates — in each pair the first is the
implementation (143 and 42 lines) and the second a small follow-up (2 and 6
lines) that reused the message. Harmless, but `git log --oneline` reads as if
work was done twice.

**One commit has no type prefix.** `c7f8d0c` starts at `(cyd28r):` with no
`feat`/`fix`. It is a feature. Any changelog generator keying off the
conventional prefix will drop it.

**The 2.8" sketch rename is a breaking change for source builders only.**
`mister_monitor_CYD28R` and `mister_monitor_CYD28R_2USB` became
`mister_monitor_CYD28R_ILI9341` and `mister_monitor_CYD28R_ST7789`
(`ceb6976`). Users of the web flasher are unaffected; anyone with a local
clone and a build script will need to update paths.
