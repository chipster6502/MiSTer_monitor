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
from urllib.parse import urlparse

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
        print("ℹ️ names.txt not found, using CORE_NAME_MAPPING only")
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
    'svi318', 'fmtowns', 'amiga', 'ao486', 'pcxt',
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
    'AO486': 'PC Dos',
    'PCXT': 'PC Dos',
    'PCjr': 'PC Dos',
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
    'CD-i': 'Phillips CD-i',
    'MegaDuck': 'Mega Duck',
    'NEOGEO': 'Neo-Geo',
    'NeoGeo-CD': 'Neo-Geo CD',
    'NeoGeoPocket': 'Neo-Geo Pocket',
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
}

# names.txt fills in cores not already in CORE_NAME_MAPPING
for k, v in NAMES_TXT.items():
    if k not in CORE_NAME_MAPPING:
        CORE_NAME_MAPPING[k] = v

# Set of all known system friendly names — used to detect CURRENTPATH = core name
KNOWN_SYSTEM_NAMES = set(v.lower() for v in CORE_NAME_MAPPING.values()) | \
                     set(v.lower() for v in NAMES_TXT.values())

# Case-insensitive lookup dict — keys are lowercased
CORE_NAME_MAPPING_LOWER = {k.lower(): v for k, v in CORE_NAME_MAPPING.items()}

import threading

# ---------------------------------------------------------------------------
# Centralized state. All access must hold _state_lock.
# ---------------------------------------------------------------------------
_state_lock = threading.Lock()

_state = {
    'core':              'Menu',   # friendly name — used for display, image lookup, and ScreenScraper mapping
    'system_name':       'Menu',   # alias of 'core' (same value); kept for backward compatibility
    'game':              '',       # game name (filename without extension)
    'game_path':         '',       # absolute path to ROM file
    'is_arcade':         False,    # True if current core is arcade
    'rom_details':       None,     # last ScreenScraper result (dict or None)
    'rom_details_stale': True,     # True = needs refresh on next request
}

# Error tracking — exposed via /status/error_state and /status/all
server_error_state        = ''    # last error message, empty string if none
last_valid_core           = ''    # last corename that produced a valid state
last_valid_core_timestamp = 0.0   # epoch time of last valid state update

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
    '.nes', '.sfc', '.smd', '.md', '.gba', '.gb', '.gbc',
    '.a78', '.a52', '.a26', '.n64', '.z64', '.pce', '.cue',
    '.lnx', '.ngp', '.ngc', '.ws', '.wsc', '.sg', '.sms',
    '.gg', '.col', '.vec', '.int', '.psx', '.img',
    # 8-bit computers
    '.prg', '.d64', '.t64', '.tap', '.crt', '.g64',  # Commodore
    '.atr', '.xex', '.cas', '.car',                   # Atari 8bit
    '.dsk', '.st', '.msa', '.stx', '.dim',            # Atari ST
    '.tzx', '.tap', '.z80', '.sna', '.trd', '.scl',  # Spectrum
    '.cdt', '.cpc', '.voc',                           # Amstrad CPC
    '.vhd', '.hdf', '.adf', '.adz',                   # Amiga
    '.do', '.po', '.2mg',                             # Apple II
    '.mx1', '.mx2',                                   # MSX
    '.col', '.cv',                                     # ColecoVision
    '.m3u',                                            # playlists
}

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

    # --- Navigation vs real load ---
    # MiSTer writes FILESELECT and CURRENTPATH at the exact same nanosecond
    # during OSD navigation. After a real load, only FILESELECT is updated.
    fs_ns  = _get_mtime_ns('/tmp/FILESELECT')
    cp_ns  = _get_mtime_ns('/tmp/CURRENTPATH')
    is_navigation = (fs_ns == cp_ns)

    if is_navigation:
        # User is browsing OSD — keep current state unchanged
        print("🔀 OSD navigation detected — state unchanged")
        return

    # --- SAM detection (takes priority if active and current) ---
    if _sam_is_current():
        sam_active, sam_core_raw, sam_core_friendly, sam_game, sam_path = _sam_get_current()
        if sam_active and sam_core_raw:
            print(f"🎮 SAM active — core='{sam_core_friendly}' game='{sam_game}'")
            with _state_lock:
                _state['core']              = sam_core_friendly  # friendly — for display and image lookup
                _state['system_name']       = sam_core_friendly
                _state['game']              = sam_game
                _state['game_path']         = sam_path
                _state['is_arcade']         = False
                _state['rom_details']       = None
                _state['rom_details_stale'] = True
            return

    # --- Menu ---
    if not corename or corename.upper() == 'MENU':
        print("📋 MENU detected")
        with _state_lock:
            _state['core']              = 'Menu'
            _state['system_name']       = 'Menu'
            _state['game']              = ''
            _state['game_path']         = ''
            _state['is_arcade']         = False
            _state['rom_details']       = None
            _state['rom_details_stale'] = True
        return

    # --- Resolve friendly core name ---
    friendly_name = (CORE_NAME_MAPPING.get(corename) or
                    CORE_NAME_MAPPING_LOWER.get(corename.lower()) or
                    corename)

    # --- Arcade detection ---
    ARCADE_FRESHNESS = 30  # seconds
    corename_ts   = _get_mtime_ns('/tmp/CORENAME') / 1e9
    activegame_ts = _get_mtime_ns('/tmp/ACTIVEGAME') / 1e9

    activegame_arcade_fresh = (
        activegame and
        '/_Arcade/' in activegame and
        activegame_ts >= corename_ts - ARCADE_FRESHNESS
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

    elif fullpath and 'arcade' in fullpath.lower() and not _is_known_non_arcade(corename):
        # Arcade launched via OSD
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

    with _state_lock:
        _state['core']        = 'Arcade' if is_arcade else friendly_name  # friendly — for display and image lookup
        _state['system_name'] = 'Arcade' if is_arcade else friendly_name
        _state['game']              = game_name
        _state['game_path']         = game_path
        _state['is_arcade']         = is_arcade
        _state['rom_details']       = None
        _state['rom_details_stale'] = True

    print(f"✅ State updated: core='{_state['core']}' game='{game_name}' arcade={is_arcade}")

_last_event_time = 0.0
_DEBOUNCE_SECONDS = 0.3

def _watcher_thread():
    """
    Runs inotifywait in monitor mode and reacts to filesystem events.
    Calls _update_state() whenever a relevant file changes.
    Restarts automatically if inotifywait dies unexpectedly.
    """
    print("👁️ Watcher thread started")
    while True:
        try:
            proc = subprocess.Popen(
                ['inotifywait', '-m', '-e', 'close_write,create'] + _WATCHED_FILES,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True
            )
            for line in proc.stdout:
                line = line.strip()
                if not line:
                    continue
                print(f"📂 inotify event: {line}")

                # CORENAME changes always trigger _update_state immediately
                # Other events are debounced to avoid noise during navigation
                is_corename = '/tmp/CORENAME' in line
                
                # Debounce: ignore events that arrive too close together
                now = time.time()
                global _last_event_time
                if not is_corename and (now - _last_event_time < _DEBOUNCE_SECONDS):
                    print(f"⏱️ Debounced")
                    _last_event_time = now
                    continue
                _last_event_time = now
                
                _update_state()
            proc.wait()
        except Exception as e:
            print(f"⚠️ Watcher thread error: {e}")
        print("🔄 Watcher thread restarting...")
        time.sleep(1)


def _start_watcher():
    """Starts the background watcher thread as a daemon."""
    t = threading.Thread(target=_watcher_thread, daemon=True)
    t.start()

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
        
        # Endpoints principales
        if path == '/status/core':
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
                'timestamp': int(time.time())
            }
            self.send_json_response(status)
        else:
            self.send_error_response(404, 'Endpoint not found')

    # ========== OPTIMIZED CORE FUNCTIONS ==========
    
    def get_current_core(self):
        """Returns the currently active core friendly name from centralized state."""
        with _state_lock:
            return _state['core']
        
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

    def detect_arcade_name_similarity(self, corename, activegame_path):
        """
        Detecta similitudes entre CORENAME y nombre de archivo arcade
        Para resolver conflictos MiSTer nativo vs interfaz web
        
        Returns: (is_similar, confidence_score)
        """
        if not corename or not activegame_path:
            return False, 0.0
        
        # Extraer nombre del archivo .mra
        arcade_filename = os.path.splitext(os.path.basename(activegame_path))[0]
        
        # Clean names for comparison
        corename_clean = re.sub(r'[^a-z0-9]', '', corename.lower())
        arcade_clean = re.sub(r'[^a-z0-9]', '', arcade_filename.lower())
        
        print(f"🔍 Comparing: '{corename}' (clean: '{corename_clean}') vs '{arcade_filename}' (clean: '{arcade_clean}')")
        
        # Criterio 1: Coincidencia exacta
        if corename_clean == arcade_clean:
            print(f"✅ Exact match found")
            return True, 1.0
        
        # Criterio 2: CORENAME es prefijo significativo
        if len(corename_clean) >= 4 and arcade_clean.startswith(corename_clean):
            confidence = len(corename_clean) / len(arcade_clean)
            print(f"✅ Prefix match found (confidence: {confidence:.2f})")
            return True, confidence
        
        # Criterio 3: Subcadenas comunes
        if len(corename_clean) >= 6:
            common_chars = 0
            for char in corename_clean:
                if char in arcade_clean:
                    common_chars += 1
            
            coverage = common_chars / len(corename_clean)
            if coverage >= 0.7:  # 70% de caracteres comunes
                print(f"✅ Character similarity found (coverage: {coverage:.2f})")
                return True, coverage
        
        # Criterion 4: Remove common suffixes
        # Simplified version without complex regex
        suffixes_to_remove = ['m72', 'cps1', 'cps2', 'neogeo', 'world', 'usa', 'japan']
        
        corename_base = corename_clean
        arcade_base = arcade_clean
        
        for suffix in suffixes_to_remove:
            if corename_base.endswith(suffix):
                corename_base = corename_base[:-len(suffix)]
            if arcade_base.endswith(suffix):
                arcade_base = arcade_base[:-len(suffix)]
        
        if len(corename_base) >= 4 and len(arcade_base) >= 4:
            if corename_base == arcade_base or arcade_base.startswith(corename_base):
                print(f"✅ Base name match found: '{corename_base}' vs '{arcade_base}'")
                return True, 0.8
        
        print(f"❌ No significant similarity found")
        return False, 0.0

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
        Gets the current ROM using multiple methods
        """
        # Method 1: Read ACTIVEGAME (priority)
        try:
            with open('/tmp/ACTIVEGAME', 'r') as f:
                content = f.read().strip()
                if content:
                    return os.path.basename(content)
        except:
            pass
        
        # Method 2: Read SAM_Game.txt
        try:
            with open('/tmp/SAM_Game.txt', 'r') as f:
                content = f.read().strip()
                if content:
                    return os.path.basename(content)
        except:
            pass
        
        # Method 3: Parse SAM_Game.mgl
        try:
            with open('/tmp/SAM_Game.mgl', 'r') as f:
                content = f.read()
                match = re.search(r'<file[^>]*>([^<]+)</file>', content)
                if match:
                    file_path = match.group(1)
                    return os.path.basename(file_path)
        except:
            pass
        
        # Method 4: Search for LASTGAME/LASTROM files
        try:
            game_patterns = ['/tmp/LASTGAME*', '/tmp/LASTROM*', '/tmp/*ROM*']
            for pattern in game_patterns:
                games = glob.glob(pattern)
                if games:
                    latest_file = max(games, key=os.path.getctime)
                    try:
                        with open(latest_file, 'r') as f:
                            content = f.read().strip()
                            if content:
                                return os.path.basename(content)
                    except:
                        continue
        except:
            pass
        
        return "Sin ROM"

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
    
    def get_file_from_zip_enhanced(self, zip_path, internal_path):
        """
        ENHANCED: Extract file content from ZIP with multiple search strategies
        """
        try:
            print(f"📂 Reading from ZIP: {internal_path}")
            
            with zipfile.ZipFile(zip_path, 'r') as zip_file:
                zip_files = zip_file.namelist()
                
                # Strategy 1: Exact match
                if internal_path in zip_files:
                    print(f"✅ Exact match: {internal_path}")
                    with zip_file.open(internal_path) as file_in_zip:
                        return file_in_zip.read()
                
                # Strategy 2: Path separator variants
                variants = [
                    internal_path.replace('\\', '/'),
                    internal_path.replace('/', '\\'),
                    internal_path.replace('\\', '/').lstrip('/'),
                    internal_path.replace('/', '\\').lstrip('\\')
                ]
                
                for variant in variants:
                    if variant in zip_files:
                        print(f"✅ Variant match: {variant}")
                        with zip_file.open(variant) as file_in_zip:
                            return file_in_zip.read()
                
                # Strategy 3: Case-insensitive search
                internal_lower = internal_path.lower()
                for zip_file_path in zip_files:
                    if zip_file_path.lower() == internal_lower:
                        print(f"✅ Case-insensitive match: {zip_file_path}")
                        with zip_file.open(zip_file_path) as file_in_zip:
                            return file_in_zip.read()
                
                # Strategy 4: Filename-only search
                filename = os.path.basename(internal_path)
                filename_lower = filename.lower()
                
                for zip_file_path in zip_files:
                    if os.path.basename(zip_file_path).lower() == filename_lower:
                        print(f"✅ Filename match: {zip_file_path}")
                        with zip_file.open(zip_file_path) as file_in_zip:
                            return file_in_zip.read()
                
                # Strategy 5: Stem match — must mirror get_zip_file_info_enhanced,
                # otherwise the size lookup succeeds but the content read fails.
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
                    print(f"✅ Stem match: {chosen}")
                    with zip_file.open(chosen) as file_in_zip:
                        return file_in_zip.read()
                
                # Show debug info
                print(f"❌ File not found. Searched for: {internal_path}")
                print(f"📋 Available files (first 10):")
                for i, zf in enumerate(zip_files[:10]):
                    print(f"   {i+1}. {zf}")
                
                return None
                
        except Exception as e:
            print(f"❌ ZIP read error: {e}")
            return None
    
    def get_zip_file_info_enhanced(self, zip_path, internal_path):
        """
        ENHANCED: Get file info from ZIP with multiple search strategies
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
                        return filename, info.file_size
                
                # Case-insensitive search
                internal_lower = internal_path.lower()
                for zip_file_path in zip_files:
                    if zip_file_path.lower() == internal_lower:
                        info = zip_file.getinfo(zip_file_path)
                        filename = os.path.basename(zip_file_path)
                        print(f"✅ File info (case-insensitive): {filename} ({info.file_size:,} bytes)")
                        return filename, info.file_size
                
                # Filename-only search
                target_filename = os.path.basename(internal_path).lower()
                for zip_file_path in zip_files:
                    if os.path.basename(zip_file_path).lower() == target_filename:
                        info = zip_file.getinfo(zip_file_path)
                        filename = os.path.basename(zip_file_path)
                        print(f"✅ File info (filename): {filename} ({info.file_size:,} bytes)")
                        return filename, info.file_size
                
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
                    return filename, info.file_size
                
                print(f"❌ File info not found: {internal_path}")
                return None, 0
            
        except Exception as e:
            print(f"❌ ZIP info error: {e}")
            return None, 0
    
    def get_rom_details(self):
        """
        Returns ROM details (CRC, hashes, path).
        Uses _state['rom_details'] as cache — refreshed when rom_details_stale is True.
        """
        print(f"[{time.strftime('%H:%M:%S')}] Getting ROM details...")

        with _state_lock:
            stale   = _state['rom_details_stale']
            cached  = _state['rom_details']

        if not stale and cached is not None:
            print("📋 Using cached ROM details")
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

        with _state_lock:
            _state['rom_details']       = result
            _state['rom_details_stale'] = False

        return result
    
    def get_rom_details_forced(self):
        """
        Forced ROM details: bypasses game-name detection and timestamp checks.
        Goes directly to _get_non_arcade_rom_path() / _get_arcade_rom_path()
        so that RESCAN GAME works even when FILESELECT timestamps are stale.
        """
        print("🔄 === FORCED ROM DETAILS (bypass timestamp check) ===")
        try:
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
                return {
                    "filename": "", "size": 0, "crc32": "", "md5": "", "sha1": "",
                    "path": "", "available": False,
                    "error": "Forced scan: no ROM path found via CURRENTPATH/ACTIVEGAME",
                    "detection_method": "forced_none", "timestamp": int(time.time())
                }

            print(f"🔄 Forced path resolved: {rom_path}")
            is_zip, zip_path, internal_path = self.is_zip_path(rom_path)
            if is_zip:
                result = self.get_rom_details_from_zip(rom_path, zip_path, internal_path)
            else:
                result = self.get_rom_details_from_file(rom_path)

            result["detection_method"] = "forced"
            # Update cache so subsequent normal calls benefit from this result
            with _state_lock:
                _state['rom_details']       = result
                _state['rom_details_stale'] = False
            return result

        except Exception as e:
            import traceback
            traceback.print_exc()
            return {
                "filename": "", "size": 0, "crc32": "", "md5": "", "sha1": "",
                "path": "", "available": False,
                "error": f"Forced scan error: {str(e)}",
                "detection_method": "forced_error", "timestamp": int(time.time())
            }

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
        # This is the standard MiSTer pattern for games inside ZIP collections:
        #   FULLPATH  = "games/Apple-II/Collection.zip/"  (directory context with ZIP)
        #   CURRENTPATH = "221B Baker Street.do"           (just the filename)
        # → combined = "games/Apple-II/Collection.zip/221B Baker Street.do"
        if currentpath and not os.path.dirname(currentpath) and fullpath:
            fullpath_dir = fullpath.rstrip('/')
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
                    if os.path.exists(final_path):
                        print(f"✅ ROM file found via {source_name}: {final_path}")
                        return final_path
                    else:
                        print(f"❌ Direct file not found: {final_path}")

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

                        print(f"❌ No valid path found via {source_name} - trying next source")
                        continue

            except Exception as e:
                print(f"❌ Error resolving {source_name}: {e} - trying next source")
                continue

        print(f"❌ No valid ROM path found from any source")
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
            
            # Calculate hashes (only for files < 100MB for performance)
            crc32 = ""
            md5 = ""
            sha1 = ""
            
            # Size limit to avoid blocking server with very large files
            MAX_SIZE_FOR_HASH = 100 * 1024 * 1024  # 100MB
            
            if file_size <= MAX_SIZE_FOR_HASH:
                try:
                    start_time = time.time()
                    print(f"Calculating hashes for {filename}...")
                    
                    with open(rom_path, 'rb') as f:
                        # Read file in chunks to avoid saturating memory
                        chunk_size = 64 * 1024  # 64KB chunks
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
            filename, file_size = self.get_zip_file_info_enhanced(resolved_zip_path, internal_path)
            
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
            
            # Calculate hashes
            crc32 = ""
            md5 = ""
            sha1 = ""
            
            MAX_SIZE_FOR_HASH = 100 * 1024 * 1024  # 100MB
            
            if file_size <= MAX_SIZE_FOR_HASH:
                try:
                    start_time = time.time()
                    print(f"🔢 Calculating hashes for {filename}...")
                    
                    # Get file content from ZIP
                    file_content = self.get_file_from_zip_enhanced(resolved_zip_path, internal_path)
                    
                    if file_content is None:
                        raise Exception("Could not read file from ZIP")
                    
                    # Calculate hashes
                    import zlib
                    import hashlib
                    
                    crc32_calc = zlib.crc32(file_content)
                    md5_calc = hashlib.md5(file_content)
                    sha1_calc = hashlib.sha1(file_content)
                    
                    crc32 = format(crc32_calc & 0xffffffff, '08X')
                    md5 = md5_calc.hexdigest().upper()
                    sha1 = sha1_calc.hexdigest().upper()
                    
                    calc_time = time.time() - start_time
                    print(f"✅ Hashes calculated in {calc_time:.2f}s")
                    print(f"   CRC32: {crc32}")
                    print(f"   MD5: {md5}")
                    print(f"   SHA1: {sha1}")
                    
                except Exception as e:
                    error_msg = f"Hash calculation failed: {str(e)}"
                    print(f"❌ {error_msg}")
                    
                    # Return partial success
                    return {
                        "filename": filename,
                        "size": file_size,
                        "crc32": "",
                        "md5": "",
                        "sha1": "",
                        "path": full_path,
                        "available": True,
                        "hash_calculated": False,
                        "error": error_msg,
                        "zip_path": zip_path,
                        "resolved_zip_path": resolved_zip_path,
                        "internal_path": internal_path,
                        "timestamp": int(time.time())
                    }
            else:
                print(f"⚠️ File too large for hash calculation: {file_size:,} bytes")
            
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
                "file_too_large": file_size > MAX_SIZE_FOR_HASH,
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
        self.send_response(200)
        self.send_header('Content-type', 'text/plain')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(str(data).encode('utf-8'))
    
    def send_json_response(self, data):
        self.send_response(200)
        self.send_header('Content-type', 'application/json')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(json.dumps(data).encode('utf-8'))
    
    def send_error_response(self, code, message):
        self.send_response(code)
        self.send_header('Content-type', 'text/plain')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(message.encode('utf-8'))

if __name__ == '__main__':
    try:
        _start_watcher()
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
        print("")
        server.serve_forever()
    except Exception as e:
        print(f"Error starting server: {e}")
        import traceback
        traceback.print_exc()

