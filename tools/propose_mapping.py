#!/usr/bin/env python3
"""
propose_mapping.py — propose CORE_NAME_MAPPING entries for official cores we
do not name yet, using each core's own source as the authority.

WHY THIS WORKS
    The key CORE_NAME_MAPPING needs is the string the core writes to
    /tmp/CORENAME, and that string is the first field of CONF_STR in the core's
    Verilog:

        localparam CONF_STR = {
            "Spectrum;;",          <- ZX-Spectrum.rbf announces itself as this
            ...

    Measured over the 127 official non-arcade cores: 93 extract cleanly, and of
    the 58 that could be checked against the existing table, 58 matched. Zero
    contradictions. It gets right precisely the cases a human gets wrong:

        AdventureVision.rbf -> 'AVision'      CDi.rbf     -> 'CD-i'
        ColecoVision.rbf    -> 'Coleco'       GnW.rbf     -> 'GameNWatch'
        Specialist.rbf      -> 'SPMX'         ZX-Spectrum -> 'Spectrum'

WHY IT PROPOSES INSTEAD OF COMMITTING
    1. A wrong key never fires, so it looks handled while being dead. That is
       worse than an obvious gap, and the failure mode is real: PCXT.sv and
       Tandy1000.sv are ONE source whose CONF_STR is a compile-time ternary, so
       a naive read gives PCXT the name 'Tandy1000'. Those are detected and
       deliberately NOT proposed.
    2. The friendly name is editorial. 'Spectrum' -> 'ZX Spectrum' is guessable;
       'GameNWatch' -> 'Nintendo Game & Watch' is not, and 'SPMX' -> 'Specialist
       MX' is not. The bot fills the placeholder; a human writes the name.

    So: the bot does the part that errs, and leaves the part only you can do.

SCOPE
    Official MiSTer-devel cores only. Unofficial cores reach us through
    scan_github.py and the runtime log at /status/unknown_cores.

USAGE
    GITHUB_TOKEN=... python3 propose_mapping.py \
        --server MiSTer/Scripts/.config/mister_monitor/mister_status_server.py \
        --out /tmp/proposal.json [--apply]

EXIT CODES
    0  nothing to propose      1  proposals written      2  fetch error
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
from concurrent.futures import ThreadPoolExecutor

OFFICIAL_DB = ("https://raw.githubusercontent.com/MiSTer-devel/"
               "Distribution_MiSTer/main/db.json.zip")
RAW = "https://raw.githubusercontent.com/MiSTer-devel"
ORG_REPOS = "https://api.github.com/orgs/MiSTer-devel/repos"

# The convention is spelled three ways in the wild — MiSTer-devel itself ships
# ZX-Spectrum_MISTer — so matching is case-insensitive or real cores are missed.
REPO_SUFFIXES = ("_MiSTer", "_MISTer", "_Mister")
BRANCHES = ("master", "main")
SKIP_FOLDERS = {"_Arcade", "_Utility"}
SKIP_CORES = {"menu", "MiSTerLaggy"}


def _norm(s):
    """'TI-99_4A' and 'Ti994a' are the same repo; the DB and GitHub disagree."""
    return re.sub(r"[^a-z0-9]", "", (s or "").lower())


def _get(url, token=None, timeout=20):
    headers = {"User-Agent": "MiSTer-Monitor-mapping-bot"}
    if token and "api.github.com" in url:
        headers["Authorization"] = f"Bearer {token}"
        headers["Accept"] = "application/vnd.github+json"
    try:
        with urllib.request.urlopen(
                urllib.request.Request(url, headers=headers), timeout=timeout) as r:
            return r.read()
    except Exception:
        return None


# --------------------------------------------------------------------------- #
# inputs
# --------------------------------------------------------------------------- #

def official_cores():
    raw = _get(OFFICIAL_DB)
    if not raw:
        raise SystemExit("cannot reach the official Distribution DB")
    z = zipfile.ZipFile(io.BytesIO(raw))
    db = json.loads(z.read(z.namelist()[0]))
    out = set()
    for path in db.get("files", {}):
        if not path.lower().endswith(".rbf"):
            continue
        parts = path.split("/")
        if len(parts) < 2 or parts[0] in SKIP_FOLDERS or parts[0].startswith("|"):
            continue
        core = re.sub(r"_\d{8}[a-z0-9_]*\.rbf$", "", parts[-1], flags=re.I)
        core = re.sub(r"\.rbf$", "", core, flags=re.I)
        if core and core not in SKIP_CORES:
            out.add(core)
    return sorted(out)


def existing_keys(server_path):
    src = io.open(server_path, encoding="utf-8").read()
    m = re.search(r"^CORE_NAME_MAPPING\s*=\s*\{(.*?)^\}", src, re.S | re.M)
    if not m:
        raise SystemExit(f"CORE_NAME_MAPPING not found in {server_path}")
    pairs = re.findall(r"""['"]([^'"]+)['"]\s*:\s*['"]([^'"]+)['"]""", m.group(1))
    return {k.lower() for k, _ in pairs}


def devel_repos(token):
    """
    All MiSTer-devel repo names.

    Needed because the repo name is not derivable from the .rbf: Ti994a.rbf
    lives in TI-99_4A_MiSTer. Guessing '<rbf>_MiSTer' resolves only 96 of 127
    cores — the misses are the pre-2018 repos (C64, Minimig, NeoGeo, Ti994a)
    whose names predate the convention. Listing and normalising recovers them.
    """
    out = []
    for page in range(1, 6):
        data = _get(f"{ORG_REPOS}?per_page=100&page={page}&type=public", token)
        if not data:
            break
        chunk = json.loads(data)
        if not chunk:
            break
        out += [(r["name"], r.get("default_branch") or "master") for r in chunk]
        if len(chunk) < 100:
            break
        time.sleep(1)
    return out


# --------------------------------------------------------------------------- #
# the extraction
# --------------------------------------------------------------------------- #

def extract_conf_str(body):
    """
    (name, flag). flag is None on a clean read, otherwise why it is unusable.

    The ternary check is not a nicety. PCXT.sv reads:
        `define CONF_STR_SYSTEM (`SYSTEM_VARIANT_TANDY ? "Tandy1000;..." : "PCXT;...")
    One source, two cores, name chosen at build time. Reading the first literal
    gives PCXT the name 'Tandy1000' — a mapping that would never fire while
    looking correct. Refusing to guess is the entire point.
    """
    if not body:
        return None, "source not fetched"
    i = body.find("CONF_STR")
    if i < 0:
        return None, "no CONF_STR in source"
    seg = body[i:i + 400]
    if re.search(r"\?\s*\n?\s*\"", seg[:220]):
        return None, "CONF_STR is build-conditional (one source, several cores)"
    m = re.search(r"CONF_STR[^\"]{0,80}?\"([^;\"]+)", seg, re.S)
    if not m:
        return None, "CONF_STR present but unparsed"
    name = m.group(1).strip()
    return (name, None) if name else (None, "empty CONF_STR field")


def resolve(core, repo_index, token):
    """(core, corename, repo, branch, sv, flag)"""
    # 1. the repo. Convention first, then the normalised index for legacy names.
    candidates = [(f"{core}{s}", b) for s in REPO_SUFFIXES for b in BRANCHES]
    hit = repo_index.get(_norm(core))
    if hit:
        candidates.insert(0, hit)

    for repo, branch in candidates:
        # 2. the source. Named after the .rbf, not the repo (TI-99_4A_MiSTer
        #    ships Ti994a.sv), so the core name is the right guess here.
        body = _get(f"{RAW}/{repo}/{branch}/{core}.sv")
        if body is None:
            continue
        name, flag = extract_conf_str(body.decode("utf-8", "replace"))
        return core, name, repo, branch, f"{core}.sv", flag
    return core, None, None, None, None, "no repo/.sv found by convention"


# --------------------------------------------------------------------------- #
# output
# --------------------------------------------------------------------------- #

def apply_to_server(server_path, proposals):
    """
    Append entries at the end of the CORE_NAME_MAPPING block.

    Anchored on the block's own closing brace rather than on any neighbouring
    entry, so it survives the table being reordered.
    """
    src = io.open(server_path, encoding="utf-8").read()
    m = re.search(r"^(CORE_NAME_MAPPING\s*=\s*\{)(.*?)(^\})", src, re.S | re.M)
    if not m:
        raise SystemExit("CORE_NAME_MAPPING block not found")

    lines = ["", "    # --- Proposed automatically from each core's CONF_STR.",
             "    # The KEY is verbatim from the core's Verilog and is correct.",
             "    # The VALUE is a placeholder: replace it with a display name",
             "    # before merging ('C16' -> 'Commodore 16').", ]
    for p in sorted(proposals, key=lambda x: x["corename"].lower()):
        lines.append(f"    '{p['corename']}': '{p['corename']}',"
                     f"  # {p['rbf']}.rbf, {p['repo']}")
    block = m.group(2).rstrip("\n") + "\n" + "\n".join(lines) + "\n"
    out = src[:m.start(2)] + block + src[m.end(2):]
    io.open(server_path, "w", encoding="utf-8").write(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--out", default="/tmp/proposal.json")
    ap.add_argument("--apply", action="store_true",
                    help="write the proposed entries into --server")
    args = ap.parse_args()

    token = os.environ.get("GITHUB_TOKEN", "")
    cores = official_cores()
    known = existing_keys(args.server)
    print(f"{len(cores)} official non-arcade cores | {len(known)} keys already mapped\n")

    repo_index = {}
    for name, branch in devel_repos(token):
        for suf in REPO_SUFFIXES:
            if name.lower().endswith(suf.lower()):
                repo_index[_norm(name[: -len(suf)])] = (name, branch)
                break
    print(f"MiSTer-devel repos indexed: {len(repo_index)}"
          f"{'' if token else '  (no GITHUB_TOKEN: expect fewer)'}\n")

    with ThreadPoolExecutor(max_workers=8) as ex:
        results = list(ex.map(lambda c: resolve(c, repo_index, token), cores))

    proposals, review, already = [], [], 0
    for core, name, repo, branch, sv, flag in results:
        if flag or not name:
            review.append({"rbf": core, "reason": flag or "unknown"})
            continue
        if name.lower() in known:
            already += 1
            continue
        proposals.append({
            "rbf": core, "corename": name, "repo": repo,
            "source": f"https://github.com/MiSTer-devel/{repo}/blob/{branch}/{sv}",
            "diverges": core.lower() != name.lower(),
        })

    print(f"already mapped : {already}")
    print(f"to propose     : {len(proposals)}")
    print(f"needs a human  : {len(review)}\n")

    if proposals:
        print("PROPOSED (key from CONF_STR, value is a placeholder)\n")
        for p in sorted(proposals, key=lambda x: x["corename"].lower()):
            mark = "  <- differs from the .rbf name" if p["diverges"] else ""
            print(f"    '{p['corename']}': '{p['corename']}',"
                  f"   # {p['rbf']}.rbf{mark}")
        print()

    if review:
        print("NOT PROPOSED — the bot refuses to guess these\n")
        for r in sorted(review, key=lambda x: x["rbf"].lower()):
            print(f"    {r['rbf']:<22} {r['reason']}")
        print()

    with io.open(args.out, "w", encoding="utf-8") as f:
        json.dump({"generated": int(time.time()),
                   "proposals": proposals, "review": review}, f, indent=2)
    print(f"report -> {args.out}")

    if args.apply and proposals:
        apply_to_server(args.server, proposals)
        print(f"applied {len(proposals)} entries -> {args.server}")

    return 1 if proposals else 0


if __name__ == "__main__":
    sys.exit(main())
