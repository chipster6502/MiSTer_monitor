#!/usr/bin/env python3
"""
backfill_github.py — one-shot historical sweep of the MiSTer ecosystem.

WHY THIS EXISTS
    scan_github.py only looks forward (created:>since); it will never see the
    hundreds of tools, boards, cases and cores published since ~2017. The
    observatory's catalog needs that history once. This script collects it,
    classifies it with the shared taxonomy, and writes the same JSON shapes
    the weekly scan will keep appending to. Run it once (or a few times);
    it is idempotent, so re-running only adds what is missing.

HOW IT BEATS THE 1,000-RESULT CAP
    GitHub Search caps every query at 1,000 retrievable results, and
    'MiSTer in:name' alone matches ~21,500 repos. Each base query is
    therefore windowed by creation date: probe total_count, and while a
    window holds more than 1,000 repos, bisect it. Day-sized windows that
    still exceed the cap (never observed for these queries) fall back to
    the first 1,000 sorted by stars, with a warning.

WHAT COMES OUT
    --catalog   entries classified with HIGH confidence (catalog-grade)
    --pending   LOW-confidence entries, each carrying 'confidence' and
                'reasons', for human review. Approving one = moving the
                object into the catalog verbatim; deleting one = rejecting
                (the baseline remembers it was seen, it will not return).
    --excluded  TSV of everything dropped as noise, so the filter itself
                can be audited. Not meant to be committed.
    Nothing this script writes is published without you: you review, you
    commit. The page only ever serves what is in the repo.

USAGE
    GITHUB_TOKEN=... python3 tools/backfill_github.py \
        --catalog docs/ecosystem/data/catalog.json \
        --pending tools/pending_repos.json \
        --update-baseline tools/known_repos.json
    (unauthenticated works but crawls at 10 searches/min instead of 30)

EXIT CODES
    0  completed      2  aborted (rate limit wall, --max-requests, network)
"""

import argparse
import datetime as dt
import io
import json
import os
import re
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

import ecosystem_taxonomy as tax

API = "https://api.github.com/search/repositories"

# Recall comes from the union of angles, precision from the classifier.
# fork:false everywhere: a catalog of forks of Main_MiSTer helps nobody.
BASE_QUERIES = [
    ("topic:mister-fpga fork:false",              "topic mister-fpga"),
    ("topic:misterfpga fork:false",               "topic misterfpga"),
    ("MiSTer in:name fork:false",                 "name contains mister"),
    ('"mister fpga" in:description fork:false',   "description phrase"),
    ("misterfpga in:description fork:false",      "description one-word"),
]
README_QUERY = ('"mister fpga" in:readme fork:false', "readme phrase")


class Budget:
    """Counts requests and enforces --max-requests as a hard stop."""
    def __init__(self, limit):
        self.limit = limit
        self.used = 0

    def spend(self):
        self.used += 1
        if self.limit and self.used > self.limit:
            raise RuntimeError(f"--max-requests {self.limit} reached")


def gh_get(url, token, budget, pace):
    """One API call: paced, rate-limit aware, retries on 403 once the
    window resets. Raises on anything that is not recoverable."""
    for attempt in range(4):
        budget.spend()
        req = urllib.request.Request(url, headers={
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "MiSTer-Monitor-ecosystem-backfill",
            **({"Authorization": f"Bearer {token}"} if token else {}),
        })
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                data = json.loads(r.read())
                remaining = int(r.headers.get("X-RateLimit-Remaining", "1"))
                reset = int(r.headers.get("X-RateLimit-Reset", "0"))
            if remaining == 0 and reset:
                wait = max(0, reset - time.time()) + 2
                print(f"    .. search quota exhausted, sleeping {wait:.0f}s",
                      flush=True)
                time.sleep(wait)
            else:
                time.sleep(pace)
            return data
        except urllib.error.HTTPError as e:
            if e.code == 422:
                # Page beyond the 1,000-result cap: not an error, a wall.
                return {"items": [], "total_count": 0, "_capped": True}
            if e.code == 403 and attempt < 3:
                retry = e.headers.get("Retry-After")
                reset = e.headers.get("X-RateLimit-Reset")
                wait = int(retry) if retry else \
                    max(0, int(reset or 0) - time.time()) + 2 if reset else 65
                print(f"    .. 403 (rate limit), sleeping {wait:.0f}s",
                      flush=True)
                time.sleep(wait)
                continue
            raise
    raise RuntimeError("gave up after repeated 403s")


def total_count(base_q, lo, hi, token, budget, pace):
    q = urllib.parse.quote(f"{base_q} created:{lo}..{hi}")
    data = gh_get(f"{API}?q={q}&per_page=1", token, budget, pace)
    return int(data.get("total_count", 0))


def fetch_window(base_q, lo, hi, token, budget, pace):
    """All repos of one window, paged. sort=stars so that if a window is
    somehow capped anyway, what survives is the most relevant slice."""
    out = []
    q = urllib.parse.quote(f"{base_q} created:{lo}..{hi}")
    for page in range(1, 11):                      # 10 x 100 = the hard cap
        url = (f"{API}?q={q}&sort=stars&order=desc&per_page=100&page={page}")
        data = gh_get(url, token, budget, pace)
        items = data.get("items", [])
        out.extend(items)
        if len(items) < 100:
            break
    return out


def midpoint(lo, hi):
    a = dt.date.fromisoformat(lo)
    b = dt.date.fromisoformat(hi)
    return (a + (b - a) // 2).isoformat()


def sweep(base_q, label, since, until, token, budget, pace):
    """Adaptive date windows: bisect while a window exceeds the cap."""
    got = []
    stack = [(since, until)]
    while stack:
        lo, hi = stack.pop()
        n = total_count(base_q, lo, hi, token, budget, pace)
        if n == 0:
            continue
        if n > 1000 and lo != hi:
            mid = midpoint(lo, hi)
            nxt = (dt.date.fromisoformat(mid) +
                   dt.timedelta(days=1)).isoformat()
            # A midpoint equal to lo means a 2-day window; split as 1+1.
            stack.append((min(nxt, hi), hi))
            stack.append((lo, mid))
            continue
        if n > 1000:
            print(f"  !! {label}: {lo} alone holds {n} repos; "
                  f"keeping top 1000 by stars", flush=True)
        print(f"  {label}: {lo}..{hi} -> {n}", flush=True)
        got.extend(fetch_window(base_q, lo, hi, token, budget, pace))
    return got


def load_json(path, default):
    if path and os.path.isfile(path):
        with io.open(path, encoding="utf-8") as f:
            return json.load(f)
    return default


def write_json(path, payload):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with io.open(path, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2, ensure_ascii=False)
        f.write("\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--since", default="2015-01-01",
                    help="creation-date floor (MiSTer itself starts in 2017; "
                         "the margin is free)")
    ap.add_argument("--until", default=dt.date.today().isoformat())
    ap.add_argument("--catalog", default="docs/ecosystem/data/catalog.json")
    ap.add_argument("--pending", default="tools/pending_repos.json")
    ap.add_argument("--excluded",
                    default=os.path.join(tempfile.gettempdir(),
                                         "backfill_excluded.tsv"),
                    help="audit trail of dropped noise; not for committing. "
                         "Defaults to the platform temp dir (so it resolves "
                         "correctly on Windows, where a literal /tmp does not "
                         "exist)")
    ap.add_argument("--update-baseline", metavar="KNOWN_REPOS_JSON",
                    help="also record every processed repo in "
                         "known_repos.json so the weekly scan never "
                         "re-announces what the backfill already saw")
    ap.add_argument("--include-readme", action="store_true",
                    help="adds the in:readme query: extra recall, "
                         "noticeably more noise and requests")
    ap.add_argument("--max-requests", type=int, default=0,
                    help="hard stop as a safety valve (0 = unlimited)")
    args = ap.parse_args()

    token = os.environ.get("GITHUB_TOKEN", "")
    pace = 2.2 if token else 6.5     # 30/min with token, 10/min without
    budget = Budget(args.max_requests)
    print(f"searching GitHub {'with' if token else 'WITHOUT'} a token "
          f"({'30' if token else '10'} searches/min), "
          f"created:{args.since}..{args.until}\n", flush=True)

    catalog = load_json(args.catalog,
                        {"generated": "", "count": 0, "entries": []})
    pending = load_json(args.pending, {"generated": "", "entries": []})

    # Idempotency: what is already cataloged or queued is never touched.
    # known_repos.json's 'reported' is deliberately NOT a skip list here —
    # it means 'mentioned once in the weekly issue', which is a different
    # artifact from 'present in the catalog'.
    seen = {e["id"] for e in catalog["entries"]}
    seen |= {e["id"] for e in pending["entries"]}

    queries = list(BASE_QUERIES)
    if args.include_readme:
        queries.append(README_QUERY)

    today = dt.date.today().isoformat()
    stats = {"dup": 0, "noise": 0, "high": 0, "low": 0}
    processed = set()
    excluded_rows = []
    t0 = time.time()

    try:
        for base_q, label in queries:
            print(f"## {label}", flush=True)
            for repo in sweep(base_q, label, args.since, args.until,
                              token, budget, pace):
                rid = repo["full_name"].lower()
                if rid in seen or rid in processed:
                    stats["dup"] += 1
                    continue
                processed.add(rid)
                category, confidence, reasons = tax.classify(repo)
                if category is None:
                    stats["noise"] += 1
                    excluded_rows.append(
                        f"{rid}\t{(repo.get('description') or '')[:80]}")
                    continue
                entry = tax.make_entry(repo, category, "backfill", today)
                if confidence == "high":
                    catalog["entries"].append(entry)
                    stats["high"] += 1
                else:
                    entry["confidence"] = confidence
                    entry["reasons"] = reasons[:6]
                    pending["entries"].append(entry)
                    stats["low"] += 1
            print(flush=True)
    except (RuntimeError, urllib.error.URLError, OSError) as e:
        print(f"\n!! aborted: {e}\n   partial results are still written; "
              f"re-running resumes where this left off (idempotent).",
              flush=True)
        rc = 2
    else:
        rc = 0

    # Stable order -> stable diffs. The page sorts however it likes at render.
    catalog["entries"].sort(key=lambda e: (e["category"], e["id"]))
    catalog["generated"] = today
    catalog["count"] = len(catalog["entries"])
    write_json(args.catalog, catalog)

    pending["entries"].sort(key=lambda e: (e["category"], e["id"]))
    pending["generated"] = today
    write_json(args.pending, pending)

    if excluded_rows:
        # Disposable audit trail: its write must never sink a run whose real
        # outputs (catalog, pending) are already on disk. makedirs mirrors
        # write_json; the guard turns any remaining path or permission problem
        # into a warning instead of the traceback that ended the first Windows
        # run here.
        try:
            os.makedirs(os.path.dirname(args.excluded) or ".", exist_ok=True)
            with io.open(args.excluded, "w", encoding="utf-8") as f:
                f.write("\n".join(excluded_rows) + "\n")
        except OSError as e:
            print(f"   (skipped excluded audit {args.excluded}: {e})")

    if args.update_baseline and rc == 0:
        base = load_json(args.update_baseline, {"reported": []})
        allseen = sorted({r.lower() for r in base.get("reported", [])} |
                         processed | seen)
        with io.open(args.update_baseline, "w", encoding="utf-8") as f:
            json.dump({"comment": "Repos already surfaced by scan_github.py. "
                                  "Presence here means 'reported once', not "
                                  "'relevant'.",
                       "updated": today,
                       "reported": allseen}, f, indent=1)
            f.write("\n")
        print(f"baseline -> {args.update_baseline} ({len(allseen)} repos)")

    dtme = time.time() - t0
    hist = {}
    for e in catalog["entries"] + pending["entries"]:
        hist[e["category"]] = hist.get(e["category"], 0) + 1
    print(f"== {budget.used} requests in {dtme:.0f}s | "
          f"catalog +{stats['high']} | pending +{stats['low']} | "
          f"noise {stats['noise']} | duplicates {stats['dup']}")
    for cat in list(tax.CATEGORIES) + [tax.UNCLASSIFIED]:
        if cat in hist:
            print(f"   {cat:<20} {hist[cat]}")
    print(f"catalog -> {args.catalog} ({catalog['count']})")
    print(f"pending -> {args.pending} ({len(pending['entries'])})")
    return rc


if __name__ == "__main__":
    sys.exit(main())
