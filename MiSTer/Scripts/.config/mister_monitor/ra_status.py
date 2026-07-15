#!/usr/bin/env python3
# =============================================================================
# ra_status.py — RetroAchievements status resolver for MiSTer Monitor (A5 live)
# =============================================================================
# Full version. Adds on top of the minimal one:
#   - hardcore breakdown (unlocked_hardcore / points_hardcore)
#   - LastGameID fallback with TITLE CORROBORATION for unindexed dumps and
#     CD systems (only trusted when RA's title matches the local search_name)
#   - background unlock polling thread (event_counter + last_unlock_* fields)
#
# A5 live layer (all optional — every tier degrades to the one below it):
#   - odelot debug-log tailer: with debug=1 in /media/fat/retroachievements.cfg
#     the fork writes /tmp/ra_debug.log; the tailer turns ACHIEVEMENT TRIGGERED
#     lines into instant events (<1 s, with title AND description, no cloud
#     roundtrip) and captures the rc_hash of every load ("ROM MD5"), which
#     resolves CD systems (PSX/Saturn/MegaCD) we cannot hash locally.
#     tmpfs RAM guard included — see the tailer section.
#   - OSD trigger: /tmp/OSD_VISIBLE (fork writes it when MiSTer.ini has
#     log_file_entry=1) debounces an immediate cloud poll, ~3-8 s latency
#     without needing debug=1.
#   - 60 s cloud poll: unchanged last-resort tier, now also woken on demand
#     by the two tiers above (and it backfills points/hardcore into
#     log-sourced events).
#   - get_ra_event(): ~60-byte payload for the firmware's 5 s micro-poll.
#   - get_ra_achievements(): flat paginated trophy list for the firmware
#     subpages, served from the rows the progress fetch already downloads
#     (zero extra RA API calls).
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

_LIST_PER_PAGE_DEFAULT = 6              # trophy-list rows per firmware page
_LIST_PER_PAGE_MAX     = 8              # single-digit a{i}_ indices, always
_LIST_TITLE_MAX        = 64             # server-side cap for row titles
_LIST_DESC_MAX         = 120            # server-side cap for row descriptions

_RA_LOG_PATH         = "/tmp/ra_debug.log"   # odelot fork, debug=1
_OSD_FLAG_PATH       = "/tmp/OSD_VISIBLE"    # fork, MiSTer.ini log_file_entry=1
_LOG_POLL_SECONDS    = 0.5                   # tail/OSD watch cadence
_LOG_READ_CHUNK      = 65536                 # max bytes per incremental read
_LOG_GUARD_BYTES     = 8 * 1024 * 1024       # RAM guard threshold (tmpfs)
_LOG_MD5_TTL_SECONDS = 600                   # trust window for a logged hash
_OSD_DEBOUNCE_SECONDS = 2.0                  # min gap between OSD-fired polls

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

# --- Friendly core name -> internal RA key (normalized, ordered rules) --------
# The server merges the user's names.txt into CORE_NAME_MAPPING, so friendly
# names are NOT deterministic across installs ('Game Boy' on one MiSTer,
# 'Nintendo Game Boy' on another). Instead of exact strings we normalize the
# friendly (lowercase, alphanumerics only) and apply substring rules in
# specificity order: GBA before Game Boy, 32X before Genesis/Mega Drive,
# SNES before NES. First hit wins.

_FRIENDLY_RULES = [
    # (normalized substrings — ANY match, key)
    (("gameboyadvance", "gba"),                        "gba"),
    (("gameboy",),                                     "gameboy"),
    (("32x",),                                         "s32x"),
    (("megadrive", "genesis"),                         "genesis"),
    (("supernintendo", "superfamicom", "snes"),        "snes"),
    (("nesfamicom", "nintendones", "famicom", "nes"),  "nes"),
    (("nintendo64", "n64"),                            "n64"),
    (("turbografx", "pcengine", "tgfx"),               "tgfx16"),
    (("mastersystem", "gamegear", "sms"),              "sms"),
    (("atari7800",),                                   "atari7800"),
    (("atari2600",),                                   "atari2600"),
    # CD systems: no local hasher (is_core_supported() is False for these).
    # They resolve via the odelot log hash or the corroborated LastGameID
    # fallback — see the resolution block in get_ra_status().
    (("playstation", "psx"),                           "psx"),
    (("saturn",),                                      "saturn"),
    (("megacd", "segacd"),                             "megacd"),
]

def _friendly_to_key(core_friendly):
    """Resolve a friendly core name to an internal RA key, tolerant of
    names.txt variants. Returns '' when no rule matches (unsupported)."""
    n = re.sub(r'[^a-z0-9]', '', (core_friendly or '').lower())
    if not n or n == 'menu':
        return ''
    for needles, key in _FRIENDLY_RULES:
        if any(x in n for x in needles):
            return key
    return ''

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
    'atari2600':    [25, 51],  # sniffed 2600 game: 2600 index first
    's32x':         [10],
    # CD systems (log-hash / LastGameID resolution only):
    'psx':          [12],
    'saturn':       [39],
    'megacd':       [9],
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
    "last_desc": "",         # description — filled by the log tailer
    "pending_backfill": 0,   # AchievementID the cloud poll should enrich
}

_res_cache = {           # expensive hash+resolution, keyed by ROM identity
    "key": None,         # (rom_path, internal_path)
    "hash": "",
    "note": "",
    "err": "",          # hash error text (negative-cached like misses)
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

_poll_now = threading.Event()   # set by the tailer/OSD to wake the poll thread

_log_state = {           # odelot debug-log tailer findings (guarded by _ra_lock)
    "active": False,     # /tmp/ra_debug.log present on the last check
    "md5": "",           # last 'ROM MD5:' seen this fork session
    "md5_path": "",      # path from the preceding 'Hashing ROM:' line
    "md5_ts": 0.0,
}


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


_FLAT_BAD_CHARS = re.compile(r'[\x00-\x1f"\\]')

def _flat_str(s, maxlen=_LIST_TITLE_MAX):
    """Make a string safe for the flat-JSON contract: the firmware's naive
    extractStringValue() stops at the first quote and cannot unescape, so
    quotes, backslashes and control chars are flattened to spaces here."""
    s = _FLAT_BAD_CHARS.sub(' ', str(s or ''))
    s = re.sub(r'\s+', ' ', s).strip()
    return s[:maxlen]


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
    Calls API_GetGameInfoAndUserProgress and reduces it to a flat aggregate
    PLUS the per-achievement rows that feed get_ra_achievements(). Returns
    dict or None. Field names verified on hardware for Title, NumAchievements,
    NumAwardedToUser, Achievements{Points,DateEarned,DateEarnedHardcore};
    ID/Title/DisplayOrder/DateEarnedSoftcore additionally confirmed against
    MiSTer Companion's ra_viewer, which consumes this same endpoint in
    production (dict OR list shape for Achievements — both handled).
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
    rows = []
    ach = prog.get("Achievements", {})
    if isinstance(ach, dict):
        items = list(ach.values())
    elif isinstance(ach, list):
        items = ach
    else:
        items = []
    for a in items:
        if not isinstance(a, dict):
            continue
        p = _int(a.get("Points"))
        points_total += p
        earned_hc = bool(a.get("DateEarnedHardcore"))
        earned    = (earned_hc or bool(a.get("DateEarned"))
                     or bool(a.get("DateEarnedSoftcore")))
        if earned:
            points_earned += p
        if earned_hc:
            unlocked_hc += 1
            points_hc += p
        rows.append({
            "id":       _int(a.get("ID")),
            "title":    _flat_str(a.get("Title"), _LIST_TITLE_MAX),
            "desc":     _flat_str(a.get("Description"), _LIST_DESC_MAX),
            "points":   p,
            "unlocked": earned,
            "hardcore": earned_hc,
            "order":    _int(a.get("DisplayOrder")),
        })
    rows.sort(key=lambda r: (r["order"], r["id"]))
    for r in rows:
        r.pop("order", None)

    return {
        "title": str(prog.get("Title", "") or ""),
        "total": total,
        "unlocked": unlocked,
        "unlocked_hardcore": unlocked_hc,
        "points_total": points_total,
        "points_earned": points_earned,
        "points_hardcore": points_hc,
        "rows": rows,
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


# =============================================================================
# --- Live layer: odelot debug-log tailer + OSD trigger ------------------------
# =============================================================================
# With debug=1 in /media/fat/retroachievements.cfg the odelot fork writes
# /tmp/ra_debug.log (fopen "w" per RA-core start, fflush() after every line).
# The format anchors below are taken verbatim from the fork's achievements.cpp
# (the RA_LOG macro prefixes every line with "RA: "):
#     RA: *** ACHIEVEMENT TRIGGERED: [%u] %s \u2014 %s ***    (em dash U+2014)
#     RA: Hashing ROM: %s (%u bytes)
#     RA: ROM MD5: %s
#     RA: *** GAME COMPLETED! ***
# A debug log is NOT an API contract, so everything parses defensively and the
# OSD trigger + 60 s cloud poll stay underneath as fallback tiers: a format
# change in a future fork release degrades latency, never correctness.
#
# RAM safety: /tmp is tmpfs, and the WRITER (the fork) is what grows it — the
# per-session fopen("w") already bounds the file to one core session, and this
# tailer adds a guard on top: once the unconsumed region would exceed
# _LOG_GUARD_BYTES it punches a hole over the already-read bytes
# (fallocate PUNCH_HOLE|KEEP_SIZE — tmpfs frees those pages while the fork's
# non-O_APPEND fd keeps writing at its own offset, so neither side notices).
# Where hole punching is unavailable it falls back to truncate(0); the one-off
# re-read of the sparse gap is NUL-stripped and costs a single cheap pass.
# Net effect: enabling debug=1 with this server running is SAFER than enabling
# it bare, because the fork gains the log rotation it does not have.

_RE_LOG_UNLOCK = re.compile(
    r'ACHIEVEMENT TRIGGERED:\s*(?:\[(\d+)\]\s*)?(.*?)\s*\*{0,3}\s*$')
_RE_LOG_MD5   = re.compile(r'ROM MD5:\s*([0-9a-fA-F]{32})\s*$')
_RE_LOG_HASHP = re.compile(r'Hashing ROM:\s*(.+?)\s*(?:\(\d+\s*bytes\))?\s*$')


def _same_rom(log_path, rom_path, internal):
    """The logged hash is only trusted for the ROM that is actually active:
    basename equality against either the resolved path or the in-zip entry.
    Strict on purpose — a stale hash must never resolve the wrong game."""
    if not log_path:
        return False
    lb = os.path.basename(log_path).lower()
    for cand in (rom_path, internal):
        if cand and os.path.basename(cand).lower() == lb:
            return True
    return False


def _log_fire_unlock(aid, title, desc):
    """Instant unlock event from the log: bump the counter NOW with the local
    title/description; points and hardcore mode arrive seconds later when the
    woken cloud poll backfills them (matched via pending_backfill)."""
    with _ra_lock:
        _events["counter"] += 1
        _events["last_title"]    = _flat_str(title, 96) or "Achievement unlocked"
        _events["last_desc"]     = _flat_str(desc, 120)
        _events["last_points"]   = 0
        _events["last_hardcore"] = False
        _events["last_date"]     = ""
        if aid:
            _events["seen"].add(aid)
            _events["pending_backfill"] = aid
        _progress_cache["ts"] = 0.0   # counts refresh on the next status call
    print(f"[RA] \u26a1 log unlock: {title}" + (f" \u2014 {desc}" if desc else ""))
    _poll_now.set()


def _log_handle_line(line):
    """Route one decoded log line. Unknown lines are simply ignored."""
    if not line.startswith("RA:"):
        return
    if "ACHIEVEMENT TRIGGERED" in line:
        m = _RE_LOG_UNLOCK.search(line)
        aid, rest = 0, ""
        if m:
            aid = int(m.group(1)) if m.group(1) else 0
            rest = m.group(2) or ""
        title, desc = rest, ""
        for dash in ("\u2014", "\u2013"):    # em dash (source) / en dash (safety)
            sep = f" {dash} "
            if sep in rest:
                title, desc = rest.split(sep, 1)
                break
        _log_fire_unlock(aid, title.strip(), desc.strip())
        return
    if "GAME COMPLETED!" in line:
        _log_fire_unlock(0, "GAME COMPLETED!", "Congratulations!")
        return
    m = _RE_LOG_MD5.search(line)
    if m:
        with _ra_lock:
            _log_state["md5"] = m.group(1).lower()
            _log_state["md5_ts"] = time.time()
        print(f"[RA] \U0001f4dc log hash captured: {_log_state['md5'][:8]}\u2026")
        return
    if "Hashing ROM" in line:
        m = _RE_LOG_HASHP.search(line)
        if m:
            with _ra_lock:
                _log_state["md5_path"] = m.group(1)


def _punch_hole(fd, offset, length):
    """fallocate(FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE) via ctypes. Raises
    OSError when the libc symbol or the filesystem does not support it."""
    if length <= 0:
        return
    import ctypes, ctypes.util
    libc = ctypes.CDLL(ctypes.util.find_library("c") or "libc.so.6",
                       use_errno=True)
    fn = getattr(libc, "fallocate64", None) or libc.fallocate
    fn.argtypes = [ctypes.c_int, ctypes.c_int,
                   ctypes.c_longlong, ctypes.c_longlong]
    fn.restype = ctypes.c_int
    if fn(fd, 0x01 | 0x02, offset, length) != 0:   # KEEP_SIZE | PUNCH_HOLE
        raise OSError(ctypes.get_errno(), "fallocate punch failed")


class _LogTailer:
    """Incremental, offset-based reader of the fork's debug log.

    Invariants:
      - starts at EOF (never replays a session's history, never reads a hole
        left by a previous guard action)
      - size < offset  =>  the fork reopened with "w" (new core session):
        restart from 0 and drop the session-scoped hash
      - NULs are stripped BEFORE buffering, so sparse gaps cost nothing and
        can never balloon the partial-line buffer
    """

    def __init__(self, path):
        self.path = path
        self.offset = None      # None = not positioned yet
        self.buf = b""
        self.hole_end = 0       # bytes already reclaimed by the RAM guard
        self.punch_ok = True

    def poke(self):
        try:
            size = os.stat(self.path).st_size
        except OSError:
            if self.offset is not None:
                print("[RA] \U0001f4dc debug log gone \u2014 tail idle")
            self.offset = None
            self.buf = b""
            self.hole_end = 0
            with _ra_lock:
                _log_state["active"] = False
            return

        with _ra_lock:
            first = not _log_state["active"]
            _log_state["active"] = True

        if self.offset is None:
            self.offset = size
            self.buf = b""
            self.hole_end = 0
            if first:
                print(f"[RA] \U0001f4dc tailing {self.path} from EOF ({size} B)")
            return

        if size < self.offset:
            print(f"[RA] \U0001f4dc log restarted (new RA session, {size} B)")
            self.offset = 0
            self.buf = b""
            self.hole_end = 0
            with _ra_lock:
                _log_state["md5"] = ""
                _log_state["md5_path"] = ""
                _log_state["md5_ts"] = 0.0

        while self.offset < size:
            want = min(_LOG_READ_CHUNK, size - self.offset)
            try:
                with open(self.path, "rb") as f:
                    f.seek(self.offset)
                    chunk = f.read(want)
            except OSError as e:
                print(f"[RA] \U0001f4dc read failed: {e}")
                return
            if not chunk:
                break
            self.offset += len(chunk)
            self._feed(chunk)

        if size - self.hole_end > _LOG_GUARD_BYTES and self.offset >= size:
            self._reclaim()

    def _feed(self, chunk):
        chunk = chunk.replace(b"\x00", b"")   # sparse-gap bytes vanish here
        if not chunk:
            return
        self.buf += chunk
        if len(self.buf) > 16384:              # defensive: runaway partial line
            self.buf = self.buf[-4096:]
        *lines, self.buf = self.buf.split(b"\n")
        for raw in lines:
            line = raw.decode("utf-8", "replace").strip()
            if line:
                try:
                    _log_handle_line(line)
                except Exception as e:
                    print(f"[RA] \U0001f4dc parse error: {e}")

    def _reclaim(self):
        """tmpfs RAM guard over the already-consumed region [hole_end, offset)."""
        if self.punch_ok:
            try:
                fd = os.open(self.path, os.O_RDWR)
                try:
                    _punch_hole(fd, self.hole_end, self.offset - self.hole_end)
                finally:
                    os.close(fd)
                print(f"[RA] \U0001f4dc RAM guard: punched "
                      f"{(self.offset - self.hole_end) >> 20} MiB hole")
                self.hole_end = self.offset
                return
            except OSError as e:
                self.punch_ok = False
                print(f"[RA] \U0001f4dc hole punch unavailable ({e}) \u2014 "
                      f"falling back to truncate")
        try:
            os.truncate(self.path, 0)
            print("[RA] \U0001f4dc RAM guard: truncated log to 0 (fallback)")
            # The fork keeps writing at its own offset; re-reading the sparse
            # gap from 0 is cheap because _feed() strips the NULs.
            self.offset = 0
            self.buf = b""
            self.hole_end = 0
        except OSError as e:
            print(f"[RA] \U0001f4dc RAM guard failed: {e}")


def _live_watch_thread():
    """One 0.5 s loop drives both live tiers: the log tail and the OSD
    trigger. Idle cost when neither file exists: two failed stats/second."""
    tail = _LogTailer(_RA_LOG_PATH)
    last_osd_ns = 0
    last_osd_fire = 0.0
    print(f"[RA] \u26a1 live watcher started (log tail + OSD trigger, "
          f"{_LOG_POLL_SECONDS}s)")
    while True:
        try:
            tail.poke()
            try:
                ns = os.stat(_OSD_FLAG_PATH).st_mtime_ns
            except OSError:
                ns = 0
            if ns and ns != last_osd_ns:
                if last_osd_ns and time.time() - last_osd_fire > _OSD_DEBOUNCE_SECONDS:
                    last_osd_fire = time.time()
                    _poll_now.set()
                last_osd_ns = ns
        except Exception as e:
            print(f"[RA] watcher error: {e}")
        time.sleep(_LOG_POLL_SECONDS)


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
            _events["pending_backfill"] = 0
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
        pending = _events.get("pending_backfill", 0)
        for a in recent:
            aid = a.get("AchievementID")
            if aid is None:
                continue
            if aid in _events["seen"]:
                if pending and aid == pending:
                    # The log tailer already fired this unlock (with title and
                    # description); the cloud entry only enriches the fields
                    # the log does not carry. No counter bump.
                    try:
                        _events["last_points"] = int(a.get("Points", 0) or 0)
                    except (TypeError, ValueError):
                        pass
                    try:
                        _events["last_hardcore"] = bool(int(a.get("HardcoreMode", 0) or 0))
                    except (TypeError, ValueError):
                        pass
                    _events["last_date"] = str(a.get("Date", "") or "")
                    _events["pending_backfill"] = 0
                    pending = 0
                    _progress_cache["ts"] = 0.0
                    print(f"[RA] \U0001f6f0\ufe0f backfilled log unlock #{aid} "
                          f"(+{_events['last_points']}"
                          f"{' HC' if _events['last_hardcore'] else ''})")
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
            _events["last_desc"] = _flat_str(a.get("Description", ""), 120)
            fired = True
            print(f"[RA] 🏆 unlock: {_events['last_title']} "
                  f"(+{_events['last_points']}{' HC' if _events['last_hardcore'] else ''})")
        if fired:
            _progress_cache["ts"] = 0.0   # next endpoint call refetches counts


def _ra_poll_thread():
    print(f"[RA] 🛰️ unlock polling thread started "
          f"({_POLL_INTERVAL_SECONDS}s cadence + instant triggers)")
    while True:
        triggered = _poll_now.wait(timeout=_POLL_INTERVAL_SECONDS)
        _poll_now.clear()
        if triggered:
            # Give retroachievements.org a beat to register the unlock the
            # log/OSD tier just reported before asking for it.
            time.sleep(1.0)
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
    threading.Thread(target=_live_watch_thread, daemon=True).start()


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
        "last_unlock_description": "",
        "polling": _poll_started,
        "log_tail": False,
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
            out["last_unlock_description"] = _events["last_desc"]
            out["log_tail"]             = bool(_log_state["active"])

    user, key = _load_credentials()
    if not user or not key:
        out["status"] = "not_configured"
        _attach_events()
        return out
    out["enabled"] = True

    core_friendly, rom_path, internal, search_name = _get_active_rom(handler)
    out["core"] = core_friendly

    ra_key = _friendly_to_key(core_friendly)
    # is_core_supported() now gates only LOCAL hashing: CD cores (psx/saturn/
    # megacd) have no hasher but still resolve via the odelot log hash or the
    # corroborated LastGameID fallback in the resolution block below.
    if not ra_key or ra_key not in CORE_TO_CONSOLE_IDS:
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
        hash_err      = cached.get("err", "")
    else:
        hashable    = is_core_supported(ra_key)
        ra_hash_hex = ""
        hash_err    = ""
        if hashable:
            hres = compute_ra_hash(ra_key, rom_path, internal)
            if hres["error"] or not hres["hash"]:
                hash_err = hres["error"] or "no hash produced"
            else:
                ra_hash_hex = hres["hash"].lower()

        gid = 0
        match_method = ""
        if ra_hash_hex:
            gid, _cid = _resolve_game_id(console_ids, ra_hash_hex, user, key)
            if gid:
                match_method = "index"

        # --- odelot debug-log hash assist -------------------------------------
        # With debug=1 the fork logs the exact rc_hash of every load. That is
        # the ONLY hash source for CD systems, and it rescues local hash
        # failures on cartridges. Trusted strictly: the logged path basename
        # must match the active ROM (or its in-zip entry), and the capture
        # must be fresh — a stale hash must never resolve the wrong game.
        if gid == 0:
            with _ra_lock:
                log_md5  = _log_state["md5"]
                log_path = _log_state["md5_path"]
                log_ts   = _log_state["md5_ts"]
            if (log_md5 and log_md5 != ra_hash_hex
                    and time.time() - log_ts < _LOG_MD5_TTL_SECONDS
                    and _same_rom(log_path, rom_path, internal)):
                gid, _cid = _resolve_game_id(console_ids, log_md5, user, key)
                if gid:
                    match_method = "log_hash"
                    ra_hash_hex  = log_md5
                    print(f"[RA] \U0001f4dc matched via odelot log hash "
                          f"({log_md5[:8]}\u2026)")

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
                "method": match_method, "title": fb_title,
                "err": hash_err, "ts": now_f,
            })

    out["ra_hash"] = ra_hash_hex

    if gid == 0:
        # A hash failure is only the verdict when no rescue tier matched
        # either; it enjoys the same negative TTL as a plain miss instead of
        # re-hashing a broken file on every request.
        if hash_err:
            out["status"] = "hash_error"
            out["detail"] = hash_err
        else:
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
            _events["last_desc"] = ""
            _events["pending_backfill"] = 0
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


def get_ra_event():
    """Micro-endpoint payload for /status/retroachievements/event: ~60 bytes
    the firmware polls every few seconds. A moving counter tells it to run a
    full status fetch immediately (which arms the unlock popup)."""
    with _ra_lock:
        return {
            "event_counter": _events["counter"],
            "log_tail": bool(_log_state["active"]),
            "timestamp": int(time.time()),
        }


def get_ra_achievements(handler, page=1, per_page=_LIST_PER_PAGE_DEFAULT):
    """
    Flat, paginated trophy list for the ACTIVE game — feeds the firmware's
    page-6 subpages. Resolution is delegated to get_ra_status() (all caches
    apply) and the rows come from the payload the progress fetch already
    downloads, so this endpoint costs ZERO extra RA API calls.

    Flat-JSON contract: a{i}_id / a{i}_title / a{i}_points / a{i}_unlocked /
    a{i}_hardcore for i in [0, count). per_page is clamped to 1..8 so the
    indices stay single-digit and the firmware's substring key matcher can
    never confuse a1_ with a10_.
    """
    try:
        per = max(1, min(int(per_page), _LIST_PER_PAGE_MAX))
    except (TypeError, ValueError):
        per = _LIST_PER_PAGE_DEFAULT
    try:
        pg = int(page)
    except (TypeError, ValueError):
        pg = 1

    base = get_ra_status(handler)
    out = {
        "status": base.get("status", ""),
        "game_id": base.get("game_id", 0),
        "game_title": _flat_str(base.get("game_title", ""), 64),
        "total": 0,
        "page": 0,
        "pages": 0,
        "per_page": per,
        "count": 0,
        "timestamp": int(time.time()),
    }
    if base.get("status") != "ok" or not base.get("game_id"):
        return out

    with _ra_lock:
        data = (_progress_cache["data"]
                if _progress_cache["gid"] == base["game_id"] else None)
    rows = list((data or {}).get("rows") or [])

    total = len(rows)
    out["total"] = total
    if total == 0:
        return out

    pages = (total + per - 1) // per
    pg = max(1, min(pg, pages))
    start = (pg - 1) * per
    chunk = rows[start:start + per]

    out["page"] = pg
    out["pages"] = pages
    out["count"] = len(chunk)
    for i, r in enumerate(chunk):
        out[f"a{i}_id"]       = r["id"]
        out[f"a{i}_title"]    = r["title"]
        out[f"a{i}_desc"]     = r["desc"]
        out[f"a{i}_points"]   = r["points"]
        out[f"a{i}_unlocked"] = r["unlocked"]
        out[f"a{i}_hardcore"] = r["hardcore"]
    return out
