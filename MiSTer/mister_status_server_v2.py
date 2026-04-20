#!/usr/bin/env python3
"""
MiSTer Status Server - COMPLETE OPTIMIZED VERSION
Simplified arcade detection logic with all original functions
"""

from http.server import HTTPServer, BaseHTTPRequestHandler
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

last_valid_core = "Menu"  # Store last valid active core
last_valid_core_timestamp = 0  # When it was last updated
server_error_state = ""   # Current error state if any
server_initial_timestamps = {}
server_last_timestamps = {}
server_started = False

# Global cache state that persists across requests
global_cache_state = {
    'core': None,
    'game': None, 
    'rom_details': None
}

global_is_first_call = {
    'core': True,
    'game': True,
    'rom_details': True
}

global_has_valid_cache = {
    'core': False,
    'game': False,
    'rom_details': False
}

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

import threading

# ---------------------------------------------------------------------------
# Centralized state. All access must hold _state_lock.
# ---------------------------------------------------------------------------
_state_lock = threading.Lock()

_state = {
    'core':              'Menu',   # friendly core name
    'system_name':       'Menu',   # friendly system name
    'game':              '',       # game name (filename without extension)
    'game_path':         '',       # absolute path to ROM file
    'is_arcade':         False,    # True if current core is arcade
    'rom_details':       None,     # last ScreenScraper result (dict or None)
    'rom_details_stale': True,     # True = needs refresh on next request
}

# ---------------------------------------------------------------------------
# Background watcher thread — monitors /tmp/ files via inotifywait
# ---------------------------------------------------------------------------
_WATCHED_FILES = [
    '/tmp/CORENAME',
    '/tmp/ACTIVEGAME',
    '/tmp/CURRENTPATH',
    '/tmp/FILESELECT',
    '/tmp/FULLPATH',
]

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
                # Step 4 will call _update_state() here
            proc.wait()
        except Exception as e:
            print(f"⚠️ Watcher thread error: {e}")
        print("🔄 Watcher thread restarting...")
        time.sleep(1)


def _start_watcher():
    """Starts the background watcher thread as a daemon."""
    t = threading.Thread(target=_watcher_thread, daemon=True)
    t.start()

class MiSTerStatusHandler(BaseHTTPRequestHandler):
    
    def __init__(self, *args, **kwargs):
        global server_initial_timestamps, server_started, server_last_timestamps
        
        if not server_started:
            print("🏁 First server initialization - capturing initial timestamps")
            server_initial_timestamps = self._initialize_timestamps()
            server_last_timestamps = server_initial_timestamps.copy()  # ✅ INITIALIZE
            server_started = True
            print(f"🕐 SERVER START timestamps: {server_initial_timestamps}")
        else:
            print(f"🔄 Request handler init - using existing timestamps: {server_initial_timestamps}")
        
        # ✅ USE GLOBAL REFERENCE instead of copying
        self.last_timestamps = server_last_timestamps  # ✅ REFERENCE, NOT A COPY
        
        # Variables de instancia
        self.session_start = time.time()
        self.requests_count = 0
        self.last_significant_change = 0
        self.cached_game_state = global_cache_state['game']
        self.cached_core_state = global_cache_state['core']
        self.cached_rom_details = global_cache_state['rom_details']
        self.is_first_call = global_is_first_call
        self.has_valid_cache = global_has_valid_cache
        
        # Lista completa de sistemas conocidos no-arcade
        self.KNOWN_NON_ARCADE_SYSTEMS = [
            # === Consolas Nintendo ===
            'nes', 'nintendo', 'famicom', 'snes', 'super nintendo', 'n64', 'nintendo64',
            'gameboy', 'gbc', 'gba', 'fds', 'sgb',
            
            # === Consolas Sega ===
            'genesis', 'megadrive', 'sega', 'mastersystem', 'sms', 'gamegear', 'gg',
            'saturn', 'dreamcast', 'megacd', 'segacd', 's32x', 'sg1000',
            
            # === Consolas Sony/Otras ===
            'psx', 'playstation', 'ps1',
            
            # === Consolas Atari ===
            'atari2600', 'atari5200', 'atari7800', 'atarilynx', 'atari800', 'atarist',
            
            # === Classic consoles ===
            'colecovision', 'intellivision', 'vectrex', 'odyssey2', 'channelf', 
            'astrocade', 'creativision', 'tutor', 'supervision', 'gamate', 'pokemonmini',
            
            # === MSX y ordenadores japoneses ===
            'msx', 'msx1', 'msx2', 'msx2plus', 'x68000', 'pc8801', 'sharp', 'x1', 'pc88', 'mz',
            
            # === PC Engine / TurboGrafx ===
            'turbografx16', 'pcengine', 'tgfx16', 'tgfx16cd', 'supergrafx',
            
            # === Handhelds ===
            'wonderswan', 'wonderswancolor', 'ngp', 'ngpc', 'gamegear',
            
            # === Ordenadores europeos ===
            'gx4000', 'amstradcpc', 'amstrad', 'cpc6128', 'zx48', 'zxspectrum', 'zx81', 'zx80',
            'oric', 'bbcmicro', 'acorn', 'electron', 'archimedes', 'enterprise', 'samcoupe',
            'aquarius', 'microbee', 'atom', 'laser500',
            
            # === Ordenadores americanos ===
            'vic20', 'c64', 'c128', 'c16', 'plus4', 'pet2001', 'ti99', 'trs80', 'coco', 'dragon', 'mc10',
            'trs80coco2', 'coleco', 'adam', 'apple2', 'applei', 'macplus',
            
            # === Sistemas diversos ===
            'svi318', 'creativision', 'tutor', 'laser500', 'fmtowns', 'amiga', 'ao486', 'pcxt',
            
            # === Names from SAM (lowercase) ===
            'amiga', 'amigacd32', 'ao486', 'atari2600', 'atari5200', 'atari7800', 
            'atarilynx', 'c64', 'fds', 'gb', 'gbc', 'gba', 'genesis', 'megacd', 
            'n64', 'neogeo', 's32x', 'saturn', 'sms', 'snes', 'tgfx16', 'tgfx16cd', 
            'psx', 'x68k',
            
            # === Sistemas adicionales ===
            'APOGEE', 'ARCHIE', 'AY-3-8500', 
            'AcornElectron', 'Adam', 'Altair8800', 'Amstrad PCW', 
            'BBCMicro', 'BK0011M', 'Casio_PV-2000', 'COCO3', 'CoCo2', 'EDSAC', 
            'EpochGalaxyII', 'Galaksija', 'Interact', 
            'Laser', 'Lynx48', 'MultiComp', 'ORAO', 'Ondra_SPO186', 'Oric', 
            'PMD85', 'RX78', 
            'Sord M5', 'SuperVision', 'TI-99_4A', 'TRS-80', 'TSConf', 
            'TatungEinstein', 'TomyScramble', 'UK101', 'VECTOR06', 
            'Homelab', 'BBCBridgeCompanion', 'PocketChallengeV2', 'MyVision', 
            'SuperVision8000', 'VT52', 'CreatiVision',
            
            # === Consolas menos comunes completas ===
            'Atari2600', 'ATARI5200', 'ATARI7800', 'ATARI800', 'AtariST', 
            'WonderSwan', 'WonderSwanColor', 'Saturn', 'FDS', 'SGB', 
            'VECTREX', 'Coleco', 'Intellivision', 'ODYSSEY2', 'ChannelF', 
            'Astrocade', 'Gamate', 'PokemonMini', 'SG1000', 
            'SG-1000', 'TomyTutor', 'SCV', 'SuperGrafx', 'PDP1', 

            # === Ordenadores completos ===
            'C64', 'C16', 'C128', 'VIC20', 'Amiga', 'AO486', 'PCXT', 'Amstrad', 
            'Spectrum', 'ZX81', 'ZXNext', 'zx48', 'MSX', 'MSX1', 'X68000', 
            'Apple-II', 'APPLE-I', 'MACPLUS', 'SAM', 'SAMCOUPE',
        ]
        
        # Mapeo completo de nombres de cores
        self.CORE_NAME_MAPPING = {
            # === Nintendo ===
            'NES': 'Nintendo Entertainment System',
            'SNES': 'Super Nintendo Entertainment System', 
            'N64': 'Nintendo 64',
            'FDS': 'Family Computer Disk System',
            'GB': 'Nintendo Game Boy',
            'GBC': 'Game Boy Color',
            'GBA': 'Game Boy Advance',
            'GBA2P': 'Game Boy Advance',
            'SGB': 'Super Game Boy',
            'GameNWatch' : 'Game & Watch',
            'GAMEBOY2P' : 'Game Boy Color',
            
            # === Sega ===
            'Genesis': 'Sega Genesis/Mega Drive',
            'MegaDrive': 'Megadrive',
            'SMS': 'Master System',
            'GG': 'Sega Game Gear',
            'Saturn': 'Sega Saturn',
            'S32X': 'Megadrive 32X',
            'MegaCD': 'Mega-CD',
            'SegaCD': 'Sega CD/Mega CD',
            'SG1000': 'SG-1000',
            'GameGear' : 'Game Gear',
            
            # === Sony ===
            'PSX': 'PlayStation',
            'PlayStation': 'Sony PlayStation',
            
            # === PC Engine / TurboGrafx ===
            'TurboGrafx16': 'TurboGrafx-16/PC Engine',
            'PCEngine': 'TurboGrafx-16/PC Engine',
            'TGFX16': 'PC Engine',
            'TGFX16-CD': 'PC Engine CD-Rom',
            'SuperGrafx': 'PC Engine SuperGrafx',
            
            # === Atari ===
            'Atari2600': 'Atari 2600',
            'ATARI5200': 'Atari 5200',
            'ATARI7800': 'Atari 7800',
            'AtariLynx': 'Lynx',
            'ATARI800': 'Atari 8bit',
            'AtariST': 'Atari ST',
            
            # === Arcade ===
            'MAME': 'Multiple Arcade Machine Emulator',
            'mame': 'Multiple Arcade Machine Emulator',
            'Arcade': 'Multiple Arcade Machine Emulator',
            
            # === Ordenadores ===
            'PET2001' : 'PET',
            'C64': 'Commodore 64',
            'C128': 'Commodore 128',
            'VIC20': 'Vic-20',
            'Minimig': 'Commodore Amiga',
            'AO486': 'PC Dos',
            'PCXT': 'PC Dos',
            'Jupiter' : 'Jupiter Ace',
            'PC8801' : 'NEC PC-8801',
            'BK0011M' : "BK",
            'eg2000' : 'EG2000 Colour Genie',
            'lynx48' : 'Camputers Lynx',
            'AQUARIUS' : "Mattel Aquarius",
            'sharpmz' : 'SHARP MZ Series',
            'QL' : 'Sinclair QL',
            'SPMX' : 'Specialist MX',
            'SVI328' : 'Spectravideo SVI-328',
            'AliceMC10' : 'Alice 4K / Tandy MC-10',            
            
            # === MSX ===
            'MSX': 'MSX',
            'MSX1': 'MSX',
            'MSX2': 'MSX2 Computer',
            'MSX2Plus': 'MSX2+ Computer',
            
            # === Spectrum ===
            'Spectrum': 'ZX Spectrum',
            'zx48': 'ZX Spectrum',
            'ZX81': 'ZX81',
            'ZXNext': 'ZX Spectrum Next',
            
            # === Amstrad ===
            'Amstrad': 'CPC',
            'AmstradCPC': 'Amstrad CPC',
            'GX4000': 'Amstrad GX4000',
            
            # === Apple ===
            'Apple-II': 'Apple II',
            'APPLE-I': 'Apple I',
            'MACPLUS': 'Mac OS',
            
            # === Diversos ===
            'X68000': 'Sharp X68000',
            'Coleco': 'Colecovision',
            'Intellivision': 'Intellivision',
            'VECTREX': 'Vectrex',
            'ODYSSEY2': 'Videopac G7000',
            'ChannelF': 'Channel F',
            'CreatiVision': 'CreatiVision',
            'SuperVision': 'Watara Supervision',
            'WonderSwan': 'WonderSwan',
            'WonderSwanColor': 'WonderSwan Color',
            'NGP': 'Neo Geo Pocket',
            'NGPC': 'Neo Geo Pocket Color',
            'PokemonMini': 'Pokemon Mini',
            'Gamate': 'Bit Corporation Gamate',
            'AVision' : 'Adventure Vision',
            'Arcadia' : 'Arcadia 2001',
            'CD-i' : 'CD-i',
            'MegaDuck' : 'Mega Duck',
            'NEOGEO' : 'Neo-Geo',
            'NeoGeo-CD' : 'Neo-Geo CD',
            'NeoGeoPocket' : 'Neo-Geo Pocket',
            'Casio_PV-1000' : 'PV-1000',
            'VC4000' : 'VC 4000',
            'PocketChallenge' : 'Pocket Challenge V2',
            
            # === BBC/Acorn ===
            'BBCMicro': 'BBC Micro',
            'AcornElectron': 'Electron',
            'ARCHIE': 'Archimedes',
            'AcornAtom' : 'Atom',
            
            # === Texas Instruments ===
            'TI-99_4A': 'TI-99/4A',
            
            # === Tandy/Radio Shack ===
            'TRS-80': 'TRS-80 Color Computer',
            'COCO3': 'TRS-80 Color Computer 3',
            'CoCo2': 'TRS-80 Color Computer 2',
            
            # === SAM specific ===
            'SAM': 'SAM Coupé',
            'SAMCOUPE': 'MGT SAM Coupé',
            
            # === Oric ===
            'Oric': 'Oric 1 / Atmos',
            
            # === Special lowercase mappings ===
            'nes': 'Nintendo Entertainment System',
            'snes': 'Super Nintendo Entertainment System',
            'genesis': 'Sega Genesis/Mega Drive',
            'megadrive': 'Sega Genesis/Mega Drive',
            'gameboy': 'Game Boy',
            'gameboycolor': 'Game Boy Color',
            'gameboyadvance': 'Game Boy Advance',
            'nintendo64': 'Nintendo 64',
            'supernintendo': 'Super Nintendo',
            'playstation': 'PlayStation',
            'commodore64': 'Commodore 64',
            'pcengine': 'PC Engine',
            'turbografx16': 'TurboGrafx-16',
            'mastersystem': 'Master System',
            'atari2600': 'Atari 2600',
        }
        
        self.CORE_NAME_MAPPING.update(NAMES_TXT)
        
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

    def _update_global_cache(self, endpoint_name, value):
        """Update global cache state when local cache is updated"""
        global global_cache_state, global_is_first_call, global_has_valid_cache
        
        global_cache_state[endpoint_name] = value
        global_is_first_call[endpoint_name] = False  # Mark as no longer first call
        global_has_valid_cache[endpoint_name] = True
        
        # Update local references
        if endpoint_name == 'core':
            self.cached_core_state = value
        elif endpoint_name == 'game':
            self.cached_game_state = value
        elif endpoint_name == 'rom_details':
            self.cached_rom_details = value
        
        self.is_first_call[endpoint_name] = False
        self.has_valid_cache[endpoint_name] = True
    
    def _initialize_timestamps(self):
        """Leer timestamps iniciales de archivos"""
        timestamps = {}
        for file_name in ['CORENAME', 'ACTIVEGAME', 'CURRENTPATH', 'FILESELECT']:
            try:
                timestamps[file_name] = os.path.getmtime(f'/tmp/{file_name}')
            except:
                timestamps[file_name] = 0
        
        print(f"🕐 Initial timestamps: {timestamps}")
        return timestamps
    
    def _get_current_timestamps(self):
        """Get current timestamps (helper method)"""
        timestamps = {}
        for file_name in ['CORENAME', 'ACTIVEGAME', 'CURRENTPATH', 'FILESELECT']:
            try:
                timestamps[file_name] = os.path.getmtime(f'/tmp/{file_name}')
            except:
                timestamps[file_name] = 0
        return timestamps

    def do_GET(self):
        """Handle GET requests"""
        self.requests_count += 1
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
        elif path == '/status/debug/game':
            self.send_json_response(self.get_game_debug_info())
        elif path == '/status/debug/files':
            self.send_json_response(self.get_debug_files_info())
        elif path == '/status/rom/details':
            from urllib.parse import parse_qs
            force = parse_qs(parsed_path.query).get('force', ['0'])[0] == '1'
            if force:
                self.send_json_response(self.get_rom_details_forced())
            else:
                self.send_json_response(self.get_rom_details())
        elif path == '/status/system/type':
            self.send_json_response(self.get_system_type_info())
        elif path == '/status/debug/sam':
            self.send_json_response(self.get_sam_debug_info())
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

    def _read_stable_path_sources(self):
        """
        Returns CURRENTPATH and FULLPATH from /tmp.

        Returns:
            (currentpath_str, currentpath_timestamp, fullpath_str, source_label)
        """
        content = ""
        timestamp = 0
        fullpath = ""
        try:
            with open('/tmp/CURRENTPATH', 'r') as f:
                content = f.read().strip()
            timestamp = os.path.getmtime('/tmp/CURRENTPATH')
        except:
            pass
        try:
            with open('/tmp/FULLPATH', 'r') as f:
                fullpath = f.read().strip()
        except:
            pass
        return content, timestamp, fullpath, 'CURRENTPATH'

    def has_significant_change(self):
        """
        Enhanced timestamp checking - SAFE VERSION
        """
        global server_last_timestamps
        
        current_timestamps = {}
        for file_name in ['CORENAME', 'ACTIVEGAME', 'CURRENTPATH', 'FILESELECT']:
            try:
                current_timestamps[file_name] = os.path.getmtime(f'/tmp/{file_name}')
            except:
                current_timestamps[file_name] = 0
        
        # Read ACTIVEGAME content once — used for .ini filter below
        activegame_content = ""
        try:
            with open('/tmp/ACTIVEGAME', 'r') as f:
                activegame_content = f.read().strip()
        except:
            pass

        # Ignore ACTIVEGAME timestamp changes caused by .ini config files
        if activegame_content and activegame_content.lower().endswith('.ini'):
            current_timestamps['ACTIVEGAME'] = server_last_timestamps.get('ACTIVEGAME', 0)

        changed_files = []
        for file_name, current_timestamp in current_timestamps.items():
            last_timestamp = server_last_timestamps.get(file_name, 0)
            if current_timestamp != last_timestamp:
                changed_files.append(file_name)

        server_last_timestamps.update(current_timestamps)

        if not changed_files:
            return False, "no_changes", []

        changed_set = set(changed_files)

        # Helper: compare FILESELECT and CURRENTPATH timestamps at nanosecond precision.
        # During navigation MiSTer writes both files at the exact same nanosecond.
        # After a load, FILESELECT has a newer timestamp than CURRENTPATH.
        def fileselect_matches_currentpath():
            try:
                return (os.stat('/tmp/FILESELECT').st_mtime_ns ==
                        os.stat('/tmp/CURRENTPATH').st_mtime_ns)
            except:
                return True  # if we can't read, assume navigation (safe default)

        # CURRENTPATH alone → pure OSD cursor navigation
        if changed_set == {'CURRENTPATH'}:
            return False, "menu_navigation", changed_files

        # FILESELECT + CURRENTPATH both changed: could be navigation OR a load that
        # happened alongside navigation (missed by infrequent polling).
        # Verify by comparing current timestamps: equal → navigation, different → load.
        if changed_set == {'FILESELECT', 'CURRENTPATH'}:
            if fileselect_matches_currentpath():
                return False, "menu_navigation", changed_files
            else:
                return True, "significant_changes", changed_files

        # Any other change (CORENAME, ACTIVEGAME, FILESELECT alone, etc.): real event
        return True, "significant_changes", changed_files
    
    def _handle_cache_logic(self, endpoint_name, cached_value, has_valid_cache_flag):
        """
        Enhanced cache logic for timestamp-based caching
        FIXED: Menu navigation detection works ALWAYS, not just on first call
        """
        global server_initial_timestamps, global_cache_state, global_is_first_call, global_has_valid_cache
        
        # Use global state
        is_first_call = global_is_first_call[endpoint_name]
        cached_value = global_cache_state[endpoint_name]
        has_valid_cache_flag = global_has_valid_cache[endpoint_name]
        
        # ========== FIRST CALL VALIDATION (only for very first call) ==========
        if is_first_call:
            print(f"🔍 First call detected for {endpoint_name} - checking changes since server start")
            
            # Get current timestamps
            current_timestamps = self._get_current_timestamps()
            
            # Compare with GLOBAL initial timestamps (from server start)
            changed_since_start = []
            for file_name in ['CORENAME', 'ACTIVEGAME', 'CURRENTPATH']:
                initial_timestamp = server_initial_timestamps.get(file_name, 0)
                current_timestamp = current_timestamps[file_name]
                if current_timestamp != initial_timestamp:
                    changed_since_start.append(file_name)
                    print(f"📊 {file_name}: {initial_timestamp} → {current_timestamp} (CHANGED)")
                else:
                    print(f"📊 {file_name}: {initial_timestamp} (unchanged)")
            
            print(f"📊 Files changed since server start: {changed_since_start}")
            
            # CURRENTPATH is now a meaningful signal - no shortcut, always proceed with detection
            print(f"ℹ️ FIRST CALL - proceeding with normal detection (CURRENTPATH is significant)")
            # Mark as no longer first call and continue with detection
            global_is_first_call[endpoint_name] = False
        
        # ========== SAM LOGIC (unchanged) ==========
        # First, check if SAM_Games.log changed (existing logic works fine)
        sam_smart_active, sam_reason = self.is_sam_still_active_smart()
        
        if sam_smart_active:
            # SAM is active, let the endpoint handle SAM logic normally
            return False, "sam_active"
        
        # ========== ENHANCED CHANGE DETECTION (works for all calls) ==========
        # Check for significant changes in timestamps
        should_recalculate, reason, changed_files = self.has_significant_change()
        
        # Handle each case specifically
        if reason == "no_changes":
            # No files changed - use cache if available
            if has_valid_cache_flag and cached_value is not None:
                print(f"📋 Using cached {endpoint_name}: '{cached_value}'")
                return True, "cache_used"
            else:
                # No cache available, need to calculate
                print(f"🔄 No cache available for {endpoint_name}, proceeding with detection")
                return False, "no_cache_proceed"
        
        elif reason == "menu_navigation":
            # CURRENTPATH changed but CORENAME/ACTIVEGAME did not.
            # The user is browsing the OSD menu without loading anything.
            # Keep the last known value so the display stays stable.
            if has_valid_cache_flag and cached_value is not None:
                print(f"📋 Menu navigation - keeping cached {endpoint_name}: '{cached_value}'")
                return True, "cache_used"
            else:
                # No cache yet; proceed with detection to establish an initial value.
                print(f"⚠️ Menu navigation but no cache for {endpoint_name} - proceeding with detection")
                return False, "no_cache_proceed"
        
        elif reason == "significant_changes":
            # ✅ FIXED: CORENAME/ACTIVEGAME changed - invalidate cache and recalculate
            print(f"⚡ Significant changes detected: {changed_files} - INVALIDATING cache and proceeding with {endpoint_name} detection")
            
            # Invalidate cache for this endpoint
            global_cache_state['core'] = None
            global_cache_state['game'] = None  
            global_cache_state['rom_details'] = None
            global_has_valid_cache['core'] = False
            global_has_valid_cache['game'] = False
            global_has_valid_cache['rom_details'] = False
            
            # Proceed with normal detection to get fresh values
            return False, "proceed_detection"
        
        else:
            # Unknown reason - proceed with detection
            print(f"❓ Unknown change reason: {reason} - proceeding with {endpoint_name} detection")
            return False, "proceed_detection"

    # ========== OPTIMIZED CORE FUNCTIONS ==========
    
    def get_current_core(self):
        """
        ENHANCED CORE DETECTION - SAM first, then similarity detection for arcade conflicts
        Now with error state management and last valid core tracking
        FIXED: MENU validation now happens BEFORE cache logic
        ADDED: First call validation for CURRENTPATH-only changes
        """
        global last_valid_core, last_valid_core_timestamp, server_error_state
        
        print("🔍 === ENHANCED CORE DETECTION WITH IMPROVED CACHING===")

        # ========== NEW: EARLY MENU VALIDATION (BEFORE CACHE) ==========
        try:
            # Read CORENAME first to check for MENU state
            with open('/tmp/CORENAME', 'r') as f:
                corename_content = f.read().strip()
            
            # CRITICAL: If CORENAME is MENU, return "Menu" immediately
            if corename_content.upper() == "MENU":
                print("📋 EARLY DETECTION: CORENAME is MENU - returning 'Menu' regardless of cache")
                # Clear any cached value that might interfere
                self._update_global_cache('core', "Menu")
                return "Menu"
                
        except Exception as e:
            print(f"⚠️ Error reading CORENAME for early MENU check: {e}")
            # Continue with normal logic if we can't read the file

        # ========== EXISTING CACHE LOGIC + NEW FIRST CALL LOGIC ==========
        use_cache, cache_reason, *default_value = self._handle_cache_logic(
            'core', self.cached_core_state, self.has_valid_cache['core']
        )
        
        if use_cache:
            if cache_reason == "first_call_default":
                result = default_value[0] if default_value else "Menu"
                self._update_global_cache('core', result)
                return result
            elif cache_reason == "first_call_currentpath_only":
                # ✅ NEW: Handle first call with only CURRENTPATH changed
                result = default_value[0] if default_value else "Menu"
                print(f"🎯 First call + only CURRENTPATH changed since start - returning: '{result}'")
                return result
            elif cache_reason == "cache_used":
                return self.cached_core_state

        # Mark that this is no longer first call
        self.is_first_call['core'] = False
        
        print("🔄 Processing core detection due to significant changes...")
        
        try:
            # STEP 1: Check if SAM is active using smart detection
            sam_smart_active, sam_reason = self.is_sam_still_active_smart()
            print(f"🔍 SAM smart check: {sam_smart_active} - {sam_reason}")
            
            if sam_smart_active:
                sam_active, sam_core, sam_game, sam_path = self.is_sam_active_and_current()
                if sam_active and sam_core and sam_core.strip():
                    print(f"✅ SAM active - using SAM core: '{sam_core}'")
                    # Map core name from SAM
                    try:
                        mapped_core = self.map_sam_core_name(sam_core)
                        print(f"📝 SAM core mapped: '{sam_core}' → '{mapped_core}'")
                        
                        # ✅ NEW: Update last valid core
                        last_valid_core = mapped_core
                        last_valid_core_timestamp = time.time()
                        server_error_state = ""  # Clear error state
                        
                        self._update_global_cache('core', mapped_core)
                        return mapped_core
                    except Exception as e:
                        print(f"⚠️ Error mapping SAM core: {e}")
            
            print("🔍 Continuing with normal core detection...")
            
            # STEP 2: Read state files with timestamps (backup v3 strategy)
            # Note: corename_content already read above for MENU check
            activegame_content = ""
            activegame_timestamp = 0

            try:
                with open('/tmp/ACTIVEGAME', 'r') as f:
                    activegame_content = f.read().strip()
                activegame_timestamp = os.path.getmtime('/tmp/ACTIVEGAME')
            except:
                pass

            currentpath_content, currentpath_timestamp, stable_fullpath, path_source = \
                self._read_stable_path_sources()

            # For arcade detection, always read FULLPATH directly from /tmp.
            fullpath_content = ""
            try:
                with open('/tmp/FULLPATH', 'r') as f:
                    fullpath_content = f.read().strip()
            except:
                pass

            print(f"📄 CORENAME: '{corename_content}'")
            print(f"📄 ACTIVEGAME: '{activegame_content}' (ts: {activegame_timestamp})")
            print(f"📄 {path_source}: '{currentpath_content}' (ts: {currentpath_timestamp})")
            print(f"📄 FULLPATH (real): '{fullpath_content}'")
            
            # STEP 3: MENU validation (redundant check but kept for safety)
            if corename_content.upper() == "MENU":
                print("📋 CORENAME is MENU - returning 'Menu' (secondary check)")
                self._update_global_cache('core', "Menu")
                return "Menu"
            
            # STEP 4: No active core - use error handling
            if not corename_content:
                print("⚠️ No CORENAME available")
                try:
                    # Try to get from ACTIVEGAME if available
                    if activegame_content:
                        print(f"🔍 Trying to extract core from ACTIVEGAME: '{activegame_content}'")
                        # Extract core from path if possible
                        # This is a fallback mechanism
                        result = self._handle_error_state("NO CORENAME")
                        self._update_global_cache('core', result)
                        return result
                except:
                    pass
                
                server_error_state = "DISCONNECTED"
                result = self._handle_error_state("DISCONNECTED")
                self.cached_core_state = result
                self.has_valid_cache['core'] = True
                return result
            
            # STEP 5: ARCADE FAST PATH — check ACTIVEGAME before timestamp comparison.
            # Remote updates ACTIVEGAME and CORENAME simultaneously when launching arcade.
            # The OSD does NOT update ACTIVEGAME, so after an OSD load CORENAME will be
            # newer than ACTIVEGAME. The freshness check uses this difference to decide
            # whether ACTIVEGAME is still the current source or has been superseded by OSD.
            corename_timestamp = 0
            try:
                corename_timestamp = os.path.getmtime('/tmp/CORENAME')
            except:
                pass

            ARCADE_FRESHNESS = 30  # seconds grace period (Remote writes both files at once)
            activegame_arcade_fresh = (
                activegame_content and
                "/_Arcade/" in activegame_content and
                activegame_timestamp >= corename_timestamp - ARCADE_FRESHNESS
            )

            if activegame_arcade_fresh:
                print(f"🎮 ACTIVEGAME /_Arcade/ fresh (ag_ts={activegame_timestamp:.0f} corename_ts={corename_timestamp:.0f}) -> Arcade")
                last_valid_core = "Arcade"
                last_valid_core_timestamp = time.time()
                server_error_state = ""
                self._update_global_cache('core', "Arcade")
                return "Arcade"

            # STEP 6: Non-arcade or stale ACTIVEGAME — FULLPATH fallback, then CORENAME
            print(f"📄 FULLPATH: '{fullpath_content}'")

            if fullpath_content and "_Arcade" in fullpath_content:
                print("🎮 FULLPATH indicates _Arcade -> Arcade (OSD fallback)")
                last_valid_core = "Arcade"
                last_valid_core_timestamp = time.time()
                server_error_state = ""
                self._update_global_cache('core', "Arcade")
                return "Arcade"

            # Non-arcade: use CORENAME
            normalized_core = self.normalize_core_name(corename_content)
            print(f"🖥️ Core from CORENAME: '{normalized_core}'")
            last_valid_core = normalized_core
            last_valid_core_timestamp = time.time()
            server_error_state = ""
            self._update_global_cache('core', normalized_core)
            return normalized_core
            
        except Exception as e:
            print(f"❌ Critical error in get_current_core: {e}")
            import traceback
            traceback.print_exc()
            server_error_state = "CONNECTION ERROR"
            result = self._handle_error_state("CONNECTION ERROR")
            self._update_global_cache('core', result)
            return result
        
    def _handle_error_state(self, error_type):
        """
        Handle error states by returning last valid core or menu
        """
        global last_valid_core, last_valid_core_timestamp, server_error_state
        
        print(f"⚠️ Handling error state: {error_type}")
        
        # Check if we have a recent valid core (within last 10 minutes)
        if last_valid_core and last_valid_core != "Menu":
            age = time.time() - last_valid_core_timestamp
            if age < 600:  # 10 minutes
                print(f"🔄 Returning last valid core: '{last_valid_core}' (age: {age:.1f}s)")
                server_error_state = error_type  # Keep error state for status display
                return last_valid_core
        
        # No recent valid core, return Menu
        print("📋 No recent valid core, returning Menu")
        server_error_state = error_type
        return "Menu"
        
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
        print("🔍 === DEBUG: get_current_game() STARTED ===")
        print(f"🔍 DEBUG: global_cache_state['game']: '{global_cache_state.get('game', 'NOT_SET')}'")
        print(f"🔍 DEBUG: self.cached_game_state: '{getattr(self, 'cached_game_state', 'NOT_SET')}'")
        print(f"🔍 DEBUG: has_valid_cache['game']: {global_has_valid_cache.get('game', False)}")
        print("🔍 === OPTIMIZED GAME DETECTION ===")
        
        use_cache, cache_reason, *default_value = self._handle_cache_logic(
            'game', self.cached_game_state, self.has_valid_cache['game']
        )
        
        if use_cache:
            if cache_reason == "first_call_default":
                result = default_value[0] if default_value else ""
                self._update_global_cache('game', result)
                return result
            elif cache_reason == "first_call_currentpath_only":
                # ✅ NEW: Handle first call with only CURRENTPATH changed
                result = default_value[0] if default_value else ""
                print(f"🎯 First call + only CURRENTPATH changed since start - returning: '{result}'")
                return result
            elif cache_reason == "cache_used":
                return self.cached_game_state
        
        # Mark that this is no longer first call
        self.is_first_call['game'] = False
    
        print("📄 Processing game detection due to significant changes...")
        
        # STEP 1: Check if SAM is active using smart detection
        sam_smart_active, sam_reason = self.is_sam_still_active_smart()
        print(f"🔍 SAM smart check: {sam_smart_active} - {sam_reason}")
        
        if sam_smart_active:
            sam_active, sam_core, sam_game, sam_path = self.is_sam_active_and_current()
            if sam_active:
                print(f"✅ SAM active - returning game: '{sam_game}'")
                self._update_global_cache('game', sam_game)
                return sam_game
        
        print("🔍 Continuing with normal game detection...")
        
        # STEP 2: Read state files with timestamps (backup v3 strategy)
        corename_content = ""
        activegame_content = ""
        activegame_timestamp = 0
        currentpath_timestamp = 0

        try:
            with open('/tmp/CORENAME', 'r') as f:
                corename_content = f.read().strip()
        except:
            pass

        try:
            with open('/tmp/ACTIVEGAME', 'r') as f:
                activegame_content = f.read().strip()
            activegame_timestamp = os.path.getmtime('/tmp/ACTIVEGAME')
        except:
            pass

        currentpath_content, currentpath_timestamp, stable_fullpath, path_source = \
            self._read_stable_path_sources()

        # For arcade detection, always read FULLPATH directly from /tmp.
        fullpath_content = ""
        try:
            with open('/tmp/FULLPATH', 'r') as f:
                fullpath_content = f.read().strip()
        except:
            pass

        print(f"📄 CORENAME: '{corename_content}'")
        print(f"📄 ACTIVEGAME: '{activegame_content}' (ts: {activegame_timestamp})")
        print(f"📄 {path_source}: '{currentpath_content}' (ts: {currentpath_timestamp})")
        print(f"📄 FULLPATH (real): '{fullpath_content}'")

        # STEP 3: No active core or MENU - return empty
        if not corename_content or corename_content.upper() == "MENU":
            if corename_content.upper() == "MENU":
                print("📋 CORENAME is MENU - no active game (just browsing menu)")
            else:
                print("ℹ️ No core active - returning empty game")
            self._update_global_cache('game', "")
            return ""
        
        # STEP 4: ARCADE FAST PATH
        # CURRENTPATH is completely ignored for arcade games.
        #
        # Source priority:
        #   1. ACTIVEGAME with /_Arcade/ + .mra, fresh vs CORENAME  → Remote launch
        #      Valid only while ACTIVEGAME is as recent as CORENAME (Remote writes both).
        #      If CORENAME is newer (OSD loaded something after Remote), ACTIVEGAME is stale.
        #   2. FULLPATH containing _Arcade                           → OSD launch fallback

        corename_timestamp = 0
        try:
            corename_timestamp = os.path.getmtime('/tmp/CORENAME')
        except:
            pass

        ARCADE_FRESHNESS = 30  # seconds — Remote writes ACTIVEGAME and CORENAME together
        activegame_arcade_fresh = (
            activegame_content and
            "/_Arcade/" in activegame_content and
            activegame_content.lower().endswith('.mra') and
            activegame_timestamp >= corename_timestamp - ARCADE_FRESHNESS
        )

        print(f"📄 ACTIVEGAME: '{activegame_content}' (ag_ts={activegame_timestamp:.0f} corename_ts={corename_timestamp:.0f} fresh={activegame_arcade_fresh})")
        print(f"📄 FULLPATH:   '{fullpath_content}'")

        # --- Source 1: Remote arcade (ACTIVEGAME fresh with /_Arcade/*.mra) ---
        if activegame_arcade_fresh:
            game_name = os.path.splitext(os.path.basename(activegame_content))[0]
            print(f"🎮 Arcade (Remote/ACTIVEGAME): '{game_name}'")
            self._update_global_cache('game', game_name)
            return game_name

        # --- Source 2: OSD arcade (FULLPATH contains _Arcade) ---
        if fullpath_content and "_Arcade" in fullpath_content:
            # Game name comes from CURRENTPATH (.mra filename without extension).
            # CORENAME is the core identifier, not the game name.
            raw_currentpath_osd = ""
            try:
                with open('/tmp/CURRENTPATH', 'r') as f:
                    raw_currentpath_osd = f.read().strip()
            except:
                pass

            if raw_currentpath_osd:
                game_name = os.path.splitext(os.path.basename(raw_currentpath_osd))[0]
                print(f"🎮 Arcade (OSD/CURRENTPATH): '{game_name}'")
                self._update_global_cache('game', game_name)
                return game_name
            elif corename_content and corename_content.upper() != "MENU":
                print(f"🎮 Arcade (OSD/CORENAME fallback): '{corename_content}'")
                self._update_global_cache('game', corename_content)
                return corename_content
            else:
                print("⚠️ OSD arcade: no CURRENTPATH or CORENAME available")
                self._update_global_cache('game', "")
                return ""

        # STEP 5a: Non-arcade Remote launch.
        # Remote writes ACTIVEGAME first, then CURRENTPATH+FILESELECT simultaneously a moment
        # later, so ACTIVEGAME ends up slightly OLDER than CURRENTPATH — a timestamp comparison
        # alone is not reliable. The definitive signal is that Remote copies the game filename
        # into both ACTIVEGAME (full path) and CURRENTPATH (filename only), so:
        #   os.path.basename(ACTIVEGAME) == CURRENTPATH
        # This match only occurs when Remote loaded the game; during OSD navigation CURRENTPATH
        # changes continuously while ACTIVEGAME stays at its last Remote value, and
        # has_significant_change() suppresses detection (returns menu_navigation) so we never
        # reach this step unless ACTIVEGAME itself just changed.
        activegame_basename = os.path.basename(activegame_content) if activegame_content else ""
        activegame_remote_fresh = (
            activegame_content and
            "/_Arcade/" not in activegame_content and
            not activegame_content.lower().endswith('.ini') and
            (activegame_timestamp >= currentpath_timestamp or
             (currentpath_content and activegame_basename == currentpath_content))
        )

        if activegame_remote_fresh:
            game_name = self.extract_game_name(activegame_content, preserve_parentheses=True)
            print(f"🖥️ NON-ARCADE (Remote/ACTIVEGAME): '{game_name}'")
            self._update_global_cache('game', game_name)
            return game_name

        # STEP 5b: Non-arcade OSD load — compare FILESELECT and CURRENTPATH timestamps at nanosecond precision.
        # MiSTer writes FILESELECT and CURRENTPATH at the exact same nanosecond during navigation.
        # When a game is loaded via OSD, only FILESELECT gets a new timestamp; CURRENTPATH keeps its old one.
        # → FILESELECT_ns > CURRENTPATH_ns  : game loaded → use CURRENTPATH as game name
        # → FILESELECT_ns == CURRENTPATH_ns : navigation → no game change
        try:
            fileselect_ns  = os.stat('/tmp/FILESELECT').st_mtime_ns
            currentpath_ns = os.stat('/tmp/CURRENTPATH').st_mtime_ns
        except:
            fileselect_ns  = 0
            currentpath_ns = 0

        print(f"⏱️ FILESELECT_ns={fileselect_ns} CURRENTPATH_ns={currentpath_ns} diff={fileselect_ns - currentpath_ns}")

        if fileselect_ns > currentpath_ns and currentpath_content:
            # Extra check: if FULLPATH starts with '_' (e.g. _Computer, _Console, _Utility),
            # the user is loading a core from the OSD system folders, not a game ROM.
            # In that case FILESELECT being newer is a core-load event, not a game-load event.
            fullpath_basename = os.path.basename(fullpath_content.rstrip('/'))
            if fullpath_content and (fullpath_content.lstrip('/').startswith('_') or fullpath_basename.startswith('_')):
                print(f"⚠️ FULLPATH starts with '_' ('{fullpath_content}') → core load from OSD, not a game")
                self._update_global_cache('game', "")
                return ""
            game_name = self.extract_game_name(currentpath_content, preserve_parentheses=True)
            print(f"🖥️ NON-ARCADE (OSD/FILESELECT newer): '{game_name}'")
            self._update_global_cache('game', game_name)
            return game_name
        else:
            print(f"ℹ️ FILESELECT not newer than CURRENTPATH → navigation or no game loaded")
            self._update_global_cache('game', "")
            return ""

    # ========== HELPER FUNCTIONS ==========
    
    def get_sam_debug_info(self):
        """
        SAM-specific debug info - ENHANCED with timestamp comparison
        """
        sam_log_path = '/tmp/SAM_Games.log'
        info = {
            'timestamp': int(time.time()),
            'file_exists': os.path.exists(sam_log_path),
            'sam_detection_result': self.is_sam_active_and_current(),
            'sam_smart_check': self.is_sam_still_active_smart(),
            'raw_content': '',
            'parsed_lines': [],
            'file_info': {},
            'timestamp_comparison': {}
        }
        
        # Timestamp comparison
        current_time = time.time()
        timestamps = {}
        
        # Timestamp de SAM
        if os.path.exists(sam_log_path):
            sam_timestamp = os.path.getmtime(sam_log_path)
            timestamps['SAM_Games.log'] = {
                'timestamp': sam_timestamp,
                'age_seconds': int(current_time - sam_timestamp)
            }
        
        # Timestamps de otros archivos
        for file_name in ['CORENAME', 'ACTIVEGAME', 'CURRENTPATH']:
            file_path = f'/tmp/{file_name}'
            if os.path.exists(file_path):
                file_timestamp = os.path.getmtime(file_path)
                timestamps[file_name] = {
                    'timestamp': file_timestamp,
                    'age_seconds': int(current_time - file_timestamp),
                    'vs_sam_diff': int(file_timestamp - sam_timestamp) if 'SAM_Games.log' in timestamps else 0
                }
        
        info['timestamp_comparison'] = timestamps
        
        if os.path.exists(sam_log_path):
            try:
                stat = os.path.getmtime(sam_log_path)
                info['file_info'] = {
                    'size': os.path.getsize(sam_log_path),
                    'age_seconds': int(time.time() - stat),
                    'modification_time': stat
                }
                
                with open(sam_log_path, 'r', encoding='utf-8', errors='ignore') as f:
                    content = f.read()
                
                info['raw_content'] = content
                lines = [line.strip() for line in content.split('\n') if line.strip()]
                info['total_lines'] = len(lines)
                
                # Parse last 3 lines to show parsing
                for line in lines[-3:]:
                    parts = line.split(' - ')
                    if len(parts) >= 3:
                        parsed = {
                            'raw_line': line,
                            'timestamp': parts[0],
                            'core_raw': parts[1],
                            'core_mapped': self.map_sam_core_name(parts[1]),
                            'full_path': ' - '.join(parts[2:]),
                            'game_name': os.path.splitext((' - '.join(parts[2:])).split('/')[-1])[0]
                        }
                        info['parsed_lines'].append(parsed)
                
            except Exception as e:
                info['error'] = str(e)
        
        return info
    
    def is_known_non_arcade_system(self, corename):
        """
        Verifica si un nombre de core corresponde a un sistema conocido no-arcade
        """
        if not corename:
            return False
        
        corename_clean = re.sub(r'\s+', '', corename.lower())
        
        for known_system in self.KNOWN_NON_ARCADE_SYSTEMS:
            if known_system in corename_clean or corename_clean in known_system:
                return True
        
        return False

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

    def normalize_core_name(self, core_name):
        """
        Normaliza nombres de cores usando el mapeo existente
        """
        if not core_name:
            return "Menu"
        
        # Buscar en el mapeo
        if core_name in self.CORE_NAME_MAPPING:
            return self.CORE_NAME_MAPPING[core_name]
        
        # Casos especiales adicionales
        core_lower = core_name.lower()
        special_mappings = {
            'mame': 'Arcade',
            'hbmame': 'Arcade',
            'megadrive': 'Megadrive',
            'mastersystem': 'Master System',
            'gameboy': 'Game Boy',
            'gameboycolor': 'Game Boy Color',
            'gameboyadvance': 'Game Boy Advance',
            'nintendo64': 'Nintendo 64',
            'supernintendo': 'Super Nintendo',
            'playstation': 'PlayStation',
            'commodore64': 'Commodore 64',
            'pcengine': 'PC Engine',
        }
        
        if core_lower in special_mappings:
            return special_mappings[core_lower]
        
        # If not in mapping, return as-is
        return core_name

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
            if self.is_known_non_arcade_system(corename):
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

    def is_sam_active_and_current(self):
        """
        Checks if SAM is active and returns current info - CORRECTED SAM FORMAT
        """
        try:
            sam_log_path = '/tmp/SAM_Games.log'
            
            if not os.path.exists(sam_log_path):
                print(f"🔍 SAM_Games.log no existe en {sam_log_path}")
                return False, "", "", ""
            
            # Verificar si el archivo es reciente (ampliar a 5 minutos)
            sam_stat = os.path.getmtime(sam_log_path)
            age = time.time() - sam_stat
            
            print(f"🔍 SAM_Games.log age: {age:.1f} seconds")
            
            if age > 300:  # 5 minutos
                print(f"🔍 SAM_Games.log demasiado antiguo: {age:.1f}s > 300s")
                return False, "", "", ""
            
            # Leer contenido del log con mejor manejo de errores
            try:
                with open(sam_log_path, 'r', encoding='utf-8', errors='ignore') as f:
                    lines = f.readlines()
            except UnicodeDecodeError:
                with open(sam_log_path, 'r', encoding='latin-1') as f:
                    lines = f.readlines()
            
            if not lines:
                print("🔍 SAM_Games.log is empty")
                return False, "", "", ""
            
            # Process lines from last to first to find a valid entry
            for i in range(len(lines) - 1, -1, -1):
                line = lines[i].strip()
                if not line:
                    continue
                    
                print(f"🔍 SAM processing line: '{line}'")
                
                # Formato SAM: "04:17:58 - atarilynx - /media/fat/games/AtariLynx/..."
                # Pattern: [TIME] - [CORE] - [FULL_PATH]
                parts = line.split(' - ')
                
                if len(parts) >= 3:
                    # parts[0] = hora (04:17:58)
                    # parts[1] = core (atarilynx) 
                    # parts[2] y siguientes = ruta del archivo
                    
                    sam_core_raw = parts[1].strip()
                    sam_path = ' - '.join(parts[2:])  # Reunir ruta completa por si tiene " - " en el nombre
                    
                    # Extract game name: last path segment without extension
                    if sam_path:
                        # Get the last segment after the final /
                        game_filename = sam_path.split('/')[-1]
                        # Remove extension
                        sam_game = os.path.splitext(game_filename)[0]
                    else:
                        sam_game = ""
                    
                    # Mapear nombre del core SAM a nombre legible
                    sam_core = self.map_sam_core_name(sam_core_raw)
                    
                    print(f"✅ SAM detectado - Core: '{sam_core}' (raw: '{sam_core_raw}'), Game: '{sam_game}', Path: '{sam_path}'")
                    return True, sam_core, sam_game, sam_path
            
            print("🔍 No valid information found in SAM_Games.log")
            return False, "", "", ""
            
        except Exception as e:
            print(f"❌ Error checking SAM: {e}")
            import traceback
            traceback.print_exc()
            return False, "", "", ""
        
    def map_sam_core_name(self, sam_core_raw):
        """
        Mapea nombres de cores desde SAM a nombres legibles
        """
        # Specific mapping from SAM
        sam_core_mapping = {
            'amiga': 'Amiga',
            'amigacd32': 'Amiga CD32',
            'ao486': 'PC/Windows',
            'arcade': 'mame',  # Especial: arcade desde SAM debe mapear a mame
            'atari2600': 'Atari 2600',
            'atari5200': 'Atari 5200',
            'atari7800': 'Atari 7800',
            'atarilynx': 'Lynx',
            'c64': 'Commodore 64',
            'fds': 'Family Computer Disk System',
            'gb': 'Game Boy',
            'gbc': 'Game Boy Color',
            'gba': 'Game Boy Advance',
            'genesis': 'Megadrive',
            'megacd': 'Mega-CD',
            'n64': 'Nintendo 64',
            'neogeo': 'Neo-Geo AES',
            's32x': 'Megadrive 32X',
            'saturn': 'Saturn',
            'sms': 'Master System',
            'snes': 'Super Nintendo',
            'tgfx16': 'PC Engine',
            'tgfx16cd': 'PC Engine CD-Rom',
            'psx': 'PlayStation',
            'x68k': 'Sharp X68000',
        }
        
        # Look for specific mapping
        mapped_name = sam_core_mapping.get(sam_core_raw.lower())
        if mapped_name:
            print(f"🔄 SAM core mapeado: '{sam_core_raw}' → '{mapped_name}'")
            return mapped_name
        
        # If no specific mapping, use the standard normalization method
        normalized = self.normalize_core_name(sam_core_raw)
        print(f"🔄 SAM core normalizado: '{sam_core_raw}' → '{normalized}'")
        return normalized

    def is_sam_still_active_smart(self):
        """
        Smart check of whether SAM is still active considering multiple factors
        """
        current_time = time.time()
        
        # Basic SAM check
        sam_active, sam_core, sam_game, sam_path = self.is_sam_active_and_current()
        
        if not sam_active:
            return False, "SAM log not active or too old"
        
        # Get timestamps of regular detection files
        other_files_timestamps = {}
        for file_name in ['CORENAME', 'ACTIVEGAME', 'CURRENTPATH', 'FULLPATH']:
            file_path = f'/tmp/{file_name}'
            if os.path.exists(file_path):
                other_files_timestamps[file_name] = os.path.getmtime(file_path)
            else:
                other_files_timestamps[file_name] = 0
        
        # Obtener timestamp del log SAM
        sam_log_path = '/tmp/SAM_Games.log'
        sam_timestamp = os.path.getmtime(sam_log_path) if os.path.exists(sam_log_path) else 0
        
        # Check if ACTIVEGAME is significantly newer than SAM
        activegame_timestamp = other_files_timestamps.get('ACTIVEGAME', 0)
        corename_timestamp = other_files_timestamps.get('CORENAME', 0)
        
        # If ACTIVEGAME or CORENAME are more than 30 seconds newer than SAM,
        # the user probably switched manually
        grace_period = 30  # segundos
        
        if activegame_timestamp > sam_timestamp + grace_period:
            print(f"🔄 ACTIVEGAME newer than SAM: {activegame_timestamp - sam_timestamp:.1f}s")
            return False, f"ACTIVEGAME newer than SAM log ({activegame_timestamp - sam_timestamp:.1f}s difference)"
        
        if corename_timestamp > sam_timestamp + grace_period:
            print(f"🔄 CORENAME newer than SAM: {corename_timestamp - sam_timestamp:.1f}s")
            return False, f"CORENAME newer than SAM log ({corename_timestamp - sam_timestamp:.1f}s difference)"
        
        # Check if any other file is newer (with a smaller margin for other files)
        newest_other_file = max(other_files_timestamps.values()) if other_files_timestamps else 0
        
        if newest_other_file > sam_timestamp + 60:  # 60 segundos para otros archivos
            return False, f"Other detection files much newer than SAM log ({newest_other_file - sam_timestamp:.1f}s difference)"
        
        # SAM is active and is the most recent or current source
        return True, "SAM is active and current"

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
        session_duration = current_time - self.session_start
        
        stats = {
            'session_start_time': int(self.session_start),
            'session_duration_seconds': int(session_duration),
            'session_duration_formatted': self.format_duration(session_duration),
            'requests_count': self.requests_count,
            'requests_per_minute': round((self.requests_count / (session_duration / 60)) if session_duration > 0 else 0, 2),
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
                
                print(f"❌ File info not found: {internal_path}")
                return None, 0
            
        except Exception as e:
            print(f"❌ ZIP info error: {e}")
            return None, 0
    
    def get_rom_details(self):
        """
        ENHANCED METHOD: Get complete ROM details with improved path detection logic
        New logic: Check /status/game first, then SAM_Games.log, then CORENAME-based detection
        Includes: filename, size, CRC32, MD5, SHA1, path
        ADDED: First call validation for CURRENTPATH-only changes
        """
        try:
            print(f"[{time.strftime('%H:%M:%S')}] Getting ROM details with enhanced caching...")
            
            # Apply new cache logic
            use_cache, cache_reason, *default_value = self._handle_cache_logic(
                'rom_details', self.cached_rom_details, self.has_valid_cache['rom_details']
            )
            
            if use_cache:
                if cache_reason == "first_call_default":
                    result = default_value[0] if default_value else {
                        "filename": "", "size": 0, "crc32": "", "md5": "", "sha1": "", "path": ""
                    }
                    self._update_global_cache('rom_details', result)
                    return result
                elif cache_reason == "first_call_currentpath_only":
                    # ✅ NEW: Handle first call with only CURRENTPATH changed
                    result = default_value[0] if default_value else {
                        "filename": "", "size": 0, "crc32": "", "md5": "", "sha1": "", "path": "",
                        "available": False, "error": "Menu navigation (first call)", 
                        "detection_method": "first_call_currentpath_only", "timestamp": int(time.time())
                    }
                    print(f"🎯 First call + only CURRENTPATH changed since start - returning default ROM details")
                    self._update_global_cache('rom_details', result)
                    return result
                elif cache_reason == "cache_used":
                    return self.cached_rom_details
            
            # Mark that this is no longer first call
            self.is_first_call['rom_details'] = False
            
            print("📄 Processing ROM details detection due to significant changes...")
            
            # Get ROM path using enhanced detection logic
            rom_path = self._get_enhanced_rom_path()
            
            if not rom_path:
                print("No ROM path found with enhanced detection")
                result = {
                    "filename": "",
                    "size": 0,
                    "crc32": "",
                    "md5": "",
                    "sha1": "",
                    "path": "",
                    "available": False,
                    "error": "No active ROM found",
                    "detection_method": "none",
                    "timestamp": int(time.time())
                }
                self._update_global_cache('rom_details', result)
                return result
            
            print(f"Enhanced detection found ROM path: {rom_path}")
            
            # Check if the ROM is inside a ZIP file
            is_zip, zip_path, internal_path = self.is_zip_path(rom_path)
            
            if is_zip:
                print(f"ROM is inside ZIP: {zip_path} -> {internal_path}")
                result = self.get_rom_details_from_zip(rom_path, zip_path, internal_path)
            else:
                print(f"ROM is regular file: {rom_path}")
                result = self.get_rom_details_from_file(rom_path)
            
            # Add detection method info to result
            result["detection_method"] = getattr(self, '_last_detection_method', 'unknown')
            
            # Update cache
            self._update_global_cache('rom_details', result)
            return result
                    
        except Exception as e:
            error_msg = f"Unexpected error in get_rom_details: {str(e)}"
            print(f"CRITICAL ERROR: {error_msg}")
            import traceback
            traceback.print_exc()
            result = {
                "filename": "",
                "size": 0,
                "crc32": "",
                "md5": "",
                "sha1": "",
                "path": "",
                "available": False,
                "error": error_msg,
                "detection_method": "error",
                "timestamp": int(time.time())
            }
            self._update_global_cache('rom_details', result)
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
            self._update_global_cache('rom_details', result)
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

        currentpath, currentpath_timestamp, fullpath, path_source = \
            self._read_stable_path_sources()

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

    def get_game_debug_info(self):
        """
        Debug information for game detection - ENHANCED WITH SIMILARITY DETECTION
        """
        debug_info = {
            'timestamp': int(time.time()),
            'detection_method': 'unknown',
            'files': {},
            'arcade_detection': False,
            'sam_active': False,
            'has_arcade_path_mismatch': False,
            'name_similarity': {
                'detected': False,
                'confidence': 0.0,
                'corename': '',
                'arcade_filename': ''
            },
            'game_source': 'none'
        }
        
        # Verificar SAM PRIMERO
        sam_active, sam_core, sam_game, sam_path = self.is_sam_active_and_current()
        debug_info['sam_active'] = sam_active
        if sam_active:
            debug_info['detection_method'] = 'sam'
            debug_info['game_source'] = 'sam_log'
            debug_info['sam_details'] = {
                'core': sam_core,
                'game': sam_game,
                'path': sam_path
            }
        
        # Verificar archivos
        files_to_check = ['CORENAME', 'ACTIVEGAME', 'CURRENTPATH', 'FULLPATH']
        for file_name in files_to_check:
            file_path = f'/tmp/{file_name}'
            debug_info['files'][file_name] = {
                'exists': os.path.exists(file_path),
                'content': '',
                'age_seconds': 0
            }
            
            if os.path.exists(file_path):
                try:
                    with open(file_path, 'r') as f:
                        content = f.read().strip()
                    debug_info['files'][file_name]['content'] = content
                    stat = os.path.getmtime(file_path)
                    debug_info['files'][file_name]['age_seconds'] = int(time.time() - stat)
                except:
                    pass
        
        # If not in SAM mode, analyze normal detection
        if not sam_active:
            fullpath_content = debug_info['files']['FULLPATH']['content']
            corename_content = debug_info['files']['CORENAME']['content']
            activegame_content = debug_info['files']['ACTIVEGAME']['content']
            
            # CASO ESPECIAL: Verificar similitud de nombres para conflictos MiSTer/Web
            if (activegame_content and "/_Arcade/" in activegame_content and 
                activegame_content.endswith('.mra')):
                
                is_similar, confidence = self.detect_arcade_name_similarity(corename_content, activegame_content)
                debug_info['name_similarity'] = {
                    'detected': is_similar,
                    'confidence': confidence,
                    'corename': corename_content,
                    'arcade_filename': os.path.splitext(os.path.basename(activegame_content))[0]
                }
                
                if is_similar:
                    debug_info['arcade_detection'] = True
                    debug_info['detection_method'] = 'arcade_similarity_detected'
                    debug_info['game_source'] = 'activegame_similarity_match'
                    return debug_info
            
            # Verificar conflicto /_Arcade/ normal
            if activegame_content and "/_Arcade/" in activegame_content:
                if fullpath_content and ("arcade" in fullpath_content.lower()):
                    if not self.is_known_non_arcade_system(corename_content):
                        debug_info['arcade_detection'] = True
                        debug_info['detection_method'] = 'arcade'
                        debug_info['game_source'] = 'activegame_mra' if activegame_content.endswith('.mra') else 'currentpath'
                    else:
                        debug_info['has_arcade_path_mismatch'] = True
                        debug_info['detection_method'] = 'non_arcade_with_arcade_path'
                        debug_info['game_source'] = 'none'  # No game because of mismatch
                else:
                    debug_info['has_arcade_path_mismatch'] = True
                    debug_info['detection_method'] = 'non_arcade_with_arcade_path'
                    debug_info['game_source'] = 'none'  # No game because of mismatch
            else:
                # Normal detection
                if fullpath_content and ("arcade" in fullpath_content.lower()):
                    if not self.is_known_non_arcade_system(corename_content):
                        debug_info['arcade_detection'] = True
                        debug_info['detection_method'] = 'arcade'
                        debug_info['game_source'] = 'currentpath'
                
                if not debug_info['arcade_detection']:
                    if not corename_content:
                        debug_info['detection_method'] = 'menu'
                    else:
                        debug_info['detection_method'] = 'standard'
                        debug_info['game_source'] = 'activegame_full_name' if activegame_content else 'none'
        
        return debug_info

    def get_debug_files_info(self):
        """
        Debug file information - ENHANCED for SAM
        """
        files_to_check = ['CORENAME', 'ACTIVEGAME', 'CURRENTPATH', 'FULLPATH', 'SAM_Games.log']
        files_info = {}
        
        for file_name in files_to_check:
            file_path = f'/tmp/{file_name}'
            info = {
                'exists': False,
                'content': '',
                'size': 0,
                'age_seconds': 0,
                'error': None,
                'lines_count': 0
            }
            
            if os.path.exists(file_path):
                info['exists'] = True
                try:
                    # Manejo especial para SAM_Games.log
                    if file_name == 'SAM_Games.log':
                        try:
                            with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
                                content = f.read()
                        except UnicodeDecodeError:
                            with open(file_path, 'r', encoding='latin-1') as f:
                                content = f.read()
                        
                        lines = content.strip().split('\n')
                        info['lines_count'] = len([l for l in lines if l.strip()])
                        info['content'] = content.strip()
                        
                        # Show last 3 lines for debug
                        if lines:
                            last_lines = [l.strip() for l in lines[-3:] if l.strip()]
                            info['last_lines'] = last_lines
                    else:
                        with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
                            content = f.read().strip()
                        info['content'] = content
                    
                    stat = os.path.getmtime(file_path)
                    info['age_seconds'] = int(time.time() - stat)
                    info['size'] = os.path.getsize(file_path)
                    
                    # Special info for FULLPATH
                    if file_name == 'FULLPATH':
                        info['contains_arcade'] = "*Arcade" in content or "_Arcade" in content or "arcade" in content.lower()
                        
                except Exception as e:
                    info['error'] = str(e)
            
            files_info[file_name] = info
        
        return {
            'timestamp': int(time.time()),
            'files': files_info,
            'sam_test_result': self.is_sam_active_and_current()  # Test directo de SAM
        }

    def get_system_type_info(self):
        """
        System type information
        """
        debug_info = self.get_game_debug_info()
        
        return {
            'type': debug_info['detection_method'],
            'is_arcade': debug_info['arcade_detection'],
            'is_sam_controlled': debug_info['sam_active'],
            'current_core': self.get_current_core(),
            'current_game': self.get_current_game(),
            'timestamp': debug_info['timestamp']
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
        server = HTTPServer(('', 8081), MiSTerStatusHandler)
        print("MiSTer Status Server v2 - inotify-based detection - port 8081")
        print("Available endpoints:")
        print("  /status/core         - Current core (SAM first, then optimized detection)")
        print("  /status/game         - Current game (with fixes applied)")  
        print("  /status/rom          - Loaded ROM")
        print("  /status/system       - CPU, memory, uptime")
        print("  /status/storage      - SD/USB storage")
        print("  /status/usb          - USB devices")
        print("  /status/network      - Network status")
        print("  /status/session      - Session statistics")
        print("  /status/debug/game   - Game detection debug")
        print("  /status/debug/files  - Raw file contents debug")
        print("  /status/system/type  - System type detection")
        print("  /status/rom/details  - ROM details")
        print("  /status/all          - All data")
        print("")
        print("FIXES APPLIED v2:")
        print("  🔧 SAM detection first with timestamp validation")
        print("  🎮 Non-arcade games: Full name with parentheses preserved")
        print("  🚫 /_Arcade/ detection: No game shown if core mismatch")
        print("  📁 Arcade priority: ACTIVEGAME .mra files, then CURRENTPATH")
        print("  ⚡ Optimized detection logic maintained")
        print("")
        print("DETECTION LOGIC FIXED:")
        print("  1. 📁 SAM detection FIRST (timestamp validated)")
        print("  2. 🕹️ Arcade: FULLPATH + CORENAME validation")
        print("     - If ACTIVEGAME has /_Arcade/xxx.mra → use xxx")
        print("     - Otherwise use CURRENTPATH")
        print("  3. 🖥️ Non-arcade: ACTIVEGAME with full names")
        print("     - If ACTIVEGAME has /_Arcade/ → no game active")
        print("")
        server.serve_forever()
    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()

