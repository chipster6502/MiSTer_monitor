#!/usr/bin/env python3
# =============================================================================
# ra_status.py — RetroAchievements status resolver for MiSTer Monitor (A4 min)
# =============================================================================
# Minimal first version. Resolves the active ROM to a RetroAchievements game
# and returns the user's progress, as a FLAT JSON dict the CYD firmware can
# parse with its existing extractStringValue / extractIntValue / extractBoolValue
# helpers (no nested objects — ArduinoJson is intentionally not in the firmware).
#
# Design note (revised after hardware bring-up):
#   This module does NOT read server state or resolve ROM paths itself. It asks
#   the running handler for the data the server already computes:
#     - handler.get_current_core()   -> friendly core name
#     - handler.get_rom_details()    -> dict with resolved_zip_path / internal_path
#   The server's path resolver already handles ZIP / CHD / USB / SAM / relative
#   paths robustly; duplicating that here was the source of early bugs. We take
#   the resolved path and apply RA's per-console hash rules on top (the server's
#   own md5 is the RAW file hash, which is wrong for headered systems like NES).
#
# What this version does (and only this):
#   1. loads ra_credentials.ini
#   2. maps the friendly core name -> internal RA key -> consoleID(s)
#   3. lazily downloads+caches each console's hash index (on demand, TTL 7d)
#   4. computes the ROM's RA hash via ra_hash.compute_ra_hash on the resolved path
#   5. resolves hash -> gameID against the cached index
#   6. calls API_GetGameInfoAndUserProgress and aggregates progress
#
# NOT in this version (deferred — see PENDING.md): LastGameID fallback, hardcore
# breakdown, recent-unlock polling / event_counter.
# =============================================================================

import json
import os
import ssl
import time
import threading
import urllib.parse
import urllib.request

from ra_hash import compute_ra_hash, is_core_supported

# --- Paths & tunables --------------------------------------------------------

_BASE_DIR   = "/media/fat/Scripts/.config/mister_monitor"
_CRED_PATH  = os.path.join(_BASE_DIR, "ra_credentials.ini")
_CACHE_DIR  = os.path.join(_BASE_DIR, "ra_cache")

_INDEX_TTL_SECONDS = 7 * 24 * 3600
_API_BASE          = "https://retroachievements.org/API"
_HTTP_TIMEOUT      = 15

_CA_CANDIDATES = [
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/ssl/cert.pem",
]


def _make_ssl_context():
    for ca in _CA_CANDIDATES:
        if os.path.exists(ca):
            try:
                return ssl.create_default_context(cafile=ca), True
            except Exception:
                pass
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx, False


_SSL_CTX, _SSL_VERIFIED = _make_ssl_context()


# --- Friendly core name -> internal RA key -----------------------------------
# The server exposes the FRIENDLY name (post-CORE_NAME_MAPPING), e.g.
# "Super Nintendo/Super Famicom", not "snes". We translate friendly -> key.
# Keys must match CORE_TO_CONSOLE_IDS below and ra_hash.CORE_HASH_METHOD.
# Matching is case-insensitive and tolerant of the exact friendly string.
#
# UNVERIFIED friendly strings are marked; confirm on hardware as cores are
# tested (each is harmless if wrong — the core simply reports unsupported).

FRIENDLY_TO_KEY = {
    "super nintendo/super famicom": "snes",
    "super nintendo":               "snes",
    "nintendo nes/famicom":         "nes",
    "sega genesis/mega drive":      "genesis",
    "nintendo game boy":            "gameboy",
    "nintendo game boy color":      "gameboy",
    "nintendo game boy advance":    "gba",
    "nintendo 64":                  "n64",
    "turbografx-16/pc engine":      "tgfx16",
    "sega master system":           "sms",
    "sega master system/game gear": "sms",   # (?) verify friendly string
    "atari 2600":                   "atari7800",  # RA 2600 runs via 7800 core
}


# --- Internal RA key -> consoleID(s) -----------------------------------------
# Verified against API_GetConsoleIDs (g=1,a=1) on real hardware.
CORE_TO_CONSOLE_IDS = {
    'nes':          [7, 81],
    'snes':         [3],
    'genesis':      [1],
    'sms':          [11, 15],
    'gameboy':      [4, 6],
    'gba':          [5],
    'n64':          [2],
    'tgfx16':       [8],
    'atari7800':    [51, 25],
    's32x':         [10],
}

_index_maps = {}
_index_lock = threading.Lock()


def _friendly_to_key(core_friendly):
    """Translate a friendly core name to an internal RA key, or '' if none."""
    if not core_friendly:
        return ""
    return FRIENDLY_TO_KEY.get(core_friendly.strip().lower(), "")


# --- Credentials -------------------------------------------------------------

def _load_credentials():
    if not os.path.exists(_CRED_PATH):
        return None, None
    user = key = None
    try:
        with open(_CRED_PATH, 'r') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith((';', '#', '[')):
                    continue
                if '=' not in line:
                    continue
                k, v = line.split('=', 1)
                k = k.strip().lower()
                v = v.strip()
                if k == 'username':
                    user = v
                elif k == 'api_key':
                    key = v
    except OSError:
        return None, None
    if not user or not key:
        return None, None
    return user, key


def _mask(key):
    if not key or len(key) < 8:
        return "***"
    return key[:4] + "…" + key[-2:]


# --- RA Web API calls --------------------------------------------------------

def _api_get(endpoint, params):
    url = "%s/%s?%s" % (_API_BASE, endpoint, urllib.parse.urlencode(params))
    try:
        req = urllib.request.Request(url, headers={'User-Agent': 'MiSTerMonitor'})
        with urllib.request.urlopen(req, timeout=_HTTP_TIMEOUT, context=_SSL_CTX) as resp:
            raw = resp.read()
        return json.loads(raw.decode('utf-8'))
    except Exception as e:
        print(f"[RA] API call failed ({endpoint}): {e}")
        return None


# --- Hash index (lazy, disk-cached) ------------------------------------------

def _index_cache_path(console_id):
    return os.path.join(_CACHE_DIR, f"ra_index_{console_id}.json")


def _load_or_fetch_index(console_id, user, key):
    with _index_lock:
        if console_id in _index_maps:
            return _index_maps[console_id]

        os.makedirs(_CACHE_DIR, exist_ok=True)
        cache_path = _index_cache_path(console_id)

        raw_list = None
        if os.path.exists(cache_path):
            age = time.time() - os.path.getmtime(cache_path)
            if age < _INDEX_TTL_SECONDS:
                try:
                    with open(cache_path, 'r') as f:
                        raw_list = json.load(f)
                    print(f"[RA] index {console_id}: cache hit ({age/86400:.1f}d old)")
                except Exception:
                    raw_list = None

        if raw_list is None:
            print(f"[RA] index {console_id}: fetching (key {_mask(key)})")
            raw_list = _api_get("API_GetGameList.php",
                                {'i': console_id, 'f': 1, 'h': 1, 'y': key})
            if raw_list is None:
                return {}
            try:
                with open(cache_path, 'w') as f:
                    json.dump(raw_list, f)
            except OSError as e:
                print(f"[RA] index {console_id}: cache write failed: {e}")

        hmap = {}
        try:
            for g in raw_list:
                gid = g.get('ID')
                for h in g.get('Hashes', []) or []:
                    hmap[h.lower()] = gid
        except (AttributeError, TypeError) as e:
            print(f"[RA] index {console_id}: unexpected shape: {e}")
            return {}

        _index_maps[console_id] = hmap
        print(f"[RA] index {console_id}: {len(hmap)} hashes indexed")
        return hmap


def _resolve_game_id(console_ids, ra_hash, user, key):
    h = ra_hash.lower()
    for cid in console_ids:
        hmap = _load_or_fetch_index(cid, user, key)
        if h in hmap:
            return hmap[h], cid
    return 0, None


# --- Active ROM resolution (delegates to the server's own resolver) ----------

def _get_active_rom(handler):
    """
    Returns (core_friendly, resolved_path, internal_path).
    Uses the handler's OWN methods so all path/state logic stays in the server:
      - get_current_core()  gives the friendly core name from live state
      - get_rom_details()   gives resolved_zip_path / internal_path / path,
                            already handling ZIP / CHD / USB / SAM resolution
    resolved_path is the ZIP container (for zipped ROMs) or the plain file.
    internal_path is the in-ZIP name, or None for plain files.
    """
    core = ""
    try:
        core = handler.get_current_core() or ""
    except Exception as e:
        print(f"[RA] get_current_core failed: {e}")

    if not core or core.strip().lower() == "menu":
        return core, None, None

    try:
        details = handler.get_rom_details()
    except Exception as e:
        print(f"[RA] get_rom_details failed: {e}")
        return core, None, None

    if not isinstance(details, dict) or not details.get("available"):
        return core, None, None

    zip_path = details.get("resolved_zip_path") or details.get("zip_path")
    internal = details.get("internal_path")
    if zip_path and internal:
        return core, zip_path, internal

    plain = details.get("path") or ""
    if plain and os.path.exists(plain):
        return core, plain, None

    return core, None, None


# --- Public entry point ------------------------------------------------------

def get_ra_status(handler):
    now = int(time.time())
    out = {
        "enabled": False,
        "supported": False,
        "game_matched": False,
        "game_id": 0,
        "game_title": "",
        "total": 0,
        "unlocked": 0,
        "points_earned": 0,
        "points_total": 0,
        "ssl_verified": _SSL_VERIFIED,
        "timestamp": now,
    }

    user, key = _load_credentials()
    if not user or not key:
        out["status"] = "not_configured"
        return out
    out["enabled"] = True

    core_friendly, rom_path, internal = _get_active_rom(handler)
    out["core"] = core_friendly

    ra_key = _friendly_to_key(core_friendly)
    if not ra_key or ra_key not in CORE_TO_CONSOLE_IDS or not is_core_supported(ra_key):
        out["status"] = "core_not_supported"
        return out
    out["supported"] = True
    out["ra_key"] = ra_key
    console_ids = CORE_TO_CONSOLE_IDS[ra_key]

    if not rom_path:
        out["status"] = "no_game_loaded"
        return out

    hres = compute_ra_hash(ra_key, rom_path, internal)
    if hres["error"] or not hres["hash"]:
        out["status"] = "hash_error"
        out["detail"] = hres["error"] or "no hash produced"
        return out
    out["ra_hash"] = hres["hash"].lower()

    gid, matched_cid = _resolve_game_id(console_ids, hres["hash"], user, key)
    if gid == 0:
        out["status"] = "rom_not_recognized"
        return out
    out["game_matched"] = True
    out["game_id"] = gid

    prog = _api_get("API_GetGameInfoAndUserProgress.php",
                    {'g': gid, 'u': user, 'y': key})
    if prog is None:
        out["status"] = "progress_unavailable"
        return out

    out["game_title"] = str(prog.get("Title", "") or "")
    try:
        total = int(prog.get("NumAchievements", 0) or 0)
    except (TypeError, ValueError):
        total = 0
    try:
        unlocked = int(prog.get("NumAwardedToUser", 0) or 0)
    except (TypeError, ValueError):
        unlocked = 0

    points_total = 0
    points_earned = 0
    ach = prog.get("Achievements", {})
    if isinstance(ach, dict):
        for a in ach.values():
            try:
                p = int(a.get("Points", 0) or 0)
            except (TypeError, ValueError):
                p = 0
            points_total += p
            if a.get("DateEarned") or a.get("DateEarnedHardcore"):
                points_earned += p

    out["total"] = total
    out["unlocked"] = unlocked
    out["points_earned"] = points_earned
    out["points_total"] = points_total
    out["status"] = "ok"
    return out
