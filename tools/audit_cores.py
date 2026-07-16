#!/usr/bin/env python3
"""
audit_cores.py — tell us about MiSTer cores the monitor cannot name yet.

WHY THIS EXISTS
    A core the server cannot map shows its raw name and gets no artwork and no
    achievements. Today we find out when a user reports it (z386 took a forum
    post). This turns that into a scheduled diff.

WHAT IT SCANS
    Every database Update_All knows about — not just the official Distribution.
    Update_All's databases.py is the ecosystem's index (official, jotego,
    Coin-Op, LLAPI, DB9, unofficial, RetroAchievements, ...), so it is fetched
    live rather than hardcoded here: new databases get picked up for free.

WHAT IT CANNOT DO — read this before trusting the output
    The .rbf FILENAME IS NOT THE CORENAME. The core writes its own internal name
    to /tmp/CORENAME, and they diverge: the repo ZX-Spectrum_MISTer ships
    ZX-Spectrum.rbf but writes 'Spectrum'. Our mapping is correctly keyed on
    'Spectrum', and a naive filename diff would flag it as a bug.

    There is no authoritative list of internal names either: the official
    corenames doc was last touched in June 2022 and is missing 47 live cores
    (N64, Saturn, Jaguar, CDi...).

    So: this tool reliably answers "does a core exist that we have never
    triaged?" — it does NOT answer "what key should we add?". That key comes
    from the runtime unknown-core log, which records the literal CORENAME.
    Treat every finding as a lead to confirm, never as a patch to apply.

USAGE
    python3 audit_cores.py --server <path> --ino <path> [--ra <path>]
    python3 audit_cores.py ... --baseline tools/known_cores.json   # CI mode
    python3 audit_cores.py ... --json report.json --official-only

EXIT CODES
    0  nothing new
    1  cores found that are not in the baseline   (CI raises an issue)
    2  usage / fetch error
"""

import argparse
import io
import json
import os
import re
import sys
import time
import urllib.request
import zipfile

UPDATE_ALL_DATABASES = ("https://raw.githubusercontent.com/theypsilon/"
                        "Update_All_MiSTer/master/src/update_all/databases.py")
OFFICIAL_DB = ("https://raw.githubusercontent.com/MiSTer-devel/"
               "Distribution_MiSTer/main/db.json.zip")

# Arcade cores are addressed by .mra, never by CORENAME, so they are out of
# scope — and forks invent their own folder names for them ('_YCArcade'), hence
# a substring test rather than a fixed set. '_Unstable' holds the DB9 fork's
# dev builds: three per core per day, same CORENAME, pure noise here.
def _skip_folder(folder):
    f = (folder or "").lower()
    # '|' prefixes a user-data folder in the downloader DB ('|games/MemTest/'):
    # the .rbf files under it are payloads a core loads, not cores themselves.
    if f.startswith("|") or f == "scripts":
        return True
    return "arcade" in f or f in ("_utility", "_unstable")


# Build variants: the same Verilog recompiled for different hardware (LLAPI
# controllers, DB9 adapters) or a dev snapshot. The core's internal name does
# not change, so folding them into the base core is what keeps the report
# readable — without this, DB9 alone contributes 327 '_unstable' entries for
# cores we already map.
#
# Deliberately NOT stripped: '_2P' (GameBoy2P is its own core with its own
# games folder), '_accuracy', '_80MHz', '2XCPU' — those may well be distinct
# cores, and hiding a real core is a worse failure than one noisy line.
VARIANT_SUFFIXES = ("_llapi", "_db9", "_unstable", "_dualsdram",
                    "yc2", "yc", "userio2", "rgb8")

# Filenames that are not system cores even though they end in .rbf.
SKIP_CORES = {"menu", "MiSTerLaggy", "ADCTest", "InputTest", "MemTest"}

UA = {"User-Agent": "MiSTer-Monitor-core-audit"}


# --------------------------------------------------------------------------- #
# fetching
# --------------------------------------------------------------------------- #

def _get(url, timeout=40):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def list_databases(official_only=False):
    """
    [(db_id, url), ...] for every database Update_All indexes.

    Parsed from its source rather than imported: importing would drag in the
    whole Update_All package. Deprecated entries are named DB_*_DEPRECATED and
    are skipped — they 404 or serve stale data.
    """
    if official_only:
        return [("distribution_mister", OFFICIAL_DB)]

    try:
        src = _get(UPDATE_ALL_DATABASES).decode("utf-8", "replace")
    except Exception as e:
        print(f"!! cannot read Update_All's database index: {e}")
        print("   falling back to the official Distribution only")
        return [("distribution_mister", OFFICIAL_DB)]

    # Database(db_id=X, db_url='...', title='...')
    out, seen = [], set()
    for m in re.finditer(r"Database\(\s*db_id\s*=\s*([^,]+?),\s*"
                         r"db_url\s*=\s*'([^']+)'", src, re.S):
        ident, url = m.group(1).strip(), m.group(2).strip()
        if "DEPRECATED" in ident:
            continue
        if url in seen:
            continue
        seen.add(url)
        # db_id is usually a constant name; resolve it to its literal if we can
        lit = re.search(r"^%s\s*=\s*'([^']+)'" % re.escape(ident), src, re.M)
        out.append((lit.group(1) if lit else ident, url))
    return out


def official_arcade_cores():
    """
    Names of every arcade core the official repo ships.

    Needed because forks scatter arcade cores outside any folder called
    'arcade' — the LLAPI database keeps Pacman and IremM62 in '_LLAPI/'
    alongside console cores — and arcade is resolved by .mra, never by
    CORENAME. Deriving the set from the official repo beats hand-listing it:
    it stays correct as arcade cores are added.
    """
    try:
        raw = _get(OFFICIAL_DB)
        z = zipfile.ZipFile(io.BytesIO(raw))
        db = json.loads(z.read(z.namelist()[0]))
    except Exception as e:
        print(f"!! cannot read the official DB for the arcade list: {e}")
        return set()
    out = set()
    for path in db.get("files", {}):
        if not path.startswith("_Arcade/cores/"):
            continue
        core = re.sub(r"_\d{8}[a-z0-9_]*\.rbf$", "", path.split("/")[-1], flags=re.I)
        out.add(re.sub(r"\.rbf$", "", core, flags=re.I).lower())
    return out


def db_label(url):
    """
    'owner/repo' for a database URL.

    The repo name alone is ambiguous: MiSTer-devel and MiSTer-DB9 both publish
    a repo called Distribution_MiSTer, and labelling by repo made the official
    distribution and the DB9 fork indistinguishable in the report.
    """
    parts = url.split("/")
    return f"{parts[3]}/{parts[4]}" if len(parts) > 4 else url


def cores_from_db(url, arcade=frozenset()):
    """
    ({folder: {core, ...}}, note) for one database.

    Databases are served either zipped or as plain JSON, and both spellings
    appear in the index, so the payload is sniffed rather than trusted.
    """
    raw = _get(url)
    if raw[:2] == b"PK":
        z = zipfile.ZipFile(io.BytesIO(raw))
        db = json.loads(z.read(z.namelist()[0]))
    else:
        db = json.loads(raw.decode("utf-8", "replace"))

    found = {}
    for path in db.get("files", {}):
        if not path.lower().endswith(".rbf"):
            continue
        parts = path.split("/")
        if len(parts) < 2:
            continue                       # menu.rbf at the root
        folder = parts[0]
        if _skip_folder(folder):
            continue
        # Strip the release stamp. Forks append their own tokens after the date
        # ('AcornAtom_20260611_5314b34_DB9.rbf'), so everything from the date
        # onwards goes.
        core = re.sub(r"_\d{8}[a-z0-9_]*\.rbf$", "", parts[-1], flags=re.I)
        core = re.sub(r"\.rbf$", "", core, flags=re.I)
        # Then the variant tokens that sit BEFORE the date
        # ('Atari7800_LLAPI_20250209.rbf' -> 'Atari7800_LLAPI' -> 'Atari7800').
        changed = True
        while changed:
            changed = False
            for suf in VARIANT_SUFFIXES:
                if core.lower().endswith(suf):
                    core = core[: -len(suf)]
                    changed = True
        if not core or core in SKIP_CORES or core.lower() in arcade:
            continue
        found.setdefault(folder, set()).add(core)

    return found, db.get("db_id", "?")


# --------------------------------------------------------------------------- #
# our own tables
# --------------------------------------------------------------------------- #

def parse_server_mapping(path):
    """
    {raw_core_lower: friendly} from CORE_NAME_MAPPING.

    Parsed, not imported: the module reads /media/fat at import time and cannot
    run on a CI box.
    """
    src = io.open(path, encoding="utf-8").read()
    m = re.search(r"^CORE_NAME_MAPPING\s*=\s*\{(.*?)^\}", src, re.S | re.M)
    if not m:
        raise SystemExit(f"CORE_NAME_MAPPING not found in {path}")
    pairs = re.findall(r"""['"]([^'"]+)['"]\s*:\s*['"]([^'"]+)['"]""", m.group(1))
    return {k.lower(): v for k, v in pairs}


def parse_ino_ss_ids(path):
    """Friendly names getScreenScraperSystemId() can turn into a system id."""
    src = io.open(path, encoding="utf-8").read()
    m = re.search(r"String getScreenScraperSystemId\(String coreName\)\s*\{(.*?)\n\}",
                  src, re.S)
    if not m:
        raise SystemExit(f"getScreenScraperSystemId not found in {path}")
    body = m.group(1)
    names = re.findall(r'core\s*==\s*"([^"]+)"', body)
    names += re.findall(r'core\.indexOf\("([^"]+)"\)', body)
    return {n.lower() for n in names}


def parse_ra_needles(path):
    if not path or not os.path.isfile(path):
        return []
    src = io.open(path, encoding="utf-8").read()
    m = re.search(r"^_FRIENDLY_RULES\s*=\s*\[(.*?)^\]", src, re.S | re.M)
    return re.findall(r'"([a-z0-9]+)"', m.group(1)) if m else []


def ra_covered(friendly, needles):
    n = re.sub(r"[^a-z0-9]", "", (friendly or "").lower())
    return any(x in n for x in needles)


# --------------------------------------------------------------------------- #
# main
# --------------------------------------------------------------------------- #

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--ino", required=True)
    ap.add_argument("--ra")
    ap.add_argument("--baseline", help="JSON of already-triaged cores")
    ap.add_argument("--json", help="write the full report here")
    ap.add_argument("--official-only", action="store_true",
                    help="skip the ecosystem, scan the official Distribution")
    ap.add_argument("--update-baseline", action="store_true",
                    help="rewrite --baseline with everything seen now")
    args = ap.parse_args()

    server_map = parse_server_mapping(args.server)
    ss_names = parse_ino_ss_ids(args.ino)
    ra_needles = parse_ra_needles(args.ra)

    arcade = official_arcade_cores()
    dbs = list_databases(args.official_only)
    print(f"scanning {len(dbs)} databases "
          f"({len(arcade)} arcade cores excluded)\n")

    all_cores = {}            # core -> {"dbs": [...], "folder": str}
    unreachable = []
    for db_id, url in dbs:
        label = db_label(url)
        try:
            found, real_id = cores_from_db(url, arcade)
        except Exception as e:
            unreachable.append((label, type(e).__name__))
            continue
        total = sum(len(v) for v in found.values())
        if not total:
            continue          # wallpapers, cheats, manuals: no cores, not news
        print(f"  {total:>4} cores  {label}")
        for folder, cores in found.items():
            for c in cores:
                e = all_cores.setdefault(c, {"dbs": [], "folder": folder})
                e["dbs"].append(label)

    if unreachable:
        print(f"\n  {len(unreachable)} database(s) unreachable (reported, not fatal):")
        for lbl, err in unreachable:
            print(f"       {lbl:<44} {err}")

    print(f"\n{len(all_cores)} distinct system cores across the ecosystem")

    baseline = set()
    if args.baseline and os.path.isfile(args.baseline):
        with io.open(args.baseline, encoding="utf-8") as f:
            baseline = {c.lower() for c in json.load(f).get("triaged", [])}
        print(f"{len(baseline)} already triaged (baseline)")

    rows = []
    for core, info in sorted(all_cores.items()):
        friendly = server_map.get(core.lower())
        rows.append({
            "core": core,
            "folder": info["folder"],
            "dbs": sorted(set(info["dbs"])),
            "friendly": friendly,
            "screenscraper": bool(friendly and friendly.lower() in ss_names),
            "retroachievements": bool(friendly and ra_covered(friendly, ra_needles)),
            "triaged": core.lower() in baseline,
        })

    new = [r for r in rows if not r["triaged"]]
    unmapped = [r for r in rows if not r["friendly"]]

    if new:
        print(f"\nNOT YET TRIAGED ({len(new)})")
        print("  The .rbf name is a LEAD, not the CORENAME — confirm each one")
        print("  against /status/unknown_cores or the core itself before mapping.\n")
        for r in new:
            where = ", ".join(r["dbs"][:2]) + ("..." if len(r["dbs"]) > 2 else "")
            state = (f"-> {r['friendly']}" if r["friendly"] else "NO FRIENDLY NAME")
            print(f"    {r['core']:<24} {state:<28} [{where}]")

    print(f"\nsummary: {len(rows)} cores | {len(unmapped)} without a friendly name "
          f"| {len(new)} not triaged")

    if args.json:
        with io.open(args.json, "w", encoding="utf-8") as f:
            json.dump({"generated": int(time.time()),
                       "databases_scanned": len(dbs),
                       "cores": rows}, f, indent=2)
        print(f"report -> {args.json}")

    if args.update_baseline and args.baseline:
        with io.open(args.baseline, "w", encoding="utf-8") as f:
            json.dump({"comment": "Cores already looked at. Presence here means "
                                  "'we have seen it and decided', NOT 'supported'.",
                       "updated": time.strftime("%Y-%m-%d"),
                       "triaged": sorted(r["core"] for r in rows)}, f, indent=2)
        print(f"baseline -> {args.baseline} ({len(rows)} cores)")
        return 0

    return 1 if new else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(2)
