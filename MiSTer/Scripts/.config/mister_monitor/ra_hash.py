#!/usr/bin/env python3
# =============================================================================
# ra_hash.py — RetroAchievements ROM hashing for MiSTer Monitor
# =============================================================================
# Computes the MD5 hash of a ROM following the per-console rules used by
# rcheevos' rc_hash (the library inside odelot's Main_MiSTer fork), so the
# result can be resolved to a RetroAchievements game ID via:
#
#     https://retroachievements.org/dorequest.php?r=gameid&m=<md5>
#
# Designed to sit next to mister_status_server.py and be imported from it:
#
#     from ra_hash import compute_ra_hash, is_core_supported
#
# Also runnable standalone for verification against known hashes:
#
#     python3 ra_hash.py NES  "/media/fat/games/NES/Some Game.nes"
#     python3 ra_hash.py SNES "/media/fat/games/SNES/pack.zip" --internal "Some Game.sfc"
#
# Phase 1 scope (cartridge-based systems only):
#   raw MD5 .......... Genesis/MegaDrive, SMS, Game Gear, GB/GBC, GBA,
#                      Atari 2600 (Atari7800 core), Sega 32X
#   header skip ...... NES/FDS (iNES 16 + optional 512 trainer),
#                      SNES (512 copier header), PC Engine (512 header)
#   normalization .... N64 (byteswap v64/n64 to native z64 before hashing)
#
# Out of scope (deferred): CD-based systems (PSX, MegaCD, PCE-CD, NeoGeo CD)
# and NeoGeo cartridge sets. The server falls back to API_GetUserSummary's
# LastGameID for those (see /status/retroachievements design notes).
# =============================================================================

import hashlib
import os
import sys
import time
import zipfile
from contextlib import contextmanager

# --- Tunables ----------------------------------------------------------------

CHUNK_SIZE = 1024 * 1024          # 1MB chunks, same as the server's hasher
YIELD_SLEEP = 0.003               # per-chunk CPU yield to avoid A/V stutter
MAX_SIZE_FOR_RA_HASH = 128 * 1024 * 1024   # largest real cart is 64MB (N64);
                                           # guard against mis-resolved paths

# --- Core name -> hashing method ----------------------------------------------
# Keys are lowercase. Feed this with the *canonical* core name after your
# existing CORE_NAME_MAPPING normalization pass (see the SAM second-hop
# normalization) so aliases collapse before the lookup here.
#
# Verify the exact /tmp/CORENAME strings on real hardware before trusting
# this table — entries marked (?) are educated guesses.

CORE_HASH_METHOD = {
    'nes':          'nes',    # also handles FDS images loaded via the NES core
    'snes':         'snes',
    'genesis':      'raw',
    'megadrive':    'raw',    # (?) alias seen in some builds
    'sms':          'raw',    # Master System / Game Gear share the SMS core
    'gameboy':      'raw',    # GB and GBC
    'gameboy2p':    'raw',    # (?) 2-player variant core
    'gba':          'raw',
    'gba2p':        'raw',    # (?)
    'n64':          'n64',
    'tgfx16':       'pce',    # HuCard only; PCE-CD is out of scope
    'turbografx16': 'pce',    # (?) alias
    'atari7800':    'raw',    # RA 2600 support runs through the 7800 core
    'atari2600':    'raw',    # (?) in case a plain-2600 core name appears
    's32x':         'raw',
}

# Extensions we refuse per method (format needs conversion we don't do yet)
_EXCLUDED_EXTENSIONS = {
    'raw': {'.smd'},   # interleaved Genesis dumps; rc_hash expects .bin/.md/.gen
}


def is_core_supported(core_name):
    """True if this module can produce an RA hash for the given core."""
    return core_name.strip().lower() in CORE_HASH_METHOD


# --- Stream helpers -----------------------------------------------------------

@contextmanager
def _open_rom(path, internal_path=None):
    """
    Yields (readable_stream, uncompressed_size). Supports plain files and
    files inside a ZIP (streamed decompression — zip streams are not
    seekable, so all skipping below is done with read(), never seek()).
    """
    if internal_path:
        with zipfile.ZipFile(path, 'r') as zf:
            info = zf.getinfo(internal_path)
            with zf.open(info, 'r') as stream:
                yield stream, info.file_size
    else:
        size = os.path.getsize(path)
        with open(path, 'rb') as stream:
            yield stream, size


def _read_exact(stream, n):
    """Read exactly n bytes (or fewer at EOF). Zip streams may short-read."""
    parts = []
    remaining = n
    while remaining > 0:
        chunk = stream.read(remaining)
        if not chunk:
            break
        parts.append(chunk)
        remaining -= len(chunk)
    return b''.join(parts)


def _md5_stream(stream, md5=None):
    """MD5 the remainder of a stream in chunks, yielding CPU between chunks."""
    if md5 is None:
        md5 = hashlib.md5()
    while True:
        chunk = stream.read(CHUNK_SIZE)
        if not chunk:
            break
        md5.update(chunk)
        time.sleep(YIELD_SLEEP)
    return md5.hexdigest().upper()


# --- Per-method hashers ---------------------------------------------------------

def _hash_raw(stream, size):
    """Full-file MD5 (Genesis, SMS/GG, GB/GBC, GBA, 2600, 32X)."""
    return _md5_stream(stream), 'raw full-file MD5'


def _hash_nes(stream, size):
    """
    NES / FDS per rc_hash:
      - iNES ("NES\\x1a"): skip 16-byte header; if flags6 bit 2 is set,
        skip an additional 512-byte trainer.
      - FDS  ("FDS\\x1a"): skip 16-byte header.
      - No recognized header: hash the whole file (headerless dump).
    """
    head = _read_exact(stream, 16)

    if len(head) >= 16 and head[:4] == b'NES\x1a':
        note = 'iNES header skipped (16 bytes)'
        if head[6] & 0x04:
            _read_exact(stream, 512)
            note += ' + 512-byte trainer skipped'
        return _md5_stream(stream), note

    if len(head) >= 16 and head[:4] == b'FDS\x1a':
        return _md5_stream(stream), 'FDS header skipped (16 bytes)'

    # Headerless: the 16 bytes already read are part of the ROM data
    md5 = hashlib.md5()
    md5.update(head)
    return _md5_stream(stream, md5), 'no header detected, full-file MD5'


def _hash_snes(stream, size):
    """
    SNES per odelot's documented rule: a 512-byte SMC/SWC copier header is
    present when file_size % 1024 == 512. (rc_hash itself tests against 8KB
    granularity; both rules agree for any real SNES ROM, which is always a
    multiple of 8KB plus the optional 512-byte header.)
    """
    if size % 1024 == 512:
        _read_exact(stream, 512)
        return _md5_stream(stream), 'copier header skipped (512 bytes)'
    return _md5_stream(stream), 'no copier header, full-file MD5'


def _hash_pce(stream, size):
    """
    PC Engine HuCard per rc_hash: a 512-byte header is present when the file
    size exceeds the largest multiple of 128KB by exactly 512 bytes.
    """
    if size % 131072 == 512:
        _read_exact(stream, 512)
        return _md5_stream(stream), 'HuCard header skipped (512 bytes)'
    return _md5_stream(stream), 'no header, full-file MD5'


def _hash_n64(stream, size):
    """
    N64 per rc_hash: normalize the ROM to native big-endian (z64) byte order
    before hashing. Format is detected from the first byte:
      0x80 -> z64 (native)          : no conversion
      0x37 -> v64 (16-bit swapped)  : swap every byte pair
      0x40 -> n64 (32-bit little)   : reverse every 4-byte word
      other -> assume native
    Conversion is done chunk-by-chunk on 4-byte-aligned buffers.
    """
    first = _read_exact(stream, 4)
    if len(first) < 4:
        return None, 'file too small for N64 detection'

    b0 = first[0]
    if b0 == 0x37:
        mode, note = 'v64', 'v64 detected, 16-bit byteswap applied'
    elif b0 == 0x40:
        mode, note = 'n64', 'n64 detected, 32-bit word swap applied'
    elif b0 == 0x80:
        mode, note = 'z64', 'z64 native, no conversion'
    else:
        mode, note = 'z64', 'unknown magic 0x%02X, assuming native order' % b0

    def convert(buf):
        if mode == 'z64':
            return buf
        arr = bytearray(len(buf))
        if mode == 'v64':
            arr[0::2] = buf[1::2]
            arr[1::2] = buf[0::2]
        else:  # n64
            arr[0::4] = buf[3::4]
            arr[1::4] = buf[2::4]
            arr[2::4] = buf[1::4]
            arr[3::4] = buf[0::4]
        return bytes(arr)

    md5 = hashlib.md5()
    md5.update(convert(first))

    # Keep chunks 4-byte aligned: carry any tail remainder into the next read.
    carry = b''
    while True:
        chunk = stream.read(CHUNK_SIZE)
        if not chunk:
            break
        buf = carry + chunk
        aligned_len = (len(buf) // 4) * 4
        md5.update(convert(buf[:aligned_len]))
        carry = buf[aligned_len:]
        time.sleep(YIELD_SLEEP)

    if carry:
        # A real N64 ROM is always a multiple of 4 bytes; hash the tail
        # unconverted rather than dropping data, and flag it.
        md5.update(carry)
        note += ' (WARNING: size not multiple of 4, tail unconverted)'

    return md5.hexdigest().upper(), note


_HASHERS = {
    'raw':  _hash_raw,
    'nes':  _hash_nes,
    'snes': _hash_snes,
    'pce':  _hash_pce,
    'n64':  _hash_n64,
}


# --- Public entry point ---------------------------------------------------------

def compute_ra_hash(core_name, path, internal_path=None):
    """
    Compute the RetroAchievements MD5 for the active ROM.

    Args:
        core_name:     canonical MiSTer core name (after CORE_NAME_MAPPING)
        path:          filesystem path to the ROM file or to a .zip container
        internal_path: name of the ROM inside the ZIP, or None for plain files

    Returns a dict:
        {"hash": "<32 hex chars>" | None,
         "method": "<hashing method used>",
         "note": "<human-readable detail>",
         "error": "<reason>" | None}
    """
    result = {"hash": None, "method": None, "note": None, "error": None}

    key = core_name.strip().lower()
    method = CORE_HASH_METHOD.get(key)
    if method is None:
        result["error"] = f"core '{core_name}' not supported for RA hashing"
        return result
    result["method"] = method

    rom_name = internal_path if internal_path else path
    ext = os.path.splitext(rom_name)[1].lower()
    if ext in _EXCLUDED_EXTENSIONS.get(method, ()):
        result["error"] = f"extension '{ext}' not supported ({key}: needs deinterleaving)"
        return result

    try:
        with _open_rom(path, internal_path) as (stream, size):
            if size == 0:
                result["error"] = "empty file"
                return result
            if size > MAX_SIZE_FOR_RA_HASH:
                result["error"] = f"file too large for RA hash ({size:,} bytes)"
                return result

            start = time.time()
            digest, note = _HASHERS[method](stream, size)
            elapsed = time.time() - start

            result["hash"] = digest
            result["note"] = f"{note} ({size:,} bytes in {elapsed:.2f}s)"
            return result

    except KeyError:
        result["error"] = f"'{internal_path}' not found inside {path}"
    except (OSError, zipfile.BadZipFile) as e:
        result["error"] = f"read error: {e}"
    return result


# --- CLI for hash verification ---------------------------------------------------

if __name__ == '__main__':
    import argparse

    parser = argparse.ArgumentParser(
        description="Compute a RetroAchievements MD5 for a ROM "
                    "(verify against the 'Supported Game Files' list on the "
                    "game's retroachievements.org page).")
    parser.add_argument('core', help="MiSTer core name (e.g. NES, SNES, N64)")
    parser.add_argument('path', help="ROM file or ZIP container")
    parser.add_argument('--internal', default=None,
                        help="filename inside the ZIP")
    args = parser.parse_args()

    r = compute_ra_hash(args.core, args.path, args.internal)
    if r["error"]:
        print(f"ERROR: {r['error']}")
        sys.exit(1)
    print(f"method : {r['method']}")
    print(f"note   : {r['note']}")
    print(f"md5    : {r['hash']}")
    print(f"\nresolve: curl 'https://retroachievements.org/dorequest.php?r=gameid&m={r['hash'].lower()}'")
