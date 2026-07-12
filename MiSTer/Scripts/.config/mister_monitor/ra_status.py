#!/usr/bin/env python3
# =============================================================================
# ra_status.py — RetroAchievements status resolver for MiSTer Monitor (A4 full)
# =============================================================================
# Full version. Adds on top of the minimal one:
#   - hardcore breakdown (unlocked_hardcore / points_hardcore)
#   - LastGameID fallback with TITLE CORROBORATION for unindexed dumps and
#     CD systems (only trusted when RA's title matches the local search_name)
#   - background unlock polling thread (event_counter + last_unlock_* fields)
#
# Threading / coupling model:
#   The polling thread never imports the server module (that pattern loaded a
#   ghost second instance during bring-up). Instead the server injects a state
#   getter at startup:
#       from ra_status import start_ra_polling
#       start_ra_polling(lambda: (_state, _state_lock))
#   The getter is used ONLY to read _state['seq'] so the thread can detect
#   game changes; everything else flows through the normal endpoint path.
#
# Flat-JSON contract: every field is a scalar (string/int/bool). The CYD
# firmware parses with extractStringValue/extractIntValue/extractBoolValue —
# no nested objects, ever.
#
# UNVERIFIED field-name assumptions (flagged in PENDING.md):
#   - API_GetUserRecentAchievements entries: AchievementID, Title, Points,
#     Date, GameID, HardcoreMode, and the 'm' (minutes) request param.
#     A real live unlock (odelot fork) is needed to confirm.
#   - API_GetUserSummary: LastGameID field. Verifiable with curl.
# =============================================================================

import json
import os
import re
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

_INDEX_TTL_SECONDS    = 7 * 24 * 3600   # console hash index refresh
_PROGRESS_TTL_SECONDS = 30              # progress aggregate cache
_NEGATIVE_TTL_SECONDS = 120             # retry window for unresolved dumps
_POLL_INTERVAL_SECONDS = 60             # unlock polling cadence
_RECENT_WINDOW_MINUTES = 15             # lookback for GetUserRecentAchievements

_API_BASE     = "https://retroachievements.org/API"
_HTTP_TIMEOUT = 15

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
    "sega master system/game gear": "sms",       # (?) verify friendly string
    "atari 2600":                   "atari7800",  # RA 2600 runs via 7800 core
}

# --- Internal RA key -> consoleID(s), verified via API_GetConsoleIDs ----------
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

# --- Module state --------------------------------------------------------------
# _index_lock guards the per-console hash maps.
# _ra_lock guards context, events and the progress/resolution caches.
# The two locks are never nested.

_index_maps = {}
_index_lock = threading.Lock()

_ra_lock = threading.Lock()

_ra_context = {          # what the polling thread believes is active
    "seq": None,         # server _state['seq'] at last successful resolution
    "game_id": 0,
}

_events = {              # unlock event state (popup source for the firmware)
    "counter": 0,        # monotonic across games — firmware fires on change
    "seen": set(),       # AchievementIDs already observed for baseline logic
    "baseline_done": False,
    "last_title": "",
    "last_points": 0,
    "last_hardcore": False,
    "last_date": "",
}

_res_cache = {           # expensive hash+resolution, keyed by ROM identity
    "key": None,         # (rom_path, internal_path)
    "hash": "",
    "note": "",
    "gid": 0,
    "cid": None,
    "method": "",        # "index" | "lastgame"
    "title": "",         # title from fallback progress (avoids refetch)
    "ts": 0.0,
}

_progress_cache = {      # aggregated progress, short TTL
    "gid": 0,
    "ts": 0.0,
    "data": None,
}

_state_getter  = None    # injected by start_ra_polling()
_poll_started  = False


# --- Credentials ---------------------------------------------------------------

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


# --- RA Web API calls ------------------------------------------------------------

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


# --- Progress aggregation ------------------------------------------------------

def _fetch_progress_aggregate(gid, user, key):
    """
    Calls API_GetGameInfoAndUserProgress and reduces it to a flat aggregate.
    Returns dict or None. Field names verified on hardware for Title,
    NumAchievements, NumAwardedToUser, Achievements{Points,DateEarned,
    DateEarnedHardcore}.
    """
    prog = _api_get("API_GetGameInfoAndUserProgress.php",
                    {'g': gid, 'u': user, 'y': key})
    if prog is None:
        return None

    def _int(v):
        try:
            return int(v or 0)
        except (TypeError, ValueError):
            return 0

    total    = _int(prog.get("NumAchievements"))
    unlocked = _int(prog.get("NumAwardedToUser"))

    points_total = points_earned = points_hc = 0
    unlocked_hc = 0
    ach = prog.get("Achievements", {})
    if isinstance(ach, dict):
        for a in ach.values():
            p = _int(a.get("Points"))
            points_total += p
            earned_hc = bool(a.get("DateEarnedHardcore"))
            earned    = earned_hc or bool(a.get("DateEarned"))
            if earned:
                points_earned += p
            if earned_hc:
                unlocked_hc += 1
                points_hc += p

    return {
        "title": str(prog.get("Title", "") or ""),
        "total": total,
        "unlocked": unlocked,
        "unlocked_hardcore": unlocked_hc,
        "points_total": points_total,
        "points_earned": points_earned,
        "points_hardcore": points_hc,
    }


def _get_progress_cached(gid, user, key):
    now = time.time()
    with _ra_lock:
        if (_progress_cache["gid"] == gid and _progress_cache["data"] is not None
                and now - _progress_cache["ts"] < _PROGRESS_TTL_SECONDS):
            return _progress_cache["data"]
    data = _fetch_progress_aggregate(gid, user, key)
    if data is not None:
        with _ra_lock:
            _progress_cache["gid"]  = gid
            _progress_cache["ts"]   = now
            _progress_cache["data"] = data
    return data


# --- Title corroboration (LastGameID fallback) --------------------------------

def _norm_title(s):
    return re.sub(r'[^a-z0-9]', '', (s or '').lower())


def _titles_match(local_name, ra_title):
    """
    Conservative corroboration: normalized containment either way, with a
    minimum length so trivial fragments can't match. 'Super Metroid' vs
    'Super Metroid' -> exact; 'Final Fantasy VII (Disc 1)' vs
    'Final Fantasy VII' -> containment.
    """
    a, b = _norm_title(local_name), _norm_title(ra_title)
    if len(a) < 4 or len(b) < 4:
        return False
    return a in b or b in a


# --- Active ROM resolution (delegates to the server's own resolver) -----------

def _get_active_rom(handler):
    """
    Returns (core_friendly, resolved_path, internal_path, search_name).
    All path/state logic stays in the server (get_current_core /
    get_rom_details); we only consume its resolved fields.
    """
    core = ""
    try:
        core = handler.get_current_core() or ""
    except Exception as e:
        print(f"[RA] get_current_core failed: {e}")

    if not core or core.strip().lower() == "menu":
        return core, None, None, ""

    try:
        details = handler.get_rom_details()
    except Exception as e:
        print(f"[RA] get_rom_details failed: {e}")
        return core, None, None, ""

    if not isinstance(details, dict) or not details.get("available"):
        return core, None, None, ""

    search_name = str(details.get("search_name", "") or "")

    zip_path = details.get("resolved_zip_path") or details.get("zip_path")
    internal = details.get("internal_path")
    if zip_path and internal:
        return core, zip_path, internal, search_name

    plain = details.get("path") or ""
    if plain and os.path.exists(plain):
        return core, plain, None, search_name

    return core, None, None, search_name


def _read_seq():
    """Read the server's state seq via the injected getter, or None."""
    if _state_getter is None:
        return None
    try:
        state, lock = _state_getter()
        with lock:
            return state.get('seq')
    except Exception:
        return None


# --- Unlock polling thread -----------------------------------------------------

def _poll_once(user, key):
    """
    One polling cycle. Split out from the thread loop for testability.
    """
    seq = _read_seq()

    with _ra_lock:
        # Game changed since last resolution? Drop context; the endpoint will
        # re-resolve on its next call and re-arm the baseline.
        if (seq is not None and _ra_context["seq"] is not None
                and seq != _ra_context["seq"]):
            print("[RA] 🛰️ game changed (seq) — resetting unlock baseline")
            _ra_context["seq"] = None
            _ra_context["game_id"] = 0
            _events["seen"].clear()
            _events["baseline_done"] = False
            return
        gid = _ra_context["game_id"]

    if not gid:
        return

    recent = _api_get("API_GetUserRecentAchievements.php",
                      {'u': user, 'y': key, 'm': _RECENT_WINDOW_MINUTES})
    if not isinstance(recent, list):
        return

    with _ra_lock:
        if not _events["baseline_done"]:
            # First poll for this game: absorb pre-existing unlocks silently
            # so we never popup something that happened before we watched.
            for a in recent:
                aid = a.get("AchievementID")
                if aid is not None:
                    _events["seen"].add(aid)
            _events["baseline_done"] = True
            print(f"[RA] 🛰️ baseline set ({len(_events['seen'])} recent absorbed)")
            return

        fired = False
        for a in recent:
            aid = a.get("AchievementID")
            if aid is None or aid in _events["seen"]:
                continue
            _events["seen"].add(aid)
            try:
                agid = int(a.get("GameID") or 0)
            except (TypeError, ValueError):
                agid = 0
            if agid != gid:
                continue   # unlock from another game/device — seen, not fired
            _events["counter"] += 1
            _events["last_title"]    = str(a.get("Title", "") or "")
            try:
                _events["last_points"] = int(a.get("Points", 0) or 0)
            except (TypeError, ValueError):
                _events["last_points"] = 0
            try:
                _events["last_hardcore"] = bool(int(a.get("HardcoreMode", 0) or 0))
            except (TypeError, ValueError):
                _events["last_hardcore"] = False
            _events["last_date"] = str(a.get("Date", "") or "")
            fired = True
            print(f"[RA] 🏆 unlock: {_events['last_title']} "
                  f"(+{_events['last_points']}{' HC' if _events['last_hardcore'] else ''})")
        if fired:
            _progress_cache["ts"] = 0.0   # next endpoint call refetches counts


def _ra_poll_thread():
    print(f"[RA] 🛰️ unlock polling thread started ({_POLL_INTERVAL_SECONDS}s)")
    while True:
        time.sleep(_POLL_INTERVAL_SECONDS)
        try:
            user, key = _load_credentials()
            if user and key:
                _poll_once(user, key)
        except Exception as e:
            print(f"[RA] poll error: {e}")


def start_ra_polling(state_getter):
    """
    Called once from the server's __main__:
        start_ra_polling(lambda: (_state, _state_lock))
    Injects live-state access and starts the daemon polling thread.
    """
    global _state_getter, _poll_started
    _state_getter = state_getter
    if _poll_started:
        return
    _poll_started = True
    threading.Thread(target=_ra_poll_thread, daemon=True).start()


# --- Public entry point ----------------------------------------------------------

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
        "unlocked_hardcore": 0,
        "points_earned": 0,
        "points_total": 0,
        "points_hardcore": 0,
        "match_method": "",
        "event_counter": 0,
        "last_unlock_title": "",
        "last_unlock_points": 0,
        "last_unlock_hardcore": False,
        "last_unlock_date": "",
        "polling": _poll_started,
        "ssl_verified": _SSL_VERIFIED,
        "timestamp": now,
    }

    # Events are attached to every response so the firmware can watch the
    # counter from any page, whatever the resolution outcome below.
    def _attach_events():
        with _ra_lock:
            out["event_counter"]        = _events["counter"]
            out["last_unlock_title"]    = _events["last_title"]
            out["last_unlock_points"]   = _events["last_points"]
            out["last_unlock_hardcore"] = _events["last_hardcore"]
            out["last_unlock_date"]     = _events["last_date"]

    user, key = _load_credentials()
    if not user or not key:
        out["status"] = "not_configured"
        _attach_events()
        return out
    out["enabled"] = True

    core_friendly, rom_path, internal, search_name = _get_active_rom(handler)
    out["core"] = core_friendly

    ra_key = FRIENDLY_TO_KEY.get((core_friendly or "").strip().lower(), "")
    if not ra_key or ra_key not in CORE_TO_CONSOLE_IDS or not is_core_supported(ra_key):
        out["status"] = "core_not_supported"
        _attach_events()
        return out
    out["supported"] = True
    out["ra_key"] = ra_key
    console_ids = CORE_TO_CONSOLE_IDS[ra_key]

    if not rom_path:
        out["status"] = "no_game_loaded"
        _attach_events()
        return out

    # --- Resolution (hash + index + fallback), cached per ROM identity -------
    cache_key = (rom_path, internal)
    now_f = time.time()

    with _ra_lock:
        cached = dict(_res_cache) if _res_cache["key"] == cache_key else None

    use_cached = False
    if cached:
        if cached["gid"] > 0:
            use_cached = True
        elif now_f - cached["ts"] < _NEGATIVE_TTL_SECONDS:
            use_cached = True   # recent negative — don't hammer the API

    if use_cached:
        ra_hash_hex   = cached["hash"]
        gid           = cached["gid"]
        match_method  = cached["method"]
        fb_title      = cached["title"]
    else:
        hres = compute_ra_hash(ra_key, rom_path, internal)
        if hres["error"] or not hres["hash"]:
            out["status"] = "hash_error"
            out["detail"] = hres["error"] or "no hash produced"
            _attach_events()
            return out
        ra_hash_hex = hres["hash"].lower()

        gid, _cid = _resolve_game_id(console_ids, ra_hash_hex, user, key)
        match_method = "index" if gid else ""
        fb_title = ""

        # --- LastGameID fallback with title corroboration --------------------
        if gid == 0 and search_name:
            summary = _api_get("API_GetUserSummary.php",
                               {'u': user, 'y': key, 'g': 1, 'a': 1})
            last_gid = 0
            if isinstance(summary, dict):
                try:
                    last_gid = int(summary.get("LastGameID") or 0)
                except (TypeError, ValueError):
                    last_gid = 0
            if last_gid:
                agg = _get_progress_cached(last_gid, user, key)
                if agg and _titles_match(search_name, agg["title"]):
                    gid = last_gid
                    match_method = "lastgame"
                    fb_title = agg["title"]
                    print(f"[RA] fallback corroborated: '{search_name}' ~ "
                          f"'{agg['title']}' (game {gid})")
                elif agg:
                    print(f"[RA] fallback rejected: '{search_name}' !~ "
                          f"'{agg['title']}'")

        with _ra_lock:
            _res_cache.update({
                "key": cache_key, "hash": ra_hash_hex, "gid": gid,
                "method": match_method, "title": fb_title, "ts": now_f,
            })

    out["ra_hash"] = ra_hash_hex

    if gid == 0:
        out["status"] = "rom_not_recognized"
        _attach_events()
        return out

    out["game_matched"] = True
    out["game_id"] = gid
    out["match_method"] = match_method

    # Arm/refresh the polling context for this game.
    seq = _read_seq()
    with _ra_lock:
        if _ra_context["game_id"] != gid:
            _events["seen"].clear()
            _events["baseline_done"] = False
            _events["last_title"] = ""
            _events["last_points"] = 0
            _events["last_hardcore"] = False
            _events["last_date"] = ""
        _ra_context["game_id"] = gid
        _ra_context["seq"] = seq

    agg = _get_progress_cached(gid, user, key)
    if agg is None:
        out["status"] = "progress_unavailable"
        _attach_events()
        return out

    out["game_title"]        = agg["title"]
    out["total"]             = agg["total"]
    out["unlocked"]          = agg["unlocked"]
    out["unlocked_hardcore"] = agg["unlocked_hardcore"]
    out["points_total"]      = agg["points_total"]
    out["points_earned"]     = agg["points_earned"]
    out["points_hardcore"]   = agg["points_hardcore"]
    out["status"] = "ok"
    _attach_events()
    return out
