#!/usr/bin/env python3
"""
scan_github.py — find MiSTer work that lives in no database.

WHY THIS EXISTS
    audit_cores.py scans the 92 databases Update_All indexes, and misterzine
    tracks three (official, JTcores, Coin-Op). Neither sees z386: it is
    installed by hand from its author's repo and is in no database at all.
    Verified — nand2mario/z386_MiSTer was created 2026-04-26 and reached us as
    a user report months later.

    GitHub does see it. This turns that into a weekly check.

HOW IT DISCRIMINATES
    'MiSTer in:name' alone returns 21,560 repos: 'mister' is an ordinary word
    ('mister-midpines/star-wars'). The usable signal is the naming convention
    <Core>_MiSTer, which measured 23/23 genuine on a live sample. Everything
    else falls back to a description test.

WHAT IT CANNOT DO
    Same limit as always: this yields a REPO, not a CORENAME. 'z386_MiSTer'
    does not tell you whether the core writes 'Z386', 'z386' or something else
    to /tmp/CORENAME. It changes the order of work — you stop discovering cores
    from bug reports and start asking for the CORENAME already knowing what to
    ask about. /status/unknown_cores remains the only source of the literal key.

USAGE
    GITHUB_TOKEN=... python3 scan_github.py --days 8 --baseline tools/known_repos.json
    (unauthenticated works but rate-limits at 10 searches/min)

EXIT CODES
    0  nothing new      1  new repos found      2  search unavailable
"""

import argparse
import io
import json
import os
import re
import sys
import time
import urllib.parse
import urllib.request

API = "https://api.github.com/search/repositories"

# The <Core>_MiSTer convention. Case varies in the wild — MiSTer-devel itself
# ships 'ZX-Spectrum_MISTer', and vinej publishes 'X16_Mister' — so the test is
# case-insensitive or it silently drops real cores.
CORE_NAME_RE = re.compile(r"_mister$", re.I)

# Owners whose output already arrives through audit_cores.py's database scan.
# Listing them here is not a value judgement: it keeps the actionable bucket to
# things no other tool would tell us about.
COVERED_OWNERS = {
    "mister-devel", "mister-db9", "mister-llapi", "mister-unstable-nightlies",
    "jotego", "coin-opcollection", "theypsilon", "atrac17", "mikes11",
    "zaparooproject", "misterfpga",
}

# Arcade is addressed by .mra and never by CORENAME, so an arcade repo is not a
# core we could ever need to map.
ARCADE_NAME_RE = re.compile(r"^arcade[-_]", re.I)

# Deliberately no \b before 'fpga': the community writes 'MisterFPGA' as one
# word, and a word boundary drops those (observed: 'Maldita Castilla ported for
# MisterFPGA').
NEWSY_RE = re.compile(r"(fpga|de10|de-10|\bcore\b|\bmra\b|\brbf\b|arcade|"
                      r"retro|sidi|mistex|analogue)", re.I)


def search(query, token, pages=3):
    """Repos matching `query`, newest first. Stops at `pages` x 100."""
    out = []
    for page in range(1, pages + 1):
        url = f"{API}?q={urllib.parse.quote(query)}&sort=updated&order=desc&per_page=100&page={page}"
        req = urllib.request.Request(url, headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "MiSTer-Monitor-ecosystem-scan",
            **({"Authorization": f"Bearer {token}"} if token else {}),
        })
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                data = json.loads(r.read())
        except Exception as e:
            # 403 here is almost always the rate limit: 10 searches/min without
            # a token, 30 with one. Report what we have rather than crash.
            print(f"!! search failed on page {page}: {e}")
            break
        items = data.get("items", [])
        out.extend(items)
        if len(items) < 100:
            break
        time.sleep(2)          # stay under the secondary rate limit
    return out


def classify(repo):
    """'core' | 'news' | None (noise)."""
    name = repo.get("name") or ""
    owner = (repo.get("owner") or {}).get("login", "")
    desc = repo.get("description") or ""

    if CORE_NAME_RE.search(name):
        if owner.lower() in COVERED_OWNERS:
            return "news"          # real, but the DB scan already covers it
        if ARCADE_NAME_RE.match(name):
            return "news"          # .mra territory, not a CORENAME
        return "core"

    # No convention: fall back to what it says about itself.
    if "mister" in (name + " " + desc).lower() and NEWSY_RE.search(desc):
        return "news"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--days", type=int, default=8,
                    help="how far back to look; a weekly cron wants overlap")
    ap.add_argument("--baseline", help="JSON of repos already reported")
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--json", help="write the full result here")
    args = ap.parse_args()

    token = os.environ.get("GITHUB_TOKEN", "")
    print(f"searching GitHub {'with' if token else 'WITHOUT'} a token "
          f"({'30' if token else '10'} searches/min)\n")

    since = time.strftime("%Y-%m-%d", time.gmtime(time.time() - args.days * 86400))
    repos = search(f"MiSTer in:name,description fork:false created:>{since}", token)
    if not repos:
        print("no results (search unavailable, or genuinely nothing new)")
        return 2

    seen = set()
    if args.baseline and os.path.isfile(args.baseline):
        with io.open(args.baseline, encoding="utf-8") as f:
            seen = {r.lower() for r in json.load(f).get("reported", [])}

    cores, news = [], []
    for r in repos:
        kind = classify(r)
        if not kind:
            continue
        row = {
            "full_name": r["full_name"],
            "url": r["html_url"],
            "description": (r.get("description") or "").strip(),
            "created": (r.get("created_at") or "")[:10],
            "stars": r.get("stargazers_count", 0),
        }
        (cores if kind == "core" else news).append(row)

    fresh_cores = [c for c in cores if c["full_name"].lower() not in seen]
    fresh_news = [n for n in news if n["full_name"].lower() not in seen]

    print(f"created since {since}: {len(repos)} repos -> "
          f"{len(cores)} core-shaped, {len(news)} ecosystem, "
          f"{len(repos) - len(cores) - len(news)} noise")
    print(f"not reported before: {len(fresh_cores)} cores, {len(fresh_news)} news\n")

    if fresh_cores:
        print(f"## Unofficial core candidates ({len(fresh_cores)})\n")
        for c in sorted(fresh_cores, key=lambda x: -x["stars"]):
            print(f"  {c['full_name']:<44} {c['stars']:>4}* {c['created']}")
            if c["description"]:
                print(f"      {c['description'][:88]}")
        print()

    if fresh_news:
        print(f"## Ecosystem activity ({len(fresh_news)})\n")
        for n in sorted(fresh_news, key=lambda x: -x["stars"]):
            print(f"  {n['full_name']:<44} {n['stars']:>4}* {n['created']}")
            if n["description"]:
                print(f"      {n['description'][:88]}")
        print()

    if args.json:
        with io.open(args.json, "w", encoding="utf-8") as f:
            json.dump({"generated": int(time.time()), "since": since,
                       "cores": fresh_cores, "news": fresh_news}, f, indent=2)

    if args.update_baseline and args.baseline:
        allseen = sorted(seen | {c["full_name"].lower() for c in cores + news})
        with io.open(args.baseline, "w", encoding="utf-8") as f:
            json.dump({"comment": "Repos already surfaced by scan_github.py. "
                                  "Presence here means 'reported once', not "
                                  "'relevant'.",
                       "updated": time.strftime("%Y-%m-%d"),
                       "reported": allseen}, f, indent=1)
        print(f"baseline -> {args.baseline} ({len(allseen)} repos)")
        return 0

    return 1 if (fresh_cores or fresh_news) else 0


if __name__ == "__main__":
    sys.exit(main())
