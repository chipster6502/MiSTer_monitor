#!/usr/bin/env python3
"""
MiSTer Status Server - COMPLETE OPTIMIZED VERSION
Simplified arcade detection logic with all original functions
"""

from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
import json
import os
import subprocess
import time
import glob
import re
import shutil
import hashlib
import zlib
import zipfile
import io
import socket
from urllib.parse import urlparse

# RetroAchievements status resolver (sibling module). Imported lazily-safe:
# if the file is missing the server still starts; the route reports the error.
try:
    from ra_status import (get_ra_status, start_ra_polling,
                           get_ra_event, get_ra_achievements)
    _RA_AVAILABLE = True
except Exception as _ra_e:
    _RA_AVAILABLE = False
    print(f"ℹ️ ra_status not loaded ({_ra_e}); /status/retroachievements disabled")
import queue

def _load_names_txt():
    """
    Reads /media/fat/names.txt and returns a dict {corename: friendly_name}.
    File format: CORENAME:          Friendly Name
    """
    names = {}
    try:
        with open('/media/fat/names.txt', 'r', encoding='utf-8', errors='ignore') as f:
            for line in f:
                line = line.strip()
                if ':' in line and not line.startswith('#') and not line.startswith('|'):
                    key, _, value = line.partition(':')
                    key = key.strip()
                    value = value.strip()
                    if key and value:
                        names[key] = value
        print(f"✅ names.txt loaded: {len(names)} entries")
    except FileNotFoundError:
        print("ℹ️ names.txt not found - this is normal; using raw core names.")
    except Exception as e:
        print(f"⚠️ Error reading names.txt: {e}")
    return names

NAMES_TXT = _load_names_txt()

# ---------------------------------------------------------------------------
# System constants — moved to module level for access by _update_state()
# ---------------------------------------------------------------------------

KNOWN_NON_ARCADE_SYSTEMS = [
    'nes', 'nintendo', 'famicom', 'snes', 'super nintendo', 'n64', 'nintendo64',
    'gameboy', 'gbc', 'gba', 'fds', 'sgb',
    'genesis', 'megadrive', 'sega', 'mastersystem', 'sms', 'gamegear', 'gg',
    'saturn', 'dreamcast', 'megacd', 'segacd', 's32x', 'sg1000',
    'psx', 'playstation', 'ps1',
    'atari2600', 'atari5200', 'atari7800', 'atarilynx', 'atari800', 'atarist',
    'colecovision', 'intellivision', 'vectrex', 'odyssey2', 'channelf',
    'astrocade', 'creativision', 'tutor', 'supervision', 'gamate', 'pokemonmini',
    'msx', 'msx1', 'msx2', 'msx2plus', 'x68000', 'pc8801', 'sharp', 'x1', 'pc88', 'mz',
    'turbografx16', 'pcengine', 'tgfx16', 'tgfx16cd', 'supergrafx',
    'wonderswan', 'wonderswancolor', 'ngp', 'ngpc',
    'gx4000', 'amstradcpc', 'amstrad', 'cpc6128', 'zx48', 'zxspectrum', 'zx81', 'zx80',
    'oric', 'bbcmicro', 'acorn', 'electron', 'archimedes', 'enterprise', 'samcoupe',
    'aquarius', 'microbee', 'atom', 'laser500',
    'vic20', 'c64', 'c128', 'c16', 'plus4', 'pet2001', 'ti99', 'trs80', 'coco', 'dragon', 'mc10',
    'trs80coco2', 'coleco', 'adam', 'apple2', 'applei', 'macplus',
    'svi318', 'fmtowns', 'amiga', 'ao486', 'pcxt', 'z386',
    'amiga', 'amigacd32', 'ao486', 'atari2600', 'atari5200', 'atari7800',
    'atarilynx', 'c64', 'fds', 'gb', 'gbc', 'gba', 'genesis', 'megacd',
    'n64', 'neogeo', 's32x', 'saturn', 'sms', 'snes', 'tgfx16', 'tgfx16cd',
    'psx', 'x68k',
    'APOGEE', 'ARCHIE', 'AY-3-8500', 'AcornElectron', 'Adam', 'Altair8800',
    'Amstrad PCW', 'BBCMicro', 'BK0011M', 'Casio_PV-2000', 'COCO3', 'CoCo2',
    'EDSAC', 'EpochGalaxyII', 'Galaksija', 'Interact', 'Laser', 'Lynx48', 'Lynx48/96K',
    'MultiComp', 'ORAO', 'Ondra_SPO186', 'Oric', 'PMD85', 'RX78', 'Sord M5',
    'SuperVision', 'TI-99_4A', 'TRS-80', 'TSConf', 'TatungEinstein',
    'TomyScramble', 'UK101', 'VECTOR06', 'Homelab', 'BBCBridgeCompanion',
    'PocketChallengeV2', 'MyVision', 'SuperVision8000', 'VT52', 'CreatiVision',
    'Atari2600', 'ATARI5200', 'ATARI7800', 'ATARI800', 'AtariST',
    'WonderSwan', 'WonderSwanColor', 'Saturn', 'FDS', 'SGB',
    'VECTREX', 'Coleco', 'Intellivision', 'ODYSSEY2', 'ChannelF',
    'Astrocade', 'Gamate', 'PokemonMini', 'SG1000', 'SG-1000', 'TomyTutor',
    'SCV', 'SuperGrafx', 'PDP1',
    'C64', 'C16', 'C128', 'VIC20', 'Amiga', 'AO486', 'PCXT', 'Amstrad',
    'Spectrum', 'ZX81', 'ZXNext', 'zx48', 'MSX', 'MSX1', 'X68000',
    'Apple-II', 'APPLE-I', 'MACPLUS', 'SAM', 'SAMCOUPE',
]

CORE_NAME_MAPPING = {
    'NES': 'Nintendo NES/Famicom',
    'SNES': 'Super Nintendo/Super Famicom',
    'N64': 'Nintendo 64',
    'FDS': 'Famicom Disk System',
    'GAMEBOY': 'Nintendo Game Boy',
    'GB': 'Nintendo Game Boy',
    'GBC': 'Nintendo Game Boy Color',
    'GBA': 'Nintendo Game Boy Advance',
    'GBA2P': 'Nintendo Game Boy Advance 2P',
    'SGB': 'Nintendo Super Game Boy',
    'GameNWatch': 'Nintendo Game & Watch',
    'GAMEBOY2P': 'Nintendo Game Boy Color',
    'Genesis': 'Sega Genesis/Mega Drive',
    'MegaDrive': 'Sega Genesis/Mega Drive',
    'SMS': 'Sega Master System',
    'GG': 'Sega Game Gear',
    'Saturn': 'Sega Saturn',
    'S32X': 'Sega Genesis/Megadrive 32X',
    'MegaCD': 'Sega Mega-CD',
    'SegaCD': 'Sega CD/Mega CD',
    'SG1000': 'Sega SG-1000',
    'GameGear': 'Sega Game Gear',
    'PSX': 'Sony PlayStation',
    'PlayStation': 'Sony PlayStation',
    'TurboGrafx16': 'TurboGrafx-16/PC Engine',
    'PCEngine': 'TurboGrafx-16/PC Engine',
    'TGFX16': 'TurboGrafx-16/PC Engine',
    'TGFX16-CD': 'TurboGrafx-16/PC Engine CD-Rom',
    'SuperGrafx': 'PC Engine SuperGrafx',
    'Atari2600': 'Atari 2600',
    'ATARI5200': 'Atari 5200',
    'ATARI7800': 'Atari 7800',
    'AtariLynx': 'Atari Lynx',
    'ATARI800': 'Atari 8bit',
    'AtariST': 'Atari ST/STE',
    'MAME': 'Arcade',
    'mame': 'Arcade',
    'Arcade': 'Arcade',
    'PET2001': 'Commodore PET',
    'C64': 'Commodore 64',
    'C128': 'Commodore 128',
    'VIC20': 'Commodore Vic-20',
    'Minimig': 'Commodore Amiga',
    'CD32': 'Amiga CD32',
    'AmigaCD32': 'Amiga CD32',
    'AO486': 'PC Dos',
    'PCXT': 'PC Dos',
    'PCjr': 'PC Dos',
    # z386: unofficial 80386 core by nand2mario. Not in the official
    # Distribution DB, so it can only be learned from users running it.
    # It shares games/AO486, so mapping it onto the existing 'PC Dos'
    # friendly name gives it artwork with no firmware change.
    'Z386': 'PC Dos',
    'Jupiter': 'Jupiter Ace',
    'PC8801': 'NEC PC-8801',
    'BK0011M': 'BK0011M',
    'eg2000': 'EG2000 Colour Genie',
    'lynx48': 'Camputers Lynx',
    'Lynx48': 'Camputers Lynx',
    'AQUARIUS': 'Mattel Aquarius',
    'sharpmz': 'SHARP MZ Series',
    'QL': 'Sinclair QL',
    'SPMX': 'Specialist MX',
    'SVI328': 'Spectravideo SVI-328',
    'AliceMC10': 'Alice 4K / Tandy MC-10',
    'MSX': 'MSX',
    'MSX1': 'MSX',
    'MSX2': 'MSX2 Computer',
    'MSX2Plus': 'MSX2+ Computer',
    'Spectrum': 'ZX Spectrum',
    'zx48': 'ZX Spectrum',
    'ZX81': 'ZX81',
    'ZXNext': 'ZX Spectrum Next',
    'Amstrad': 'Amstrad CPC',
    'AmstradCPC': 'Amstrad CPC',
    'GX4000': 'Amstrad GX4000',
    'Apple-II': 'Apple II',
    'APPLE-I': 'Apple I',
    'MACPLUS': 'Macintosh Plus',
    'X68000': 'Sharp X68000',
    'Coleco': 'Colecovision',
    'Intellivision': 'Intellivision',
    'VECTREX': 'Vectrex',
    'ODYSSEY2': 'Videopac G7000/Odyssey 2',
    'ChannelF': 'Channel F',
    'CreatiVision': 'CreatiVision',
    'SuperVision': 'Watara Supervision',
    'WonderSwan': 'WonderSwan',
    'WonderSwanColor': 'WonderSwan Color',
    'NGP': 'Neo Geo Pocket',
    'NGPC': 'Neo Geo Pocket Color',
    'PokemonMini': 'Pokemon Mini',
    'Gamate': 'Bit Corporation Gamate',
    'AVision': 'Adventure Vision',
    'Arcadia': 'Arcadia 2001',
    'CD-i': 'Philips CD-i',
    'MegaDuck': 'Mega Duck',
    'NEOGEO': 'Neo-Geo',
    'NeoGeo-CD': 'Neo-Geo CD',
    'NeoGeoPocket': 'Neo-Geo Pocket',
    'cdi':          'Philips CD-i',
    'colecovision': 'Colecovision',
    'jaguar':       'Atari Jaguar',
    'neogeocd':     'Neo-Geo CD',
    'tgfx16cd':     'TurboGrafx-16/PC Engine CD-Rom',
    'x68k':         'Sharp X68000',
    'Neo Geo MVS/AES': 'Neo-Geo',
    'Casio_PV-1000': 'Casio PV-1000',
    'VC4000': 'Interton VC 4000',
    'PocketChallenge': 'Pocket Challenge V2',
    'BBCMicro': 'BBC Micro',
    'AcornElectron': 'Acorn Electron',
    'ARCHIE': 'Acorn Archimedes',
    'AcornAtom': 'Acorn Atom',
    'TI-99_4A': 'TI-99/4A',
    'TRS-80': 'TRS-80 Color Computer',
    'COCO3': 'TRS-80 Color Computer 3',
    'CoCo2': 'TRS-80 Color Computer 2',
    'SAM': 'SAM Coupé',
    'SAMCOUPE': 'MGT SAM Coupé',
    'Oric': 'Oric 1 / Atmos',
    'nes': 'Nintendo NES/Famicom',
    'snes': 'Super Nintendo/Super Famicom',
    'genesis': 'Sega Genesis/Mega Drive',
    'megadrive': 'Sega Genesis/Mega Drive',
    'gameboy': 'Nintendo Game Boy',
    'gameboycolor': 'Nintendo Game Boy Color',
    'gameboyadvance': 'Nintendo Game Boy Advance',
    'nintendo64': 'Nintendo 64',
    'supernintendo': 'Super Nintendo',
    'playstation': 'Sony PlayStation',
    'commodore64': 'Commodore 64',
    'pcengine': 'TurboGrafx-16/PC Engine',
    'turbografx16': 'TurboGrafx-16/PC Engine',
    'mastersystem': 'Sega Master System',
    'atari2600': 'Atari 2600',
    'Adam': 'Coleco Adam',
    'Altair8800': 'Altair 8800',
    'APOGEE': 'Apogee BK-01 / Radio-86RK',
    'Apple-IIgs': 'Apple IIgs',
    'Apple-Lisa': 'Apple Lisa',
    'Arduboy': 'Arduboy',
    'Astrocade': 'Bally Astrocade',
    'BBCBridgeCompanion': 'BBC Bridge Companion',
    'C16': 'Commodore 16 - Plus/4',
    'Casio_PV-2000': 'Casio PV-2000',
    'Chess': 'Chess',
    'Chip8': 'SuperChip / Chip-8',
    'Donut': 'Donut',
    'Enterprise': 'Elan Enterprise',
    'FLAPPY': 'Flappy Bird',
    'Game and Watch': 'Nintendo Game & Watch',
    'GBMidi': 'Midi to Game Boy sound module',
    'GenMidi': 'Midi to Genesis sound module',
    'Interact': 'Interact Home Computer',
    'IQ151': 'IQ 151',
    'Laser': 'Vtech Laser 310',
    'MultiComp': 'MultiComp from Grant Searle',
    'MyVision': 'Nichibutsu My Vision',
    'Ondra SPO 186': 'Ondra SPO 186',
    'ORAO': 'Orao / Eagle',
    'PDP1': 'DEC PDP-1',
    'PMD85': 'Tesla PMD 85',
    'RX78': 'Bandai RX-78',
    'SlugCross': 'Slug Cross from bhayame',
    'SuperVision8000': 'Bandai Super Vision 8000',
    'TatungEinstein': 'Tatung Einstein TC01 & 256',
    'TK2000': 'TK 2000 Color Computer',
    'TomyTutor': 'Tomy Tutor / Pyuta / Pyuta Jr.',
    'TSConf': 'TSConf',
    'UK101': 'Compukit UK101',
    'VECTOR06': 'Vector-06C',
    'VT52': 'DEC VT52',
}

# names.txt fills in cores not already in CORE_NAME_MAPPING.
# Membership is tested CASE-INSENSITIVELY: CORE_NAME_MAPPING_LOWER (built
# below) collapses keys to lowercase with last-insert-wins, so a names.txt
# key differing only in case ('ao486' vs curated 'AO486') would silently
# replace the curated value on every lowercase lookup — which is the path
# SAM detection takes. The curated names are load-bearing: the firmware's
# ScreenScraper table is keyed on them, so letting a user label win here
# costs that core its artwork. names.txt keeps its legitimate job — naming
# cores the curated table has never heard of.
_curated_lower = {k.lower() for k in CORE_NAME_MAPPING}
for k, v in NAMES_TXT.items():
    if k.lower() not in _curated_lower:
        CORE_NAME_MAPPING[k] = v

# Set of all known system friendly names — used to detect CURRENTPATH = core name
KNOWN_SYSTEM_NAMES = set(v.lower() for v in CORE_NAME_MAPPING.values()) | \
                     set(v.lower() for v in NAMES_TXT.values())

# Case-insensitive lookup dict — keys are lowercased
CORE_NAME_MAPPING_LOWER = {k.lower(): v for k, v in CORE_NAME_MAPPING.items()}


# ---------------------------------------------------------------------------
# Unknown cores
# ---------------------------------------------------------------------------
# A core we cannot name shows its raw string and gets no artwork. Finding out
# has always meant waiting for a user to notice (z386 arrived via a forum post),
# and a scheduled scan of the distributions only half-solves it: it sees .rbf
# FILENAMES, which are not the CORENAME — ZX-Spectrum.rbf announces itself as
# 'Spectrum' — and it cannot see a manually-installed core at all, which is
# exactly what z386 is.
#
# So the running server records what it actually saw. That string IS the key a
# mapping needs: no inference, no guessing.
#
# Deliberately NOT telemetry. The file never leaves the MiSTer; the endpoint is
# there so a user can paste it into a report if they choose to. The counter and
# the timestamps exist to tell a core someone genuinely plays from one they
# opened once by accident.
# threading is imported again further down, next to the watcher threads; the
# duplicate is deliberate. This block runs at line ~250, that import lands at
# ~378, and a module-level Lock() here would raise NameError before the server
# ever answered a request. Re-importing is free (sys.modules is a cache).
import threading

_UNKNOWN_CORES_FILE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), 'unknown_cores.json')

_unknown_cores = {}                  # raw corename -> {first_seen, last_seen, count}
_unknown_cores_lock = threading.Lock()
_unknown_cores_loaded = False


def _load_unknown_cores():
    """Restore the log so counts survive a restart. Corruption is not fatal."""
    global _unknown_cores, _unknown_cores_loaded
    if _unknown_cores_loaded:
        return
    _unknown_cores_loaded = True
    try:
        with open(_UNKNOWN_CORES_FILE, 'r') as f:
            data = json.load(f)
        if isinstance(data.get('cores'), dict):
            _unknown_cores = data['cores']
            print(f"📋 Unknown-core log restored: {len(_unknown_cores)} entries")
    except FileNotFoundError:
        pass
    except Exception as e:
        # A truncated file must never stop the server from serving status.
        print(f"⚠️ unknown_cores.json unreadable ({e}) - starting a fresh log")


def _save_unknown_cores_locked():
    """
    Atomic write: temp file + os.replace.

    The MiSTer loses power by having its plug pulled, which is precisely when a
    half-written JSON would be left behind — and _load_unknown_cores() would
    then discard the whole history.
    """
    tmp = _UNKNOWN_CORES_FILE + '.tmp'
    try:
        with open(tmp, 'w') as f:
            json.dump({'comment': 'Cores this MiSTer ran that MiSTer Monitor '
                                  'could not name. Local only; nothing is sent '
                                  'anywhere. Safe to delete.',
                       'updated': int(time.time()),
                       'cores': _unknown_cores}, f, indent=1)
        os.replace(tmp, _UNKNOWN_CORES_FILE)
    except Exception as e:
        print(f"⚠️ Cannot persist unknown_cores.json: {e}")
        try:
            os.unlink(tmp)
        except Exception:
            pass


def _note_unknown_core(raw_corename):
    """
    Record a core name that resolves to nothing in CORE_NAME_MAPPING.

    Membership in the mapping is the test, NOT 'friendly == raw': several cores
    map to themselves ('MSX' -> 'MSX'), and comparing the strings would file
    those as unknown forever.

    Disk is touched only on a genuinely new name or once a minute, so a core
    left running cannot turn this into a write loop on the SD card.
    """
    name = (raw_corename or '').strip()
    if not name or name.upper() == 'MENU':
        return
    if name.lower() in CORE_NAME_MAPPING_LOWER or name in CORE_NAME_MAPPING:
        return

    now = int(time.time())
    with _unknown_cores_lock:
        _load_unknown_cores()
        entry = _unknown_cores.get(name)
        if entry is None:
            _unknown_cores[name] = {'first_seen': now, 'last_seen': now,
                                    'count': 1}
            print(f"❓ Unknown core recorded: '{name}' "
                  f"(no CORE_NAME_MAPPING entry) -> /status/unknown_cores")
            _save_unknown_cores_locked()
            return
        entry['count'] = entry.get('count', 0) + 1
        stale = now - entry.get('last_seen', 0) >= 60
        entry['last_seen'] = now
        if stale:
            _save_unknown_cores_locked()


def get_unknown_cores():
    """Payload for /status/unknown_cores."""
    with _unknown_cores_lock:
        _load_unknown_cores()
        cores = [
            {'corename': k,
             'first_seen': v.get('first_seen'),
             'last_seen': v.get('last_seen'),
             'count': v.get('count', 0)}
            for k, v in _unknown_cores.items()
        ]
    cores.sort(key=lambda c: (-(c['count'] or 0), c['corename'].lower()))
    return {
        'count': len(cores),
        'cores': cores,
        'note': ('Cores this MiSTer ran that could not be mapped to a friendly '
                 'name. These strings are the literal CORENAME and are exactly '
                 'what a mapping must be keyed on. Local only.'),
        'timestamp': int(time.time()),
    }

import threading

# ---------------------------------------------------------------------------
# Centralized state. All access must hold _state_lock.
# ---------------------------------------------------------------------------
_state_lock = threading.Lock()

# Serializes ROM-detail computation so a firmware retry can't start a second
# hash/CRC while the first is still running (the two would then fight over SD and
# CPU). A second caller blocks here, then picks up the cache the first one wrote.
_rom_details_compute_lock = threading.Lock()

_state = {
    'core':              'Menu',   # friendly name — used for display, image lookup, and ScreenScraper mapping
    'system_name':       'Menu',   # alias of 'core' (same value); kept for backward compatibility
    'game':              '',       # game name (filename without extension)
    'game_path':         '',       # absolute path to ROM file
    'is_arcade':         False,    # True if current core is arcade
    'rom_details':       None,     # last ScreenScraper result (dict or None)
    'rom_details_stale': True,     # True = needs refresh on next request
    'seq':               0,        # monotonic generation counter — bumps on every REAL identity change
    'updated_at':        0.0,      # epoch of last committed change
    'last_event':        'boot',   # 'boot' | 'load' | 'core' | 'menu' | 'sam'
}

# Raw CORENAME as seen at the last evaluation — a core change must always
# bypass the navigation gate (stale coupled nav timestamps would otherwise
# mask a core-only load).
_last_evaluated_corename = None

# Error tracking — exposed via /status/error_state and /status/all
server_error_state        = ''    # last error message, empty string if none
last_valid_core           = ''    # last corename that produced a valid state
last_valid_core_timestamp = 0.0   # epoch time of last valid state update

def _atari_78_or_26(game_path):
    """Real system of a game loaded through the Atari7800 core (plays both).
    .a26 -> 2600; .a78 -> 7800; else sniff the A78 header signature
    ('ATARI7800' at offset 1) when the file is directly readable; headerless
    dumps default to 2600 (real 7800 dumps virtually always carry the header).
    ZIP-internal paths fall through to the extension rules only."""
    p = game_path.lower()
    if p.endswith('.a26'):
        return 'Atari 2600'
    if p.endswith('.a78'):
        return 'Atari 7800'
    try:
        if os.path.isfile(game_path):
            with open(game_path, 'rb') as f:
                head = f.read(16)
            if len(head) >= 10 and head[1:10] == b'ATARI7800':
                return 'Atari 7800'
    except OSError:
        pass
    return 'Atari 2600'


def _commit_state(core, game, game_path, is_arcade, event):
    """
    Atomically commits a derived state. Bumps 'seq' and invalidates the
    rom-details cache ONLY when the identity actually changed, so a spurious
    re-evaluation can no longer wipe a hash computed for the same game.
    Returns True if the state changed.
    """
    with _state_lock:
        changed = (_state['core']      != core or
                   _state['game']      != game or
                   _state['game_path'] != game_path or
                   _state['is_arcade'] != is_arcade)
        if changed:
            _state['core']              = core
            _state['system_name']       = core
            _state['game']              = game
            _state['game_path']         = game_path
            _state['is_arcade']         = is_arcade
            _state['rom_details']       = None
            _state['rom_details_stale'] = True
            _state['seq']              += 1
            _state['updated_at']        = time.time()
            _state['last_event']        = event
        seq_now = _state['seq']
    if changed:
        print(f"✅ State committed (seq={seq_now}, {event}): core='{core}' game='{game}' arcade={is_arcade}")
    else:
        print(f"♻️ Evaluation confirmed current state (seq={seq_now}) — rom cache preserved")
    return changed

# ---------------------------------------------------------------------------
# Background watcher thread — monitors /tmp/ files via inotifywait
# ---------------------------------------------------------------------------
_WATCHED_FILES = [
    '/tmp/CORENAME',
    '/tmp/ACTIVEGAME',
    '/tmp/CURRENTPATH',
    '/tmp/FILESELECT',
    '/tmp/FULLPATH',
    '/tmp/STARTPATH',   # arcade ROM path — needed to detect arcade game changes
]

def _ensure_watched_files():
    """
    inotifywait aborts entirely if ANY watched path is missing. On setups
    without MiSTer Remote, /tmp/ACTIVEGAME (and others) may never be created,
    which traps the watcher in an endless restart loop and detection never
    runs. Create any missing target as an empty file so the watch can attach;
    MiSTer overwrites it with real content the moment it writes.
    """
    for path in _WATCHED_FILES:
        try:
            if not os.path.exists(path):
                open(path, 'a').close()
                print(f"📄 Created missing watch target: {path}")
        except Exception as e:
            print(f"⚠️ Could not create {path}: {e}")

def _is_known_non_arcade(corename):
    """Returns True if corename belongs to a known non-arcade system."""
    return (corename.lower() in [s.lower() for s in KNOWN_NON_ARCADE_SYSTEMS])


def _read_file(path):
    """Reads a /tmp/ file and returns its content stripped, or '' on error."""
    try:
        with open(path, 'r', encoding='utf-8', errors='ignore') as f:
            return f.read().strip()
    except:
        return ''


# ─────────────────────────────────────────────────────────────────────────────
# ROM-load contention guard
#
# Hashing a ROM reads it off the same SD/USB the core is loading from, so doing
# it while the core's "Loading" progress bar is still filling steals bandwidth
# and stutters the load. The core load is the MiSTer binary reading the file in a
# chunked read() loop (the progress bar tracks that loop), so we know the load
# has finished when that process stops reading.
#
# We watch /proc/<pid>/io 'rchar' (bytes read at the syscall level). Unlike
# 'read_bytes' (block layer only), 'rchar' also covers network-backed storage
# (CIFS/NAS), so this works regardless of where the user keeps their ROMs.
#
# Best-effort and fails OPEN: if the process or counter can't be read, it returns
# at once and hashing proceeds exactly as before. It never blocks detection,
# never holds a lock, and never raises.
# ─────────────────────────────────────────────────────────────────────────────
_LOAD_POLL_INTERVAL  = 0.25         # seconds between rchar samples
_LOAD_ACTIVITY_BYTES = 512 * 1024   # per-poll growth above this = "still loading"
_LOAD_QUIET_WINDOW   = 1.5          # seconds with no activity = load finished
_LOAD_INITIAL_GRACE  = 0.8          # if no activity seen by now, assume idle
_LOAD_MAX_WAIT       = 25.0         # hard cap (streaming cores never go quiet)


def _find_mister_pid():
    """Return the PID of the running MiSTer binary, or None. Fails soft."""
    try:
        out = subprocess.check_output(['pidof', 'MiSTer'],
                                      stderr=subprocess.DEVNULL, timeout=2)
        parts = out.decode(errors='ignore').strip().split()
        if parts:
            return int(parts[0])
    except Exception:
        pass
    # Fallback for environments without a working pidof: scan /proc/*/comm
    try:
        for entry in os.listdir('/proc'):
            if not entry.isdigit():
                continue
            try:
                with open(f'/proc/{entry}/comm', 'r') as f:
                    if f.read().strip() == 'MiSTer':
                        return int(entry)
            except Exception:
                continue
    except Exception:
        pass
    return None


def _read_mister_rchar(pid):
    """Return cumulative 'rchar' bytes from /proc/<pid>/io, or None."""
    try:
        with open(f'/proc/{pid}/io', 'r') as f:
            for line in f:
                if line.startswith('rchar:'):
                    return int(line.split(':', 1)[1].strip())
    except Exception:
        pass
    return None


def _wait_for_rom_load_to_settle():
    """
    Block until the MiSTer core has finished copying the ROM to SDRAM, so hashing
    doesn't compete with the load. Returns promptly when the system is idle, when
    the signal is unavailable, or after _LOAD_MAX_WAIT.
    """
    pid = _find_mister_pid()
    if pid is None:
        return  # can't observe -> proceed (same behaviour as before)

    prev = _read_mister_rchar(pid)
    if prev is None:
        return

    start         = time.monotonic()
    last_activity = start
    saw_activity  = False

    while True:
        time.sleep(_LOAD_POLL_INTERVAL)
        now = time.monotonic()

        cur = _read_mister_rchar(pid)
        if cur is None:
            return  # process gone / unreadable -> proceed

        if (cur - prev) > _LOAD_ACTIVITY_BYTES:
            saw_activity  = True
            last_activity = now
        prev = cur

        # Was loading, now quiet for the full window -> load finished.
        if saw_activity and (now - last_activity) >= _LOAD_QUIET_WINDOW:
            print(f"⏳ ROM load settled after {now - start:.1f}s — hashing now")
            return

        # Nothing was loading when we arrived -> don't wait the full window.
        if not saw_activity and (now - start) >= _LOAD_INITIAL_GRACE:
            return

        # Safety cap (e.g. streaming cores that never go quiet).
        if (now - start) >= _LOAD_MAX_WAIT:
            print(f"⏳ ROM load wait hit {_LOAD_MAX_WAIT:.0f}s cap — hashing anyway")
            return


def _get_mtime_ns(path):
    """Returns mtime in nanoseconds, or 0 on error."""
    try:
        return os.stat(path).st_mtime_ns
    except:
        return 0

def _sam_get_current():
    """
    Reads SAM_Games.log and returns (is_active, core, game, path).
    Format: "HH:MM:SS - corename - /full/path/to/game"
    Returns False tuple if log doesn't exist, is too old, or has no valid entry.
    """
    sam_log_path = '/tmp/SAM_Games.log'

    if not os.path.exists(sam_log_path):
        return False, '', '', ''

    age = time.time() - os.path.getmtime(sam_log_path)
    if age > 300:  # 5 minutes
        print(f"🔍 SAM_Games.log too old: {age:.1f}s")
        return False, '', '', ''

    try:
        with open(sam_log_path, 'r', encoding='utf-8', errors='ignore') as f:
            lines = f.readlines()
    except Exception:
        return False, '', '', ''

    for line in reversed(lines):
        line = line.strip()
        if not line:
            continue
        parts = line.split(' - ')
        if len(parts) >= 3:
            sam_core_raw = parts[1].strip()
            sam_path     = ' - '.join(parts[2:])
            game_filename = sam_path.split('/')[-1]
            sam_game      = os.path.splitext(game_filename)[0]
            sam_core      = (CORE_NAME_MAPPING.get(sam_core_raw) or
                             CORE_NAME_MAPPING_LOWER.get(sam_core_raw.lower()) or
                             sam_core_raw)
            sam_core      = CORE_NAME_MAPPING.get(sam_core, sam_core)
            print(f"✅ SAM detected — core='{sam_core}' game='{sam_game}'")
            return True, sam_core_raw, sam_core, sam_game, sam_path

    return False, '', '', '', ''


def _sam_is_current():
    """
    Returns True if SAM_Games.log is active AND is the most recent detection
    source (i.e. CORENAME/ACTIVEGAME are not significantly newer than the log).
    """
    sam_log_path = '/tmp/SAM_Games.log'
    if not os.path.exists(sam_log_path):
        return False

    sam_ts = os.path.getmtime(sam_log_path)
    grace  = 30  # seconds

    for fname in ['CORENAME', 'ACTIVEGAME']:
        try:
            fts = os.path.getmtime(f'/tmp/{fname}')
            if fts > sam_ts + grace:
                print(f"🔄 {fname} newer than SAM by {fts - sam_ts:.1f}s — SAM not current")
                return False
        except:
            pass

    return True

_KNOWN_ROM_EXTS = {
    '.zip', '.mra', '.mgl', '.rom', '.bin', '.iso', '.chd',
    # Nintendo
    '.nes', '.fds', '.nsf', '.sfc', '.smc', '.bs', '.spc',
    '.gba', '.gb', '.gbc', '.n64', '.z64', '.min',
    # Sega
    '.smd', '.md', '.gen', '.32x', '.sg', '.sms', '.gg',
    # Sony
    '.psx', '.exe',
    # NEC / Hudson
    '.pce', '.sgx',
    # SNK
    '.neo', '.ngp', '.ngc',
    # Atari
    '.a78', '.a52', '.a26', '.lnx', '.jag', '.j64',
    '.atr', '.xex', '.cas', '.car', '.atx', '.xfd',
    '.st', '.msa', '.stx', '.dim',
    # Other consoles / handhelds
    '.ws', '.wsc', '.pc2', '.col', '.cv', '.vec', '.ovr', '.int',
    '.sv', '.ch8', '.hex', '.gmc',
    # Commodore
    '.prg', '.d64', '.d81', '.t64', '.tap', '.crt', '.g64', '.reu',
    # Spectrum / SAM / Next
    '.tzx', '.z80', '.sna', '.trd', '.scl', '.csw', '.mgt',
    # Amstrad CPC
    '.cdt', '.cpc', '.voc',
    # Amiga / PC
    '.vhd', '.hdf', '.adf', '.adz', '.img', '.ima', '.vfd',
    # Apple
    '.do', '.po', '.2mg', '.nib', '.hdv',
    # MSX
    '.mx1', '.mx2', '.dsk', '.cue',
    # Japanese computers
    '.d88', '.ram',
    # British / misc micros
    '.ssd', '.dsd', '.ace', '.mdv', '.win', '.bas', '.lod',
    '.vz', '.caq', '.c10', '.ccc', '.cmd', '.jvi', '.m99',
    # Eastern-bloc / other computers
    '.c00', '.com', '.edd', '.fdd', '.rka', '.rkr', '.rks',
    '.rmm', '.odi', '.gam', '.cin', '.k7', '.p',
    # PDP-1
    '.pdp', '.rim',
}

# Containers whose CRC is never indexed by ScreenScraper (0MHz DOS packs
# build per-pack VHDs, so their CRCs exist in no database — and the guest OS
# rewrites them, so the CRC is unstable anyway). Extensible.
_NO_HASH_EXTS = {'.vhd'}

# Extensions worth hashing on MOST cores but not on specific ones. Keyed by the
# RAW CORENAME, never the friendly name: names.txt lets any user rename 'AO486'
# to whatever they like, and a friendly-name key would then miss silently.
#
# .chd on ao486: the 0MHz collection builds its own CHDs (64 of its 327 games
# mount one), so the container's CRC exists in no database and the search always
# falls through to the name query anyway. Hashing CRC32+MD5+SHA1 over a ~700 MB
# CHD costs ~35 s of DE10-Nano ARM — longer than the firmware's 12 s HTTP
# timeout, which is why CD-based DOS games visibly cycle through retries while
# .vhd games resolve instantly. RetroAchievements does not cover DOS, so nothing
# depends on that MD5 here.
#
# Deliberately NOT global: on console CD cores (PSX, Saturn, MegaCD, Neo Geo CD)
# the CHD's MD5 is exactly what resolves the RetroAchievements set. Putting
# '.chd' in _NO_HASH_EXTS would trade a DOS-only annoyance for broken RA across
# every CD core — reasoning by extension instead of by context, the same mistake
# that made 'boot' match 'Boot Camp'.
#
# .iso/.img on ao486 stay hashed on purpose: only 7 files in the collection, they
# are small, and a raw rip's CRC may legitimately be indexed by ScreenScraper.
_NO_HASH_EXTS_BY_CORE = {
    'AO486': {'.chd'},
}


def _read_corename_raw():
    """Raw CORENAME ('AO486'), RA_-prefix stripped. '' when unreadable."""
    try:
        with open('/tmp/CORENAME', 'r') as f:
            corename = f.read().strip()
    except Exception:
        return ''
    # RA_-prefixed cores: the RetroAchievements toolkit ships RA_SNES etc.
    return corename[3:] if corename.startswith('RA_') else corename


def _is_no_hash(ext, corename):
    """
    True when hashing this file can never yield a usable CRC/MD5.

    MUST be the single source of truth for both the hashing decision in
    get_rom_details_from_file() and the no_hash flag in _enrich_rom_result().
    If the two ever disagree, the server skips the hash while still reporting
    no_hash=false, and the firmware burns its full 5 x 20 s retry budget waiting
    for a CRC that is never coming.
    """
    ext = (ext or '').lower()
    if ext in _NO_HASH_EXTS:
        return True
    return ext in _NO_HASH_EXTS_BY_CORE.get(corename or '', frozenset())


# ---------------------------------------------------------------------------
# NeoGeo -> ScreenScraper romnom
# ---------------------------------------------------------------------------
# ScreenScraper indexes NeoGeo as MAME romsets ('mslug2.zip'). MiSTer runs .neo
# containers whose CRC exists in no database, so for this system the CRC route
# can never match — verified against gameid 37605, whose 37 rom entries are all
# .zip of 17-19 MB while the .neo on disk is 45 MB. Different universes.
#
# What actually resolves NeoGeo today is ScreenScraper's OWN fuzzy fallback on
# romnom: it strips '(mslug).neo' and matches the leftover title. That only
# works when the pack happens to name the file the way ScreenScraper does, and
# packs are inconsistent about it. All three observed on the same @MiSTer Pack:
#   'Metal Slug - Super Vehicle-001 (mslug).neo' -> hit  (gameid 37604)
#   'Metal Slug 2 (mslug2).neo'                  -> miss (SS calls it
#                                                   'Metal Slug 2 - Super Vehicle-001/ii')
#   'Metal Slug X (mslugx).neo'                  -> miss (HTTP 404)
# 48 of the 281 romsets carry a subtitle, so this is a systemic gap, not one
# unlucky game.
#
# The romset id is the one identifier that is always present and always right:
# it sits in the parentheses in pack layouts and IS the stem in Darksoft ones.
# Sending it as romnom matches exactly, with no CRC and no fuzzy search —
# verified: romnom=mslug2.zip returns gameid 37605 / rom id 21808.
_NEOGEO_CORENAMES = frozenset({'neogeo'})

# Installed by the MiSTer Downloader itself: |games/NEOGEO/romsets.xml is in the
# official Distribution DB (md5 9b5536a3b95bcd755a4904d34e55582d). Both files
# are read because a user may own either collection: romsets.xml describes the
# Darksoft pack, gog-romsets.xml the GOG/Humble one.
_ROMSET_XML_NAMES = ('romsets.xml', 'gog-romsets.xml')

_romset_cache = {}                    # directory -> (mtime_stamp, frozenset)
_romset_cache_lock = threading.Lock()


def _load_romset_names(directory):
    """
    Every romset id declared by the NeoGeo core's own data files.

    A 'name' may carry comma-separated aliases ('mslug3b6,mslug6',
    'burningfh,brningfh'); each alias is registered, since any of them can be
    the filename on disk.

    Cached per directory, invalidated by mtime: the file is 41 KB and would
    otherwise be re-parsed on every single game load.
    """
    if not directory or not os.path.isdir(directory):
        return frozenset()

    paths = [os.path.join(directory, n) for n in _ROMSET_XML_NAMES]
    try:
        stamp = tuple(os.path.getmtime(p) if os.path.isfile(p) else 0
                      for p in paths)
    except Exception:
        return frozenset()
    if not any(stamp):
        return frozenset()

    with _romset_cache_lock:
        cached = _romset_cache.get(directory)
        if cached and cached[0] == stamp:
            return cached[1]

    import xml.etree.ElementTree as ET
    names = set()
    for p in paths:
        if not os.path.isfile(p):
            continue
        try:
            root = ET.parse(p).getroot()
        except Exception as e:
            print(f"⚠️ {os.path.basename(p)} parse failed: {e}")
            continue
        for rs in root.iter('romset'):
            for alias in (rs.get('name') or '').split(','):
                alias = alias.strip().lower()
                if alias:
                    names.add(alias)

    frozen = frozenset(names)
    with _romset_cache_lock:
        _romset_cache[directory] = (stamp, frozen)
    print(f"ℹ️ NeoGeo romset ids loaded from {directory}: {len(frozen)}")
    return frozen


def _neogeo_games_dir(rom_path):
    """
    The directory holding romsets.xml for this ROM.

    The ROM can be nested — inside a pack ZIP, inside a 'World A-Z/' subfolder —
    while romsets.xml always sits at the games/NEOGEO root, so walk upwards from
    the ROM until a romset file appears. Bounded to 4 levels: covers every
    observed layout and stops the walk from wandering up to /media/fat.
    """
    if not rom_path:
        return None
    # A ZIP-internal path is not a filesystem path: the ZIP is the real entry.
    m = re.search(r'(.+\.zip)', rom_path, re.IGNORECASE)
    d = os.path.dirname(m.group(1) if m else rom_path)
    for _ in range(4):
        if not d or d == '/':
            break
        if any(os.path.isfile(os.path.join(d, n)) for n in _ROMSET_XML_NAMES):
            return d
        d = os.path.dirname(d)
    return None


def _neogeo_ss_romnom(rom_path, filename, corename):
    """
    ScreenScraper romnom for a NeoGeo ROM: '<romset>.zip', or '' when unknown.

    Two candidates, most confident first:
      1. the parenthesised group at the end of the stem —
         'Metal Slug 2 (mslug2).neo' — the pack layouts, unambiguous;
      2. the bare stem — 'mslug2.neo' — the Darksoft layout.

    Both are confirmed against the core's own romsets.xml before use, so a
    stray '(rev 1)' or an arbitrary filename can never be sent as a romset.
    When nothing validates, '' is returned and the firmware keeps its current
    behaviour: games that already resolve keep resolving.

    Prefix matching is deliberately NOT attempted. 'Metal Slug 2' prefixes both
    'mslug2' and 'mslug2t' (Metal Slug 2 Turbo); guessing between them is the
    very mistake _lookup_neogeo_romset already refuses to make.
    """
    if (corename or '').strip().lower() not in _NEOGEO_CORENAMES:
        return ''

    stem = os.path.splitext(os.path.basename(filename or ''))[0]
    if not stem:
        return ''

    candidates = []
    m = re.search(r'\(([^()]+)\)\s*$', stem)
    if m:
        candidates.append(m.group(1).strip())
    candidates.append(stem.strip())

    names = _load_romset_names(_neogeo_games_dir(rom_path))
    if not names:
        return ''

    for c in candidates:
        if c.lower() in names:
            romnom = c.lower() + '.zip'
            print(f"🎯 NeoGeo romset resolved: '{stem}' -> romnom={romnom}")
            return romnom

    print(f"ℹ️ NeoGeo: no romset id confirmed for '{stem}' - using filename")
    return ''

# --- Container-image denylist -------------------------------------------------
# A DOS .vhd usually holds an entire environment or a multi-game compilation,
# not a single title. jeuRecherche has no notion of "no match": it returns the
# best fuzzy hit for whatever string it gets, so 'boot' yields a real game id
# (observed: 170580) and the firmware shows wrong artwork with every appearance
# of success. No similarity threshold fixes this — the query itself denotes no
# game. This is NOT an extension check: 0MHz ships one .vhd per game with a real
# title ('Prince of Persia (1990).vhd'), where name search is exactly right.
#
# All matching happens AFTER _clean_search_name(), against a name that is
# lowercased and has -/_/whitespace collapsed to single spaces, so one entry
# covers 'top-300', 'Top 300' and 'top_300'.

# Whole-name matches: the cleaned name IS the container, no suffix possible.
GENERIC_MEDIA_NAMES = {'hdd', 'harddisk', 'system'}

# OS designators that may qualify a bare container marker: 'BOOT-DOS98'.
_OS_TOKEN = r'(?:(?:ms)?dos ?\d*(?:\.\d+)?|win ?(?:31|3\.1|95|98|me|xp)|w9[58])'

# Bare container names, optionally qualified by an OS designator.
# 'boot' is deliberately NOT a free prefix: 'Boot Camp' is a real DOS title, so
# only 'boot' alone or 'boot <os>' counts. Asymmetric cost drives this — a false
# positive silently kills artwork for a real game forever, a false negative
# merely leaves the existing bug on a name we have not seen yet. So 'BOOT-386'
# or 'BOOT-Games' would escape by design: widen only with a filename in hand.
GENERIC_MEDIA_RE = re.compile(
    r'^(?:hdd?\d*|disk\d*|drive ?[a-z]|' + _OS_TOKEN + r'|boot(?: ' + _OS_TOKEN + r')?)$',
    re.I,
)

# Leading-marker matches: a collection marker plus a per-distribution builder or
# variant suffix — 'Shareware Pack-fbit', 'Top 300 DOS Games'. Chasing the
# literals is hopeless (every builder picks a different suffix), so match the
# whole leading marker. Never mid-name: 'Doom Shareware' is a real game and must
# still reach the search. Unlike 'boot', these are implausible as the START of a
# real title, which is what makes free-prefix matching safe here.
GENERIC_MEDIA_PREFIXES = ('shareware', 'top 300')


def is_generic_media_name(stem):
    """True when the cleaned name identifies a container image, not a game."""
    s = re.sub(r'[-_\s]+', ' ', (stem or '').strip().lower()).strip()
    if not s:
        return False
    if s in GENERIC_MEDIA_NAMES or GENERIC_MEDIA_RE.match(s):
        return True
    return any(s == p or s.startswith(p + ' ') for p in GENERIC_MEDIA_PREFIXES)


# 0MHz glues variant markers onto the stem as pseudo-extensions that splitext()
# does not remove, so they survive into the query and guarantee a jeuRecherche
# miss on a game that IS in the database — the mirror image of the container
# denylist: a false negative on a real game, not wrong art on a non-game.
# Censused over all 387 image names in the 0mhz-net/0mhz-collection MGLs:
#   .mt32  x87   Roland MT-32 audio build
#   .r2/.r3/.r4  x35 total   setup revision
# No other dotted markers exist there. They stack in EITHER order, hence the
# loop: 'cannon fodder 1.r2.mt32' and 'day of the tentacle.mt32.r2' both occur.
# The revision marker is restricted to a dot separator (that is how the whole
# collection writes it); allowing ' r2' would risk eating a real title's tail.
_VARIANT_RE = re.compile(
    r'(?:'
    r'[.\-_ ]+(?:mt32|mt-32)'   # audio build; separator varies in community packs
    r'|\.r\d+'                  # setup revision; always dotted
    r')$',
    re.I,
)

# A trailing '-<n>' is a CD disc number, not part of the title: '7th guest-1.chd'
# is disc 1 of The 7th Guest, and querying '7th guest-1' misses. Two guards, both
# earned by counterexamples rather than intuition:
#   * at most 2 digits — 'top-300' is a compilation marker the container denylist
#     needs intact, not a disc; real discs are single-digit in the whole corpus.
#   * the remainder must be more than one word — a bare 'F-15.vhd' would otherwise
#     be decapitated to 'F', the same catastrophic false positive as 'Boot Camp'.
# Safe because the collection numbers SERIES with a space ('dune 2', 'doom 2' —
# 98 of them) and NEVER with a hyphen; the only two hyphen-numbered names in the
# whole set are '7th guest-1' and 'thunder in paradise-1', both discs.
_DISC_RE = re.compile(r'(.*)-\d{1,2}$')


def _strip_disc_number(base):
    """Removes a trailing CD disc number when it cannot be part of the title."""
    mo = _DISC_RE.match(base)
    if not mo:
        return base
    title = mo.group(1)
    return title if ' ' in title else base   # one-word remainder: keep, 'F-15' stays


def _strip_variant_markers(base):
    """Removes stacked trailing variant markers ('.r2.mt32') from a file stem."""
    while True:
        stripped = _VARIANT_RE.sub('', base)
        if stripped == base:      # the pattern needs >=1 separator char, so every
            return base           # successful sub shortens: no infinite loop
        base = stripped


def _clean_search_name(name):
    """
    Derives a ScreenScraper text-search query from a game/file name:
    strips extension, variant markers, disc numbers, bracketed tags and ALL
    parenthesised groups, then collapses separators.
      'Prince of Persia (1990)(Broderbund).vhd' -> 'Prince of Persia'
      '7th guest-1.mt32.chd'                    -> '7th guest'
      'cannon fodder 1.r2.mt32.vhd'             -> 'cannon fodder 1'
      'Doom_[0MHz].mgl'                         -> 'Doom'
    Recall beats precision here: jeuRecherche matches best on bare titles.
    Validated against the real 0mhz-dos item listing on archive.org.
    """
    base = os.path.splitext(os.path.basename(name or ''))[0]
    base = _strip_variant_markers(base)        # '.mt32', '.r2' audio/revision siblings
    base = _strip_disc_number(base)            # '-1' CD disc number
    base = re.sub(r'\[[^\]]*\]', '', base)     # [tags]
    base = re.sub(r'\([^)]*\)', '', base)      # (Year)(Publisher)(Region)
    base = base.lstrip('~ ')                   # 0MHz marks broken setups with a leading '~'
    base = base.replace('_', ' ')
    base = re.sub(r'\s{2,}', ' ', base).strip(' -.')
    return base

def _enrich_rom_result(result):
    """
    Adds name-search metadata to a rom-details result (success OR failure):
      search_name      — clean title for jeuRecherche.php. Always populated:
                         the firmware displays it even when it must not search.
      no_hash          — True when the CRC can NEVER arrive (unindexable,
                         mutable container). Distinct from hash_calculated,
                         which is also False while a hash is still in flight —
                         an ambiguity that costs the firmware 5x20s of pointless
                         retries on every .vhd load.
      ss_romnom        — ScreenScraper romnom override ('mslug2.zip'). Only
                         populated when a romset id could be confirmed against
                         the core's own data files; '' means "use the filename",
                         i.e. exactly today's behaviour.
      container_image  — True when search_name denotes a whole-environment or
                         compilation image rather than a game ('boot',
                         'BOOT-DOS98', 'Shareware Pack-fbit'). The firmware must
                         never text-search these: jeuRecherche has no "no match"
                         and returns a fuzzy hit (observed: id 170580 for
                         'boot'), i.e. confident-looking wrong artwork.
      name_search_hint — True when the CRC route cannot work: no ROM resolvable,
                         no CRC computed, or an unindexed container.

    These two flags are deliberately independent. A DOS game with a valid but
    unindexed CRC (pack-built CHDs) has container_image=False and hint=False,
    and must STILL reach the text search after the CRC path misses twice —
    conflating them into one boolean would silently kill that path.
    """
    with _state_lock:
        game_for_name = _state['game']
        path_for_name = _state['game_path']

    ext = os.path.splitext(result.get('path') or path_for_name or '')[1].lower()
    search_name = _clean_search_name(result.get('filename') or game_for_name)
    corename_raw = _read_corename_raw()
    no_hash = _is_no_hash(ext, corename_raw)

    result['search_name']      = search_name
    result['no_hash']          = bool(no_hash)
    result['container_image']  = bool(is_generic_media_name(search_name))
    result['ss_romnom']        = _neogeo_ss_romnom(
        result.get('path') or path_for_name,
        result.get('filename'),
        corename_raw)
    result['name_search_hint'] = bool(
        (not result.get('available')) or
        (not result.get('crc32')) or
        no_hash
    )
    return result

def _game_name_from_path(path):
    """
    Extracts game name from a file path.
    Only strips the extension if it is a known ROM extension.
    Avoids stripping version suffixes like '.000' or '.001'.
    """
    base = os.path.basename(path)
    ext  = os.path.splitext(base)[1].lower()
    return os.path.splitext(base)[0] if ext in _KNOWN_ROM_EXTS else base

def _update_state():
    """
    Reads /tmp/ files and updates _state.
    Called by the watcher thread on every relevant filesystem event.
    """
    corename    = _read_file('/tmp/CORENAME')
    activegame  = _read_file('/tmp/ACTIVEGAME')
    currentpath = _read_file('/tmp/CURRENTPATH')
    fullpath    = _read_file('/tmp/FULLPATH')

    # --- Navigation vs real load (tolerant gate) ---
    # Empirical MiSTer behaviour: during OSD navigation FILESELECT and
    # CURRENTPATH are written back-to-back (sub-millisecond apart); on a real
    # load only FILESELECT is touched.
    # A tolerance window separates the two cases robustly: coupled writes are
    # microseconds apart, a human cursor-move followed by Enter is >= ~100 ms.
    global _last_evaluated_corename

    fs_ns = _get_mtime_ns('/tmp/FILESELECT')
    cp_ns = _get_mtime_ns('/tmp/CURRENTPATH')
    ag_ns = _get_mtime_ns('/tmp/ACTIVEGAME')

    _NAV_COUPLING_MS = 50.0
    delta_ms = abs(fs_ns - cp_ns) / 1e6

    core_changed      = (corename != _last_evaluated_corename)
    activegame_recent = (time.time_ns() - ag_ns) <= 3_000_000_000  # explicit launch (Remote/Zaparoo)

    if delta_ms <= _NAV_COUPLING_MS and not core_changed and not activegame_recent:
        print(f"🔀 OSD navigation detected (Δ={delta_ms:.2f} ms) — state unchanged")
        return

    _last_evaluated_corename = corename

    # --- SAM detection (takes priority if active and current) ---
    if _sam_is_current():
        sam_active, sam_core_raw, sam_core_friendly, sam_game, sam_path = _sam_get_current()
        if sam_active and sam_core_raw:
            print(f"🎮 SAM active — core='{sam_core_friendly}' game='{sam_game}'")
            _commit_state(sam_core_friendly, sam_game, sam_path,
                          is_arcade=False, event='sam')
            return

    # --- Menu ---
    if not corename or corename.upper() == 'MENU':
        print("📋 MENU detected")
        _commit_state('Menu', '', '', is_arcade=False, event='menu')
        return

    # --- Resolve friendly core name ---
    # RA_-prefixed cores: the RetroAchievements toolkit (odelot fork via
    # MiSTer Companion) loads adapted cores through MGLs whose <setname>
    # prefixes the stock name with 'RA_' (RA_SNES, RA_MegaDrive, ...).
    # Strip the prefix for the lookup so they resolve exactly like their
    # stock counterparts (friendly name, images, ScreenScraper mapping,
    # RA panel). The raw corename keeps flowing to the arcade/ACTIVEGAME
    # logic below, which already handles it.
    lookup_name = corename[3:] if corename.startswith('RA_') else corename
    friendly_name = (CORE_NAME_MAPPING.get(lookup_name) or
                    CORE_NAME_MAPPING_LOWER.get(lookup_name.lower()) or
                    lookup_name)
    friendly_name = CORE_NAME_MAPPING.get(friendly_name, friendly_name)

    if friendly_name == corename and '-' in corename:
        prefix = corename.split('-', 1)[0]
        prefix_friendly = (CORE_NAME_MAPPING.get(prefix) or
                           CORE_NAME_MAPPING_LOWER.get(prefix.lower()))
        if prefix_friendly:
            print(f"🔧 MGL prefix '{prefix}' resolved to core '{prefix_friendly}'")
            friendly_name = prefix_friendly

    # --- Arcade detection ---
    ARCADE_FRESHNESS = 30  # seconds
    corename_ts   = _get_mtime_ns('/tmp/CORENAME') / 1e9
    activegame_ts = _get_mtime_ns('/tmp/ACTIVEGAME') / 1e9
    startpath_ts  = _get_mtime_ns('/tmp/STARTPATH') / 1e9

    activegame_arcade_fresh = (
        activegame and
        '/_Arcade/' in activegame and
        activegame_ts >= corename_ts - ARCADE_FRESHNESS
    )

    # STARTPATH points at the launched .mra for arcade cores. The .mra
    # extension is arcade-exclusive and independent of the launch folder,
    # so this catches arcades started from _@Favorites, custom folders,
    # etc. — cases where FULLPATH doesn't contain "arcade". Freshness is
    # checked against CORENAME so a stale STARTPATH from a previous arcade
    # session doesn't misclassify a console game loaded afterwards.
    startpath = ''
    try:
        with open('/tmp/STARTPATH', 'r') as f:
            startpath = f.read().strip()
    except Exception:
        pass

    startpath_arcade_fresh = (
        startpath.lower().endswith('.mra') and
        startpath_ts >= corename_ts - ARCADE_FRESHNESS
    )

    is_arcade = False
    game_name = ''
    game_path = ''

    if activegame_arcade_fresh:
        # Arcade launched via Remote — use ACTIVEGAME
        is_arcade = True
        game_name = _game_name_from_path(activegame)
        game_path = activegame
        print(f"🕹️ Arcade (Remote launch): {game_name}")

    elif startpath_arcade_fresh:
        # Arcade launched via OSD — detected by the .mra in STARTPATH,
        # works regardless of the launch folder (_Arcade, _@Favorites, …).
        is_arcade = True
        game_name = _game_name_from_path(startpath)
        game_path = startpath
        print(f"🕹️ Arcade (OSD .mra launch): {game_name}")

    elif fullpath and 'arcade' in fullpath.lower() and not _is_known_non_arcade(corename):
        # Arcade launched via OSD (legacy path-based detection, kept as fallback)
        is_arcade = True
        game_name = _game_name_from_path(currentpath)
        game_path = currentpath
        print(f"🕹️ Arcade (OSD launch): {game_name}")

    else:
        # Non-arcade — prefer ACTIVEGAME, fall back to CURRENTPATH
        
        cp_ext = os.path.splitext(currentpath)[1].lower() if currentpath else ''
        currentpath_is_core_name = (
            currentpath and
            cp_ext not in _KNOWN_ROM_EXTS and
            (
                currentpath.lower() in KNOWN_SYSTEM_NAMES or
                (
                    '(' not in currentpath and
                    (
                        currentpath == '..' or
                        currentpath.startswith('_@') or
                        currentpath.lower() == corename.lower() or
                        currentpath.lower() == friendly_name.lower() or
                        currentpath.lower().replace(' ', '').replace('/', '') == corename.lower() or
                        currentpath.lower() in friendly_name.lower() or
                        friendly_name.lower().endswith(currentpath.lower()) or
                        corename.lower() in currentpath.lower().replace(' ', '').replace('+', '')
                    )
                )
            )
        )
        
        if currentpath_is_core_name:
            # Core loaded without a game — clear game state
            game_name = ''
            game_path = ''
            print(f"🎮 Non-arcade: core={corename} loaded without game (CURRENTPATH='{currentpath}')")
        elif activegame and not activegame.lower().endswith('.ini'):
            game_name = _game_name_from_path(activegame)
            game_path = activegame
        elif currentpath and not currentpath.lower().endswith('.ini'):
            game_name = _game_name_from_path(currentpath)
            game_path = currentpath
        else:
            game_name = ''
            game_path = ''
        
        print(f"🎮 Non-arcade: core={corename} game={game_name}")

    # Atari7800 core plays both 2600 and 7800: resolve the game's real system
    # so artwork (ScreenScraper 26 vs 41) and the RA panel key off the game,
    # not the core. Core-only (no game) stays 'Atari 7800'.
    if not is_arcade and friendly_name == 'Atari 7800' and game_path:
        friendly_name = _atari_78_or_26(game_path)

    # Arcade is excluded on purpose: those cores are addressed by .mra and
    # are deliberately absent from CORE_NAME_MAPPING, so logging them would
    # bury the real finds under 160 false positives.
    if not is_arcade:
        _note_unknown_core(lookup_name)

    _commit_state('Arcade' if is_arcade else friendly_name,
                  game_name, game_path, is_arcade,
                  event='load' if game_name else 'core')

_SETTLE_SECONDS      = 0.4   # quiet time after the last event before evaluating
_SAFETY_POLL_SECONDS = 15.0  # idle re-check; heals watcher restarts / lost events

_event_queue = queue.Queue()

def _watcher_thread():
    """
    Runs inotifywait in monitor mode and feeds raw events into _event_queue.
    Restarts automatically if inotifywait dies unexpectedly.
    """
    print("👁️ Watcher thread started")
    while True:
        _ensure_watched_files()
        try:
            proc = subprocess.Popen(
                ['inotifywait', '-m', '-e', 'close_write,create'] + _WATCHED_FILES,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )
            for line in proc.stdout:
                line = line.strip()
                if not line:
                    continue
                print(f"📂 inotify event: {line}")
                _event_queue.put(line)
            proc.wait()
            err = (proc.stderr.read() or '').strip()
            if err or proc.returncode not in (0, None):
                print(f"⚠️ inotifywait exited (code={proc.returncode}): {err or 'no stderr'}")
        except Exception as e:
            print(f"⚠️ Watcher thread error: {e}")
        print("🔄 Watcher thread restarting...")
        time.sleep(1)

def _evaluator_thread():
    """
    Consumes events and calls _update_state() exactly once per settled burst.
    The last event of a burst (the real game load) can never be lost.
    A low-frequency safety poll re-checks FILESELECT while idle, so a missed
    inotify event (watcher restart, rare edge) can never freeze the state
    permanently — the design guarantees eventual convergence.
    """
    print("🧠 Evaluator thread started")
    pending = False
    last_evaluated_fs_ns = 0
    while True:
        timeout = _SETTLE_SECONDS if pending else _SAFETY_POLL_SECONDS
        try:
            _event_queue.get(timeout=timeout)
            pending = True          # burst open/extended — wait for quiet
            continue
        except queue.Empty:
            pass                    # timeout: burst settled, or idle tick

        if pending:
            pending = False
            _update_state()
            last_evaluated_fs_ns = _get_mtime_ns('/tmp/FILESELECT')
        else:
            # Idle safety net: FILESELECT moved but was never evaluated
            fs_ns = _get_mtime_ns('/tmp/FILESELECT')
            if fs_ns > last_evaluated_fs_ns:
                print("🛟 Safety poll: unevaluated FILESELECT change — evaluating")
                _update_state()
                last_evaluated_fs_ns = fs_ns

# --- MiSTer Monitor UDP discovery responder -------------------------------
DISCOVERY_PORT    = 51234
DISCOVERY_REQUEST = b"MMON_DISCOVER_V1"
DISCOVERY_REPLY   = b"MMON_SERVER_V1:8081"   # advertise the HTTP port too

def _start_discovery_responder():
    """
    Lets the display find this server with no hardcoded IP.
    The display broadcasts DISCOVERY_REQUEST; we reply (unicast) with
    DISCOVERY_REPLY directly to the sender, which reads our address
    from the reply's source IP.
    """
    def _run():
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind(('', DISCOVERY_PORT))
        except OSError as e:
            print(f"Discovery responder: cannot bind UDP {DISCOVERY_PORT}: {e}")
            return
        print(f"Discovery responder listening on UDP {DISCOVERY_PORT}")
        while True:
            try:
                data, addr = sock.recvfrom(64)
                if data.strip() == DISCOVERY_REQUEST:
                    sock.sendto(DISCOVERY_REPLY, addr)
                    print(f"Discovery: replied to {addr[0]}")
            except Exception as e:
                print(f"Discovery responder error: {e}")
                time.sleep(1)

    threading.Thread(target=_run, daemon=True).start()

def _start_watcher():
    """Starts the watcher (inotify producer) and evaluator (consumer) daemons."""
    threading.Thread(target=_watcher_thread, daemon=True).start()
    threading.Thread(target=_evaluator_thread, daemon=True).start()

# Session tracking — module-level so they persist across handler instances
_session_start   = time.time()
_requests_count  = 0

class MiSTerStatusHandler(BaseHTTPRequestHandler):

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)

    def _is_ini_file(self, file_path):
        """
        Check if the file is an .ini file that should be ignored as a game
        .ini files are configuration files, not games
        """
        if not file_path:
            return False
        
        # Simply check if it's an .ini file
        if file_path.lower().endswith('.ini'):
            filename = os.path.basename(file_path)
            print(f"🚫 Detected .ini configuration file: '{filename}' - ignoring for game detection")
            return True
        
        return False


    def do_GET(self):
        """Handle GET requests"""
        global _requests_count
        _requests_count += 1
        parsed_path = urlparse(self.path)
        path = parsed_path.path
        
        # Main endpoints
        if path == '/' or path == '/status':
            self.send_index_page()
        elif path == '/status/core':
            self.send_text_response(self.get_current_core())
        elif path == '/status/game':
            self.send_text_response(self.get_current_game())
        elif path == '/status/rom':
            self.send_text_response(self.get_current_rom())
        elif path == '/status/system':
            self.send_json_response(self.get_system_info())
        elif path == '/status/storage':
            self.send_json_response(self.get_storage_info())
        elif path == '/status/usb':
            self.send_json_response(self.get_usb_info())
        elif path == '/status/network':
            self.send_json_response(self.get_network_stats())
        elif path == '/status/session':
            self.send_json_response(self.get_session_stats())
        elif path == '/status/unknown_cores':
            # Cores this server could not name. Read by a human (or pasted into
            # an issue) to turn a "shows the raw name, no artwork" report into
            # the exact key to add. Local only — nothing is ever sent anywhere.
            self.send_json_response(get_unknown_cores())
        elif path == '/status/retroachievements':
            if _RA_AVAILABLE:
                self.send_json_response(get_ra_status(self))
            else:
                self.send_json_response({'enabled': False,
                                         'status': 'module_unavailable',
                                         'timestamp': int(time.time())})
        elif path == '/status/retroachievements/event':
            # ~60-byte payload for the firmware's 5 s micro-poll: just the
            # monotonic unlock counter (bumped <1 s after a real unlock when
            # the odelot debug-log tailer is active) plus the tail flag.
            if _RA_AVAILABLE:
                self.send_json_response(get_ra_event())
            else:
                self.send_json_response({'event_counter': 0,
                                         'status': 'module_unavailable',
                                         'timestamp': int(time.time())})
        elif path == '/status/retroachievements/achievements':
            # Flat paginated trophy list for the firmware subpages. Served
            # from the progress cache — zero extra RA API calls per page.
            if _RA_AVAILABLE:
                from urllib.parse import parse_qs
                q = parse_qs(parsed_path.query)
                ra_page = q.get('page', ['1'])[0]
                ra_per  = q.get('per',  ['6'])[0]
                self.send_json_response(
                    get_ra_achievements(self, ra_page, ra_per))
            else:
                self.send_json_response({'status': 'module_unavailable',
                                         'timestamp': int(time.time())})
        elif path == '/status/rom/details':
            from urllib.parse import parse_qs
            force = parse_qs(parsed_path.query).get('force', ['0'])[0] == '1'
            if force:
                self.send_json_response(self.get_rom_details_forced())
            else:
                self.send_json_response(self.get_rom_details())
        elif path == '/status/error_state':
            # NEW ENDPOINT: Return current error state
            global server_error_state, last_valid_core, last_valid_core_timestamp
            self.send_json_response({
                'error_state': server_error_state,
                'has_error': bool(server_error_state),
                'last_valid_core': last_valid_core,
                'last_valid_timestamp': last_valid_core_timestamp,
                'timestamp': int(time.time())
            })
        elif path == '/status/snapshot':
            # Atomic identity snapshot. Optional ?seq=N: if the caller already
            # has the current generation, reply with a tiny body so the ESP32
            # skips re-parsing on the (common) no-change poll.
            from urllib.parse import parse_qs
            known_seq = parse_qs(parsed_path.query).get('seq', [None])[0]
            snap = self.get_state_snapshot()
            if known_seq is not None and known_seq == str(snap['seq']):
                self.send_json_response({'seq': snap['seq'], 'unchanged': True})
            else:
                self.send_json_response(snap)
        elif path == '/status/all':
            status = {
                'core': self.get_current_core(),
                'rom': self.get_current_rom(),
                'game': self.get_current_game(),
                'system': self.get_system_info(),
                'storage': self.get_storage_info(),
                'usb': self.get_usb_info(),
                'network': self.get_network_stats(),
                'session': self.get_session_stats(),
                'error_state': server_error_state,          # NEW
                'has_error': bool(server_error_state),      # NEW
                'last_valid_core': last_valid_core,         # NEW
                'timestamp': int(time.time()),
                'snapshot': self.get_state_snapshot(),   # atomic identity block
            }
            self.send_json_response(status)
        else:
            self.send_error_response(404, 'Endpoint not found')

    # ========== OPTIMIZED CORE FUNCTIONS ==========
    
    def get_current_core(self):
        """Returns the currently active core friendly name from centralized state."""
        with _state_lock:
            return _state['core']
        
    def get_state_snapshot(self):
        """
        Single-lock atomic snapshot of the core/game identity. This is what
        the firmware polls: one request, one lock acquisition, one coherent
        state. rom_details is the CACHED value only — never computed here
        (computation stays on /status/rom/details, which can take minutes
        for large CHDs).
        """
        with _state_lock:
            return {
                'seq':               _state['seq'],
                'core':              _state['core'],
                'system_name':       _state['system_name'],
                'game':              _state['game'],
                'game_path':         _state['game_path'],
                'is_arcade':         _state['is_arcade'],
                'rom_details_stale': _state['rom_details_stale'],
                'rom_details':       _state['rom_details'],
                'last_event':        _state['last_event'],
                'updated_at':        _state['updated_at'],
                'timestamp':         int(time.time()),
            }
        
    def resolve_zip_path(self, zip_path):
        """
        Enhanced ZIP path resolution - handles relative paths from MiSTer
        """
        if not zip_path:
            return None
        
        print(f"🔍 Resolving ZIP path: {zip_path}")
        
        # If already absolute and exists, return as-is
        if os.path.isabs(zip_path) and os.path.exists(zip_path):
            print(f"✅ ZIP found (absolute): {zip_path}")
            return zip_path
        
        # Common MiSTer root directories to try
        possible_roots = [
            "/media/fat",           # Standard MiSTer location
            "/tmp",                 # Current working directory
            "/",                    # Root filesystem
            "/opt/MiSTer",         # Alternative installation
            os.getcwd(),           # Current Python script directory
        ]
        
        # Clean up the relative path
        clean_path = zip_path
        if clean_path.startswith("../../../"):
            # Remove leading ../../../ which typically points to /media/fat from /tmp
            clean_path = clean_path.replace("../../../", "")
        elif clean_path.startswith("../../"):
            clean_path = clean_path.replace("../../", "")
        elif clean_path.startswith("../"):
            clean_path = clean_path.replace("../", "")
        
        print(f"🧹 Cleaned path: {clean_path}")
        
        # Try each possible root directory
        for root in possible_roots:
            candidate_path = os.path.join(root, clean_path)
            normalized_path = os.path.normpath(candidate_path)
            
            print(f"🔍 Trying: {normalized_path}")
            
            if os.path.exists(normalized_path):
                print(f"✅ ZIP found at: {normalized_path}")
                return normalized_path
        
        # If direct resolution fails, try to find the file by searching
        filename = os.path.basename(zip_path)
        print(f"🔍 Searching for ZIP filename: {filename}")
        
        # Search in common game directories (limited depth for performance)
        search_dirs = [
            "/media/fat/games",
            "/media/fat",
            "/tmp",
        ]
        
        for search_dir in search_dirs:
            if os.path.exists(search_dir):
                try:
                    print(f"🔍 Searching in: {search_dir}")
                    for root, dirs, files in os.walk(search_dir):
                        if filename in files:
                            found_path = os.path.join(root, filename)
                            print(f"✅ ZIP found by search: {found_path}")
                            return found_path
                        
                        # Limit search depth to avoid performance issues
                        if root.count(os.sep) - search_dir.count(os.sep) >= 3:
                            dirs.clear()
                            
                except Exception as e:
                    print(f"⚠️ Search error in {search_dir}: {e}")
                    continue
        
        print(f"❌ ZIP file not found: {zip_path}")
        return None

    def get_current_game(self):
        """Returns the currently active game name from centralized state."""
        with _state_lock:
            return _state['game']

    # ========== HELPER FUNCTIONS ==========

    def extract_game_name(self, game_path, preserve_parentheses=True):
        """
        Extrae el nombre del juego de una ruta
        For non-arcade games: preserves parentheses (complete information)
        """
        if not game_path:
            return ""
        
        # Extraer nombre base del archivo
        base_name = os.path.splitext(os.path.basename(game_path))[0]
        
        if preserve_parentheses:
            # For non-arcade games: preserve parentheses (full name)
            return base_name.strip()
        else:
            # For arcade: clean parentheses if needed
            clean_name = re.sub(r'\s*\([^)]*\)', '', base_name).strip()
            return clean_name

    def _is_activegame_current(self, corename, activegame):
        """
        Verifica si ACTIVEGAME es actual para el core dado
        FIXED: More intelligent consistency checking
        """
        try:
            # Step 1: Check timestamp (basic validation)
            activegame_stat = os.path.getmtime('/tmp/ACTIVEGAME')
            age = time.time() - activegame_stat
            
            # If file is very old (more than 5 minutes), probably not current
            if age > 300:  # 5 minutes
                print(f"❌ ACTIVEGAME too old: {age:.1f}s > 300s")
                return False
            
            # Step 2: Check path consistency with core type
            if not corename or not activegame:
                print(f"❌ Missing corename or activegame")
                return False
            
            # Step 3: For known non-arcade systems, ACTIVEGAME should NOT be in _Arcade
            if _is_known_non_arcade(corename):
                if "/_Arcade/" in activegame:
                    print(f"❌ Non-arcade core '{corename}' but ACTIVEGAME is in _Arcade: {activegame}")
                    return False
                else:
                    print(f"✅ Non-arcade core '{corename}' with consistent ACTIVEGAME")
                    return True
            
            # Step 4: For potential arcade systems, check FULLPATH consistency
            try:
                with open('/tmp/FULLPATH', 'r') as f:
                    fullpath = f.read().strip()
                
                # If FULLPATH indicates arcade but ACTIVEGAME is not in _Arcade
                if ("arcade" in fullpath.lower() or "_Arcade" in fullpath):
                    if "/_Arcade/" not in activegame:
                        print(f"❌ FULLPATH indicates arcade but ACTIVEGAME not in _Arcade")
                        return False
                    else:
                        print(f"✅ Arcade context with consistent ACTIVEGAME")
                        return True
                else:
                    # FULLPATH doesn't indicate arcade, ACTIVEGAME should not be in _Arcade
                    if "/_Arcade/" in activegame:
                        print(f"❌ FULLPATH doesn't indicate arcade but ACTIVEGAME is in _Arcade")
                        return False
                    else:
                        print(f"✅ Non-arcade context with consistent ACTIVEGAME")
                        return True
                        
            except Exception as e:
                print(f"⚠️ Error reading FULLPATH: {e}")
                # If we can't read FULLPATH, fall back to basic validation
                print(f"✅ FULLPATH unavailable, accepting ACTIVEGAME based on timestamp only")
                return True
            
        except Exception as e:
            print(f"❌ Error in _is_activegame_current: {e}")
            return False

    # ========== ORIGINAL FUNCTIONS (NO CHANGES) ==========
    
    def get_current_rom(self):
        """
        Returns the current ROM filename from centralized state.

        """
        with _state_lock:
            game_path = _state['game_path']
            game_name = _state['game']
        if game_path:
            return os.path.basename(game_path)
        if game_name:
            return game_name
        return "No ROM"

    def get_system_info(self):
        """
        System information (without temperature)
        """
        info = {
            'cpu_usage': 0.0,
            'memory_usage': 0.0,
            'uptime_seconds': 0,
            'load_average': [0.0, 0.0, 0.0]
        }
        
        # Load average
        try:
            with open('/proc/loadavg', 'r') as f:
                loads = f.read().strip().split()
                info['load_average'] = [float(loads[0]), float(loads[1]), float(loads[2])]
                load_1min = float(loads[0])
                info['cpu_usage'] = round(min(load_1min * 50, 100.0), 1)
        except:
            pass
        
        # Memory
        try:
            with open('/proc/meminfo', 'r') as f:
                meminfo = f.read()
                mem_total = int(re.search(r'MemTotal:\s+(\d+)', meminfo).group(1))
                mem_available = int(re.search(r'MemAvailable:\s+(\d+)', meminfo).group(1))
                info['memory_usage'] = round((1 - mem_available / mem_total) * 100, 1)
        except:
            pass
        
        # Uptime
        try:
            with open('/proc/uptime', 'r') as f:
                uptime = float(f.read().split()[0])
                info['uptime_seconds'] = int(uptime)
        except:
            pass
        
        return info

    def get_storage_info(self):
        """
        Storage information
        """
        storage = {
            'sd_card': {'total_gb': 0, 'used_gb': 0, 'free_gb': 0, 'usage_percent': 0},
            'usb_drives': []
        }
        
        try:
            # SD card (/media/fat)
            if os.path.exists('/media/fat'):
                stat = shutil.disk_usage('/media/fat')
                total = stat.total / (1024**3)
                free = stat.free / (1024**3)
                used = total - free
                usage_percent = (used / total) * 100 if total > 0 else 0
                
                storage['sd_card'] = {
                    'total_gb': round(total, 1),
                    'used_gb': round(used, 1),
                    'free_gb': round(free, 1),
                    'usage_percent': round(usage_percent, 1)
                }
        except:
            pass
        
        return storage

    def get_usb_info(self):
        """
        USB device information
        """
        usb_info = {
            'devices': [],
            'serial_ports': [],
            'ports_used': 0,
            'ports_total': 4
        }
        
        try:
            # USB devices via lsusb
            result = subprocess.run(['lsusb'], capture_output=True, text=True, timeout=3)
            if result.returncode == 0:
                lines = result.stdout.strip().split('\n')
                for line in lines:
                    if line.strip():
                        match = re.match(r'Bus (\d+) Device (\d+): ID ([0-9a-f:]+) (.+)', line)
                        if match:
                            bus, device, usb_id, name = match.groups()
                            usb_info['devices'].append({
                                'bus': int(bus),
                                'device': int(device),
                                'id': usb_id,
                                'name': name.strip()
                            })
                
                usb_info['ports_used'] = len([d for d in usb_info['devices'] if 'hub' not in d['name'].lower()])
        except:
            pass
        
        return usb_info

    def get_network_stats(self):
        """
        Network statistics
        """
        stats = {
            'connected': False,
            'interface': '',
            'ip_address': '',
            'rx_kbps': 0.0,
            'tx_kbps': 0.0,
            'rx_bytes': 0,
            'tx_bytes': 0
        }
        
        try:
            # Active network interface
            result = subprocess.run(['ip', 'route', 'get', '8.8.8.8'], 
                                  capture_output=True, text=True, timeout=3)
            if result.returncode == 0:
                match = re.search(r'dev (\w+)', result.stdout)
                if match:
                    interface = match.group(1)
                    stats['interface'] = interface
                    
                    # Interface IP
                    ip_result = subprocess.run(['ip', 'addr', 'show', interface], 
                                             capture_output=True, text=True, timeout=3)
                    if ip_result.returncode == 0:
                        ip_match = re.search(r'inet (\d+\.\d+\.\d+\.\d+)', ip_result.stdout)
                        if ip_match:
                            stats['ip_address'] = ip_match.group(1)
                            stats['connected'] = True
        except:
            pass
        
        return stats

    def get_session_stats(self):
        """
        Session statistics
        """
        current_time = time.time()
        session_duration = current_time - _session_start
        
        stats = {
            'session_start_time': int(_session_start),
            'session_duration_seconds': int(session_duration),
            'session_duration_formatted': self.format_duration(session_duration),
            'requests_count': _requests_count,
            'requests_per_minute': round((_requests_count / (session_duration / 60)) if session_duration > 0 else 0, 2),
            'current_time': int(current_time)
        }
        
        return stats

    def format_duration(self, seconds):
        """
        Formats duration as readable text
        """
        hours = int(seconds // 3600)
        minutes = int((seconds % 3600) // 60)
        secs = int(seconds % 60)
        
        if hours > 0:
            return f"{hours}h {minutes}m {secs}s"
        elif minutes > 0:
            return f"{minutes}m {secs}s"
        else:
            return f"{secs}s"

    # ========== ROM DETAILS WITH ZIP SUPPORT ==========
    
    def is_zip_path(self, path):
        """
        Check if the path contains a ZIP file
        Returns tuple: (is_zip, zip_path, internal_path)
        """
        if not path:
            return False, None, None
            
        # Look for .zip in the path (case insensitive)
        zip_match = re.search(r'(.+\.zip)', path, re.IGNORECASE)
        if zip_match:
            zip_path = zip_match.group(1)
            # Get the part after the ZIP file
            internal_path = path[len(zip_path):].lstrip('/')
            return True, zip_path, internal_path
        
        return False, None, None
    
    def get_zip_file_info_enhanced(self, zip_path, internal_path):
        """
        ENHANCED: Get file info from ZIP with multiple search strategies.
        Returns (filename, file_size, crc32_int) where crc32_int comes straight
        from the ZIP central directory (ZipInfo.CRC) — no decompression needed.
        """
        try:
            with zipfile.ZipFile(zip_path, 'r') as zip_file:
                zip_files = zip_file.namelist()
                
                # Try multiple search strategies
                search_paths = [
                    internal_path,
                    internal_path.replace('\\', '/'),
                    internal_path.replace('/', '\\'),
                    internal_path.replace('\\', '/').lstrip('/'),
                    internal_path.replace('/', '\\').lstrip('\\')
                ]
                
                for search_path in search_paths:
                    if search_path in zip_files:
                        info = zip_file.getinfo(search_path)
                        filename = os.path.basename(search_path)
                        print(f"✅ File info found: {filename} ({info.file_size:,} bytes)")
                        return filename, info.file_size, info.CRC
                
                # Case-insensitive search
                internal_lower = internal_path.lower()
                for zip_file_path in zip_files:
                    if zip_file_path.lower() == internal_lower:
                        info = zip_file.getinfo(zip_file_path)
                        filename = os.path.basename(zip_file_path)
                        print(f"✅ File info (case-insensitive): {filename} ({info.file_size:,} bytes)")
                        return filename, info.file_size, info.CRC
                
                # Filename-only search
                target_filename = os.path.basename(internal_path).lower()
                for zip_file_path in zip_files:
                    if os.path.basename(zip_file_path).lower() == target_filename:
                        info = zip_file.getinfo(zip_file_path)
                        filename = os.path.basename(zip_file_path)
                        print(f"✅ File info (filename): {filename} ({info.file_size:,} bytes)")
                        return filename, info.file_size, info.CRC
                
                # Strategy 5: Stem match — handles cores that write the filename
                # without extension to CURRENTPATH. Compare the path stem (without
                # extension) case-insensitively;
                target_stem = os.path.splitext(internal_path)[0].lower()
                stem_matches = []
                for zip_file_path in zip_files:
                    zip_stem = os.path.splitext(zip_file_path)[0].lower()
                    if zip_stem == target_stem:
                        stem_matches.append(zip_file_path)
                
                if stem_matches:
                    rom_match = next(
                        (m for m in stem_matches
                         if os.path.splitext(m)[1].lower() in _KNOWN_ROM_EXTS),
                        None
                    )
                    chosen = rom_match if rom_match else stem_matches[0]
                    info = zip_file.getinfo(chosen)
                    filename = os.path.basename(chosen)
                    print(f"✅ File info (stem match): {filename} ({info.file_size:,} bytes)")
                    if len(stem_matches) > 1:
                        print(f"   ℹ️ {len(stem_matches)} candidates with same stem; chose ROM-ext match")
                    return filename, info.file_size, info.CRC
                
                print(f"❌ File info not found: {internal_path}")
                return None, 0, 0
            
        except Exception as e:
            print(f"❌ ZIP info error: {e}")
            return None, 0, 0
    
    def get_rom_details(self):
        """
        Returns ROM details (CRC, hashes, path).
        Uses _state['rom_details'] as cache — refreshed when rom_details_stale is True.
        """
        print(f"[{time.strftime('%H:%M:%S')}] Getting ROM details...")

        with _state_lock:
            stale        = _state['rom_details_stale']
            cached       = _state['rom_details']
            seq_at_start = _state['seq']

        if not stale and cached is not None:
            print("📋 Using cached ROM details")
            return cached

        # Coalesce concurrent requests: a second caller (e.g. the firmware's
        # retry) blocks on this lock, then re-checks the cache and returns the
        # first thread's result instead of starting a duplicate hash/CRC.
        with _rom_details_compute_lock:
            with _state_lock:
                stale   = _state['rom_details_stale']
                cached  = _state['rom_details']
            if not stale and cached is not None:
                print("📋 Using cached ROM details (computed by concurrent request)")
                return cached

            print("📄 Computing ROM details...")
            rom_path = self._get_enhanced_rom_path()

            if not rom_path:
                result = {
                    "filename": "", "size": 0, "crc32": "", "md5": "", "sha1": "",
                    "path": "", "available": False,
                    "error": "No active ROM found",
                    "detection_method": "none",
                    "timestamp": int(time.time())
                }
            else:
                is_zip, zip_path, internal_path = self.is_zip_path(rom_path)
                if is_zip:
                    result = self.get_rom_details_from_zip(rom_path, zip_path, internal_path)
                else:
                    result = self.get_rom_details_from_file(rom_path)
                result["detection_method"] = getattr(self, '_last_detection_method', 'unknown')

            _enrich_rom_result(result)
            result['seq'] = seq_at_start
            with _state_lock:
                if _state['seq'] == seq_at_start:
                    _state['rom_details']       = result
                    _state['rom_details_stale'] = False
                else:
                    print("⚠️ State changed during ROM hashing — result NOT cached (belongs to a previous game)")

            return result
    
    def get_rom_details_forced(self):
        """
        Forced ROM details: bypasses game-name detection and timestamp checks.
        Goes directly to _get_non_arcade_rom_path() / _get_arcade_rom_path()
        so that RESCAN GAME works even when FILESELECT timestamps are stale.
        """
        print("🔄 === FORCED ROM DETAILS (bypass timestamp check) ===")
        try:
            with _state_lock:
                seq_at_start = _state['seq']
            corename = ""
            try:
                with open('/tmp/CORENAME', 'r') as f:
                    corename = f.read().strip()
            except:
                pass

            is_arcade = self._is_arcade_system(corename)
            if is_arcade:
                rom_path = self._get_arcade_rom_path()
            else:
                rom_path = self._get_non_arcade_rom_path()

            if not rom_path:
                return _enrich_rom_result({
                    "filename": "", "size": 0, "crc32": "", "md5": "", "sha1": "",
                    "path": "", "available": False,
                    "error": "Forced scan: no ROM path found via CURRENTPATH/ACTIVEGAME",
                    "detection_method": "forced_none", "timestamp": int(time.time())
                })

            print(f"🔄 Forced path resolved: {rom_path}")
            is_zip, zip_path, internal_path = self.is_zip_path(rom_path)
            if is_zip:
                result = self.get_rom_details_from_zip(rom_path, zip_path, internal_path)
            else:
                result = self.get_rom_details_from_file(rom_path)

            result["detection_method"] = "forced"
            _enrich_rom_result(result)
            result['seq'] = seq_at_start
            # Update cache so subsequent normal calls benefit — but only if the
            # active game hasn't changed since we started hashing (a slow CHD
            # hash could otherwise attach this result to a different game).
            with _state_lock:
                if _state['seq'] == seq_at_start:
                    _state['rom_details']       = result
                    _state['rom_details_stale'] = False
                else:
                    print("⚠️ State changed during forced ROM hashing — result NOT cached (belongs to a previous game)")
            return result

        except Exception as e:
            import traceback
            traceback.print_exc()
            return _enrich_rom_result({
                "filename": "", "size": 0, "crc32": "", "md5": "", "sha1": "",
                "path": "", "available": False,
                "error": f"Forced scan error: {str(e)}",
                "detection_method": "forced_error", "timestamp": int(time.time())
            })

    def _get_enhanced_rom_path(self):
        """
        Enhanced ROM path detection following the new logic:
        1. Check /status/game endpoint
        2. Verify SAM_Games.log for path extraction if game matches
        3. Check CORENAME to determine arcade vs non-arcade
        4. Use ACTIVEGAME for non-arcade or STARTPATH for arcade
        """
        print("🔍 === ENHANCED ROM PATH DETECTION ===")
        
        # STEP 1: Get current game from /status/game endpoint
        try:
            current_game = self.get_current_game()
            print(f"📊 Current game from /status/game: '{current_game}'")
            
            if not current_game or current_game in ["", "Sin juego", "No game"]:
                print("❌ No current game detected")
                self._last_detection_method = "no_game"
                return None
                
        except Exception as e:
            print(f"❌ Error getting current game: {e}")
            current_game = None
        
        # STEP 2: Check SAM_Games.log if we have a current game
        if current_game:
            sam_rom_path = self._check_sam_games_log_for_path(current_game)
            if sam_rom_path:
                print(f"✅ Found ROM path in SAM_Games.log: {sam_rom_path}")
                self._last_detection_method = "sam_games_log"
                return sam_rom_path
        
        # STEP 3: Check CORENAME to determine system type
        try:
            with open('/tmp/CORENAME', 'r') as f:
                corename = f.read().strip()
                print(f"📄 CORENAME: '{corename}'")
        except Exception as e:
            print(f"❌ Cannot read CORENAME: {e}")
            corename = ""
        
        if not corename:
            print("❌ No CORENAME available")
            self._last_detection_method = "no_corename"
            return None
        
        # STEP 4: Determine if this is an arcade system
        is_arcade = self._is_arcade_system(corename)
        print(f"🎮 System type - Arcade: {is_arcade}")
        
        if is_arcade:
            # For arcade systems, use STARTPATH
            rom_path = self._get_arcade_rom_path()
            if rom_path:
                self._last_detection_method = "arcade_startpath"
            else:
                self._last_detection_method = "arcade_failed"
        else:
            # For non-arcade systems, use ACTIVEGAME
            rom_path = self._get_non_arcade_rom_path()
            if rom_path:
                self._last_detection_method = "non_arcade_activegame"
            else:
                self._last_detection_method = "non_arcade_failed"
        
        return rom_path

    def _check_sam_games_log_for_path(self, current_game):
        """
        Check SAM_Games.log to find the path for the current game
        Returns the full ROM path if found, None otherwise
        """
        try:
            sam_log_path = '/tmp/SAM_Games.log'
            
            if not os.path.exists(sam_log_path):
                print(f"📄 SAM_Games.log not found at {sam_log_path}")
                return None
            
            # Check if file is recent enough (within 5 minutes)
            sam_stat = os.path.getmtime(sam_log_path)
            age = time.time() - sam_stat
            
            if age > 300:  # 5 minutes
                print(f"📄 SAM_Games.log too old: {age:.1f}s")
                return None
            
            # Read and parse the log file
            try:
                with open(sam_log_path, 'r', encoding='utf-8', errors='ignore') as f:
                    lines = f.readlines()
            except UnicodeDecodeError:
                with open(sam_log_path, 'r', encoding='latin-1') as f:
                    lines = f.readlines()
            
            if not lines:
                print("📄 SAM_Games.log is empty")
                return None
            
            # Process lines from last to first to find the most recent matching entry
            for i in range(len(lines) - 1, -1, -1):
                line = lines[i].strip()
                if not line:
                    continue
                
                # SAM format: "04:17:58 - atarilynx - /media/fat/games/AtariLynx/..."
                parts = line.split(' - ')
                
                if len(parts) >= 3:
                    sam_path = ' - '.join(parts[2:])  # Rejoin path in case it contains " - "
                    
                    # Extract game name from path
                    if sam_path:
                        game_filename = sam_path.split('/')[-1]
                        sam_game = os.path.splitext(game_filename)[0]
                        
                        print(f"🔍 SAM entry - Game: '{sam_game}', Path: '{sam_path}'")
                        
                        # Check if this game matches our current game
                        if self._games_match(current_game, sam_game):
                            print(f"✅ Game match found in SAM: '{current_game}' == '{sam_game}'")
                            return sam_path
            
            print(f"❌ No matching game found in SAM_Games.log for: '{current_game}'")
            return None
            
        except Exception as e:
            print(f"❌ Error checking SAM_Games.log: {e}")
            return None

    def _games_match(self, game1, game2):
        """
        Check if two game names match, accounting for variations in naming
        """
        if not game1 or not game2:
            return False
        
        # Direct match
        if game1 == game2:
            return True
        
        # Case insensitive match
        if game1.lower() == game2.lower():
            return True
        
        # Remove common suffixes/prefixes and compare
        clean1 = re.sub(r'\s*\([^)]*\)', '', game1).strip()
        clean2 = re.sub(r'\s*\([^)]*\)', '', game2).strip()
        
        if clean1.lower() == clean2.lower():
            return True
        
        return False

    def _is_arcade_system(self, corename):
        """
        Determine if the current core is an arcade system
        SIMPLIFIED: Use existing get_current_core() logic instead of duplicating
        """
        try:
            current_core = self.get_current_core()
            print(f"🎮 Current core from detection: '{current_core}'")
            
            # If get_current_core() returns "Arcade", it's arcade
            is_arcade = (current_core.lower() == "arcade")
            
            print(f"🎮 '{corename}' system type → Arcade: {is_arcade}")
            return is_arcade
            
        except Exception as e:
            print(f"❌ Error in _is_arcade_system: {e}")
            return False

    def _get_arcade_rom_path(self):
        """
        Get ROM path for arcade systems using STARTPATH
        """
        try:
            with open('/tmp/STARTPATH', 'r') as f:
                startpath = f.read().strip()
                print(f"📄 STARTPATH (arcade): '{startpath}'")
                
                if startpath and os.path.exists(startpath):
                    return startpath
                else:
                    print(f"❌ STARTPATH file does not exist: {startpath}")
                    return None
                    
        except Exception as e:
            print(f"❌ Error reading STARTPATH: {e}")
            return None

    def _get_non_arcade_rom_path(self):
        """
        Get ROM path for non-arcade systems.

        MiSTer uses two separate files for path context:
          - CURRENTPATH: the selected filename (may have no directory component)
          - FULLPATH:    the current browser directory, which may include a ZIP path
                         e.g. "games/Apple-II/Collection.zip/"

        When CURRENTPATH has no directory component, FULLPATH provides the
        missing context. Combining them:
            FULLPATH.rstrip('/') + '/' + CURRENTPATH
        produces the complete virtual path, e.g.:
            games/Apple-II/Collection.zip/221B Baker Street.do

        which _resolve_mister_path() and is_zip_path() can parse correctly.

        ACTIVEGAME (when present) always contains the full path and is tried first.
        """
        activegame = ""
        activegame_timestamp = 0
        currentpath_timestamp = 0

        try:
            with open('/tmp/ACTIVEGAME', 'r') as f:
                activegame = f.read().strip()
            activegame_timestamp = os.path.getmtime('/tmp/ACTIVEGAME')
        except:
            pass

        currentpath = ''
        currentpath_timestamp = 0
        fullpath = ''
        path_source = 'CURRENTPATH'
        try:
            with open('/tmp/CURRENTPATH', 'r') as f:
                currentpath = f.read().strip()
            currentpath_timestamp = os.path.getmtime('/tmp/CURRENTPATH')
        except:
            pass
        try:
            with open('/tmp/FULLPATH', 'r') as f:
                fullpath = f.read().strip()
        except:
            pass

        print(f"📄 ACTIVEGAME:       '{activegame}' (ts: {activegame_timestamp})")
        print(f"📄 {path_source}: '{currentpath}' (ts: {currentpath_timestamp})")
        print(f"📄 FULLPATH source:  '{fullpath}'")

        # When CURRENTPATH has no directory component, combine it with FULLPATH.
        if currentpath and not os.path.dirname(currentpath) and fullpath:
            fullpath_dir = fullpath.rstrip('/')
            if os.path.basename(fullpath_dir) == currentpath:
                # MGL/CHD launches: FULLPATH already carries the
                # complete file path INCLUDING the filename — joining would
                # duplicate it ('.../game.chd/game.chd'). Use it as-is.
                print(f"🔗 FULLPATH already ends with CURRENTPATH - using it as-is: '{fullpath_dir}'")
                currentpath = fullpath_dir
            else:
                combined = fullpath_dir + '/' + currentpath
                print(f"🔗 CURRENTPATH has no directory - combining with FULLPATH: '{combined}'")
                currentpath = combined

        # Determine preferred order by timestamp (same logic as get_current_game)
        activegame_is_newer = activegame_timestamp > currentpath_timestamp
        if activegame_is_newer:
            sources = [('ACTIVEGAME', activegame), ('CURRENTPATH', currentpath)]
        else:
            sources = [('CURRENTPATH', currentpath), ('ACTIVEGAME', activegame)]

        print(f"⏱️ Preferred source: {'ACTIVEGAME' if activegame_is_newer else 'CURRENTPATH'} (newer)")

        for source_name, source_path in sources:
            if not source_path:
                print(f"⏭️ {source_name} is empty - skipping")
                continue

            # Safety check: non-arcade path should not point into _Arcade
            if "_Arcade" in source_path:
                print(f"⚠️ {source_name} contains arcade path, skipping: '{source_path}'")
                continue

            try:
                final_path = self._resolve_mister_path(source_path)
                print(f"🔧 {source_name} resolved to: '{final_path}'")

                is_zip, zip_path, internal_path = self.is_zip_path(final_path)

                if is_zip:
                    print(f"📦 ZIP detected: {zip_path} -> '{internal_path}'")
                    if os.path.exists(zip_path):
                        print(f"✅ ZIP verified via {source_name}: {zip_path}")
                        return final_path
                    else:
                        print(f"❌ ZIP not found via {source_name}: {zip_path} - trying next source")
                        continue
                else:
                    if os.path.isfile(final_path):
                        print(f"✅ ROM file found via {source_name}: {final_path}")
                        return final_path
                    elif os.path.isdir(final_path):
                        # Folder-per-game layout: the game lives in a folder named
                        # after it. os.path.exists()
                        # is also true for directories, so without this branch the
                        # server would try to hash the folder itself (Errno 21).
                        print(f"📁 {source_name} resolved to a directory — searching disc image inside")
                        try:
                            entries = sorted(os.listdir(final_path))
                        except Exception as e:
                            print(f"❌ Cannot list directory: {e}")
                            entries = []
                        for ext in ('.chd', '.cue', '.iso', '.pbp'):
                            matches = [f for f in entries if f.lower().endswith(ext)]
                            if matches:
                                chosen = os.path.join(final_path, matches[0])
                                print(f"✅ Disc image found in folder ({source_name}): {chosen}")
                                return chosen
                        print(f"❌ No disc image inside directory: {final_path}")
                        print(f"❌ Direct file not found: {final_path}")
                    else:
                        print(f"❌ Direct file not found: {final_path}")

                        # CD images: some cores (PSX, Saturn) write CURRENTPATH without the
                        # extension. Try common disc-image extensions before giving up.
                        for ext in ('.chd', '.cue', '.iso', '.pbp'):
                            cd_candidate = final_path + ext
                            if os.path.exists(cd_candidate):
                                print(f"✅ CD image found ({source_name}): {cd_candidate}")
                                return cd_candidate

                        # Last resort: same-name ZIP in the same directory
                        # (handles individual per-game ZIPs: game.dsk → game.zip/game.dsk)
                        parent_dir = os.path.dirname(final_path)
                        target_filename = os.path.basename(final_path)
                        base_name = os.path.splitext(target_filename)[0]
                        zip_candidate = os.path.join(parent_dir, base_name + '.zip')
                        print(f"🔍 Trying same-name ZIP: '{zip_candidate}'")
                        if os.path.exists(zip_candidate):
                            virtual_path = zip_candidate + '/' + target_filename
                            print(f"✅ Same-name ZIP found ({source_name}): {virtual_path}")
                            return virtual_path

                        # Title-based fallbacks: some cores (notably NEOGEO) write
                        # the game's display TITLE to CURRENTPATH instead of the
                        # filename on disk.

                        # (a) Directory scan: a file whose name starts with the
                        #     title and has a known ROM extension.
                        try:
                            entries = sorted(os.listdir(parent_dir))
                        except Exception:
                            entries = []
                        title_l = target_filename.lower()
                        for entry in entries:
                            stem, ext = os.path.splitext(entry)
                            if ext.lower() in _KNOWN_ROM_EXTS and stem.lower().startswith(title_l):
                                candidate = os.path.join(parent_dir, entry)
                                if os.path.isfile(candidate):
                                    print(f"✅ Title-prefix match ({source_name}): {candidate}")
                                    return candidate

                        # (b) romsets.xml reverse lookup: display title -> romset
                        #     name -> romset file (NEOGEO layouts).
                        candidate = self._lookup_neogeo_romset(parent_dir, target_filename)
                        if candidate:
                            print(f"✅ romsets.xml match ({source_name}): {candidate}")
                            return candidate
                        print(f"❌ No valid path found via {source_name} - trying next source")
                        continue

            except Exception as e:
                print(f"❌ Error resolving {source_name}: {e} - trying next source")
                continue

        print(f"❌ No valid ROM path found from any source")
        return None
    
    def _lookup_neogeo_romset(self, directory, title):
        """
        Reverse lookup in romsets.xml (NEOGEO): display title -> romset name.
        The core shows the title from the .neo header / romsets.xml and writes
        that TITLE to CURRENTPATH, so the file on disk (e.g. 'blazstar.neo')
        can have a completely different name. Punctuation also differs between
        sources (':' in the XML vs ' - ' in CURRENTPATH), so titles are
        compared on their alphanumeric characters only.
        """
        xml_path = os.path.join(directory, 'romsets.xml')
        if not os.path.isfile(xml_path):
            return None
        try:
            import xml.etree.ElementTree as ET
            root = ET.parse(xml_path).getroot()
        except Exception as e:
            print(f"⚠️ romsets.xml parse failed: {e}")
            return None

        def norm(s):
            return re.sub(r'[^a-z0-9]', '', s.lower())

        wanted = norm(title)
        if not wanted:
            return None
        # Pass 1: exact match on normalized title (altname or romset name).
        # Pass 2: prefix match. A prefix candidate only counts if its file
        # actually exists on disk, and if more than one qualifies we refuse
        # to guess.
        exact_file = None
        prefix_files = []
        for rs in root.iter('romset'):
            name    = rs.get('name') or ''
            altname = rs.get('altname') or ''
            norm_alt  = norm(altname)
            norm_name = norm(name)

            is_exact  = (norm_alt == wanted or norm_name == wanted)
            is_prefix = (not is_exact and
                         (norm_alt.startswith(wanted) or norm_name.startswith(wanted)))
            if not (is_exact or is_prefix):
                continue

            found = None
            for filename in (name + '.neo', name + '.zip'):
                p = os.path.join(directory, filename)
                if os.path.isfile(p):
                    found = p
                    break

            if is_exact:
                if found:
                    return found
                print(f"ℹ️ romsets.xml maps '{title}' -> '{name}' but no matching file on disk")
                return None
            if found:
                prefix_files.append((name, found))

        if len(prefix_files) == 1:
            print(f"ℹ️ romsets.xml prefix match: '{title}' -> '{prefix_files[0][0]}'")
            return prefix_files[0][1]
        if len(prefix_files) > 1:
            names = ', '.join(n for n, _ in prefix_files)
            print(f"⚠️ romsets.xml: '{title}' is ambiguous ({names}) — not guessing")
        return None

    def _resolve_mister_path(self, path):
        """
        Intelligently resolve MiSTer paths handling various relative path patterns
        """
        if not path:
            return path
        
        print(f"🔍 Resolving path: '{path}'")
        
        # Case 1: Already absolute path
        if os.path.isabs(path):
            resolved = os.path.normpath(path)
            print(f"✅ Already absolute: {resolved}")
            return resolved
        
        # Leading ../ sequences are relative to /media/fat, so '../usb0/...'
        # means /media/usb0/... and '../fat/...' means /media/fat/... itself.
        # Without this, the generic cleanup prepends /media/fat and produces
        # /media/fat/fat/... (or /media/fat/usb0/...), which don't exist.
        m = re.match(r'(?:\.\./)+((?:usb[0-7]|fat)/.*)$', path)
        if m:
            resolved = os.path.normpath('/media/' + m.group(1))
            print(f"🔧 Relative /media path resolved: {resolved}")
            return resolved
        
        # Case 2: Starts with ../../../media/fat/ - remove the ../ and normalize
        if path.startswith("../../../media/fat/"):
            # Extract the part after ../../../
            clean_path = path[9:]  # Remove "../../../"
            resolved = os.path.normpath("/" + clean_path)
            print(f"🔧 Cleaned ../../../ pattern: {resolved}")
            return resolved
        
        # Case 3: Starts with ../../ - try different resolutions
        if path.startswith("../../"):
            # Try removing ../../ and prepending /media/fat/
            clean_path = path[6:]  # Remove "../../"
            if clean_path.startswith("media/fat/"):
                resolved = os.path.normpath("/" + clean_path)
            else:
                resolved = os.path.normpath("/media/fat/" + clean_path)
            print(f"🔧 Cleaned ../../ pattern: {resolved}")
            return resolved
        
        # Case 4: Starts with ../ 
        if path.startswith("../"):
            clean_path = path[3:]  # Remove "../"
            if clean_path.startswith("media/fat/"):
                resolved = os.path.normpath("/" + clean_path)
            else:
                resolved = os.path.normpath("/media/fat/" + clean_path)
            print(f"🔧 Cleaned ../ pattern: {resolved}")
            return resolved
        
        # Case 5: Simple relative path (games/SMS/...)
        if not path.startswith("/"):
            resolved = os.path.normpath("/media/fat/" + path)
            print(f"🔧 Added /media/fat/ prefix: {resolved}")
            return resolved
        
        # Case 6: Fallback - normalize as-is
        resolved = os.path.normpath(path)
        print(f"🔧 Normalized as-is: {resolved}")
        return resolved
    
    def get_rom_details_from_file(self, rom_path):
        """
        Get ROM details from regular file (not in ZIP)
        """
        # Verify file exists
        if not os.path.exists(rom_path):
            print(f"ROM file not found: {rom_path}")
            return {
                "filename": "",
                "size": 0,
                "crc32": "",
                "md5": "",
                "sha1": "",
                "path": rom_path,
                "available": False,
                "error": "ROM file not found or not accessible",
                "timestamp": int(time.time())
            }
        
        try:
            file_size = os.path.getsize(rom_path)
            filename = os.path.basename(rom_path)
            
            print(f"Processing ROM: {filename} ({file_size:,} bytes)")
            
            # Calculate hashes (skip only for pathologically large files)
            crc32 = ""
            md5 = ""
            sha1 = ""
            
            # Size limit to avoid blocking the server with very large files.
            # 1GB safely covers any single CD image (CDs cap at ~700MB, and a CHD
            # is compressed) while still guarding against a corrupt or mis-resolved
            # path pointing at something huge.
            MAX_SIZE_FOR_HASH = 1024 * 1024 * 1024  # 1GB
            
            # Mutable containers (.vhd) are never worth hashing: ScreenScraper
            # does not index them AND the guest OS rewrites them (save files),
            # so their CRC is unstable by nature. Skip the minutes of
            # CRC+MD5+SHA1 on the ARM entirely — same outcome as file_too_large.
            _ext      = os.path.splitext(filename)[1].lower()
            _corename = _read_corename_raw()
            skip_hash = _is_no_hash(_ext, _corename)

            if skip_hash:
                _why = ("unindexable, mutable container" if _ext in _NO_HASH_EXTS
                        else f"locally built by the {_corename} collection; CRC in no database")
                print(f"Hash skipped for {filename}: {_why}")
            elif file_size <= MAX_SIZE_FOR_HASH:
                try:
                    _wait_for_rom_load_to_settle()   # don't hash mid-load

                    start_time = time.time()
                    print(f"Calculating hashes for {filename}...")
                    
                    with open(rom_path, 'rb') as f:
                        # Read file in chunks to avoid saturating memory
                        chunk_size = 1024 * 1024  # 1MB chunks
                        crc32_calc = 0
                        md5_calc = hashlib.md5()
                        sha1_calc = hashlib.sha1()
                        
                        bytes_processed = 0
                        while True:
                            chunk = f.read(chunk_size)
                            if not chunk:
                                break
                            
                            # Update all hashes with the same chunk
                            crc32_calc = zlib.crc32(chunk, crc32_calc)
                            md5_calc.update(chunk)
                            sha1_calc.update(chunk)
                            
                            bytes_processed += len(chunk)
                            time.sleep(0.003)   # ~0.3s extra per 100MB hashed
                    
                    # Format results
                    crc32 = format(crc32_calc & 0xffffffff, '08X')
                    md5 = md5_calc.hexdigest().upper()
                    sha1 = sha1_calc.hexdigest().upper()
                    
                    calc_time = time.time() - start_time
                    print(f"Hash calculation completed in {calc_time:.2f}s")
                    print(f"CRC32: {crc32}")
                    print(f"MD5: {md5}")
                    print(f"SHA1: {sha1}")
                    
                except Exception as e:
                    error_msg = f"Hash calculation failed: {str(e)}"
                    print(f"ERROR: {error_msg}")
                    return {
                        "filename": filename,
                        "size": file_size,
                        "crc32": "",
                        "md5": "",
                        "sha1": "",
                        "path": rom_path,
                        "available": True,
                        "hash_calculated": False,
                        "error": error_msg,
                        "timestamp": int(time.time())
                    }
            else:
                print(f"File too large for hash calculation: {file_size:,} bytes > {MAX_SIZE_FOR_HASH:,} bytes")
            
            # Return successful result
            result = {
                "filename": filename,
                "size": file_size,
                "crc32": crc32,
                "md5": md5,
                "sha1": sha1,
                "path": rom_path,
                "available": True,
                "hash_calculated": len(crc32) > 0,
                "file_too_large": file_size > MAX_SIZE_FOR_HASH,
                "timestamp": int(time.time())
            }
            
            print(f"ROM details successfully extracted: {filename}")
            return result
            
        except Exception as e:
            error_msg = f"Error processing file: {str(e)}"
            print(f"ERROR: {error_msg}")
            return {
                "filename": os.path.basename(rom_path),
                "size": 0,
                "crc32": "",
                "md5": "",
                "sha1": "",
                "path": rom_path,
                "available": False,
                "error": error_msg,
                "timestamp": int(time.time())
            }
    
    def get_rom_details_from_zip(self, full_path, zip_path, internal_path):
        """
        ENHANCED: Get ROM details from file inside ZIP with better path resolution
        """
        print(f"\n🔍 === ENHANCED ZIP ROM DETAILS ===")
        print(f"Full path: {full_path}")
        print(f"ZIP path: {zip_path}")
        print(f"Internal path: {internal_path}")
        
        # Resolve the actual ZIP file location
        resolved_zip_path = self.resolve_zip_path(zip_path)
        
        if not resolved_zip_path:
            error_msg = f"ZIP file not found: {zip_path}"
            print(f"❌ {error_msg}")
            
            return {
                "filename": os.path.basename(internal_path) if internal_path else "",
                "size": 0,
                "crc32": "",
                "md5": "",
                "sha1": "",
                "path": full_path,
                "available": False,
                "error": error_msg,
                "zip_path": zip_path,
                "resolved_zip_path": None,
                "internal_path": internal_path,
                "timestamp": int(time.time())
            }
        
        try:
            print(f"📂 Opening ZIP: {resolved_zip_path}")
            
            # Get file info from ZIP with enhanced search
            filename, file_size, zip_crc = self.get_zip_file_info_enhanced(resolved_zip_path, internal_path)
            
            if not filename:
                error_msg = f"File not found inside ZIP: {internal_path}"
                print(f"❌ {error_msg}")
                
                # List some ZIP contents for debugging
                try:
                    with zipfile.ZipFile(resolved_zip_path, 'r') as zip_file:
                        zip_contents = zip_file.namelist()
                        print(f"📋 ZIP contents (first 5 files):")
                        for i, file_in_zip in enumerate(zip_contents[:5]):
                            print(f"   {i+1}. {file_in_zip}")
                        if len(zip_contents) > 5:
                            print(f"   ... and {len(zip_contents) - 5} more files")
                except Exception as e:
                    print(f"⚠️ Could not list ZIP contents: {e}")
                
                return {
                    "filename": os.path.basename(internal_path) if internal_path else "",
                    "size": 0,
                    "crc32": "",
                    "md5": "",
                    "sha1": "",
                    "path": full_path,
                    "available": False,
                    "error": error_msg,
                    "zip_path": zip_path,
                    "resolved_zip_path": resolved_zip_path,
                    "internal_path": internal_path,
                    "timestamp": int(time.time())
                }
            
            print(f"📁 File found in ZIP: {filename} ({file_size:,} bytes)")

            # CRC32 comes straight from the ZIP central directory (ZipInfo.CRC) —
            # no decompression. ScreenScraper matches on CRC, so this is all the
            # firmware needs, and it returns in milliseconds instead of minutes.
            # MD5/SHA1 are not stored in the directory; we leave them empty (a CRC
            # match is enough). With no payload read there is also no SD/CPU
            # contention with the core load, so the load watcher isn't needed here.
            crc32 = format(zip_crc & 0xffffffff, '08X')
            md5 = ""
            sha1 = ""
            print(f"🔢 CRC32 from ZIP directory: {crc32} (no decompression)")

            # Return successful result
            result = {
                "filename": filename,
                "size": file_size,
                "crc32": crc32,
                "md5": md5,
                "sha1": sha1,
                "path": full_path,
                "available": True,
                "hash_calculated": len(crc32) > 0,
                "file_too_large": False,
                "zip_path": zip_path,
                "resolved_zip_path": resolved_zip_path,
                "internal_path": internal_path,
                "timestamp": int(time.time())
            }

            print(f"✅ ZIP ROM extraction successful!")
            print(f"📊 Result: {filename}, CRC32={crc32}, Size={file_size:,}")
            return result
            
        except Exception as e:
            error_msg = f"ZIP processing error: {str(e)}"
            print(f"❌ {error_msg}")
            
            return {
                "filename": os.path.basename(internal_path) if internal_path else "",
                "size": 0,
                "crc32": "",
                "md5": "",
                "sha1": "",
                "path": full_path,
                "available": False,
                "error": error_msg,
                "zip_path": zip_path,
                "resolved_zip_path": resolved_zip_path if 'resolved_zip_path' in locals() else None,
                "internal_path": internal_path,
                "timestamp": int(time.time())
            }

    # ========== HTTP RESPONSE HELPERS ==========
    
    def send_text_response(self, data):
        body = str(data).encode('utf-8')
        self.send_response(200)
        self.send_header('Content-type', 'text/plain')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(body)

    def send_index_page(self):
        """Friendly landing page for humans hitting the server root.

        The display never calls '/'; this exists only so that a manual
        connectivity test (typing the server URL into a browser) returns
        something reassuring instead of a 404 that looks like a failure.
        """
        endpoints = [
            ('/status/core', 'Active core'),
            ('/status/game', 'Active game'),
            ('/status/rom', 'Loaded ROM'),
            ('/status/rom/details', 'ROM details (CRC, hash, path)'),
            ('/status/system', 'CPU, memory, uptime'),
            ('/status/storage', 'SD / USB storage'),
            ('/status/network', 'Network status'),
            ('/status/usb', 'USB devices'),
            ('/status/session', 'Session statistics'),
            ('/status/all', 'All data combined'),
            ('/status/unknown_cores', 'Cores this MiSTer ran that we cannot name'),
        ]
        rows = ''.join(
            f'<li><a href="{p}">{p}</a> — {d}</li>' for p, d in endpoints
        )
        html = (
            '<!DOCTYPE html><html><head><meta charset="utf-8">'
            '<title>MiSTer Monitor</title></head><body>'
            '<h1>MiSTer Monitor server</h1>'
            '<p>The server is running. Available endpoints:</p>'
            f'<ul>{rows}</ul>'
            '</body></html>'
        )
        body = html.encode('utf-8')
        self.send_response(200)
        self.send_header('Content-type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(body)
    
    def send_json_response(self, data):
        body = json.dumps(data).encode('utf-8')
        self.send_response(200)
        self.send_header('Content-type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(body)
    
    def send_error_response(self, code, message):
        body = message.encode('utf-8')
        self.send_response(code)
        self.send_header('Content-type', 'text/plain')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(body)

if __name__ == '__main__':
    try:
        _start_watcher()
        _start_discovery_responder()
        if _RA_AVAILABLE:
            try:
                start_ra_polling(lambda: (_state, _state_lock))
            except Exception as e:
                print(f"ℹ️ RA polling not started: {e}")
        server = ThreadingHTTPServer(('', 8081), MiSTerStatusHandler)
        print("MiSTer Monitor Status Server v2 - port 8081")
        print("Endpoints:")
        print("  /status/core         - Active core")
        print("  /status/game         - Active game")
        print("  /status/rom          - Loaded ROM")
        print("  /status/rom/details  - ROM details (CRC, hash, path)")
        print("  /status/system       - CPU, memory, uptime")
        print("  /status/storage      - SD/USB storage")
        print("  /status/network      - Network status")
        print("  /status/usb          - USB devices")
        print("  /status/session      - Session statistics")
        print("  /status/all          - All data combined")
        print("  /status/retroachievements - RA progress for active game")
        print("  /status/retroachievements/event - unlock counter micro-poll")
        print("  /status/retroachievements/achievements - trophy list (?page=N&per=M)")
        print("  /status/unknown_cores - cores this MiSTer ran that we cannot name")
        print("")
        server.serve_forever()
    except Exception as e:
        print(f"Error starting server: {e}")
        import traceback
        traceback.print_exc()

