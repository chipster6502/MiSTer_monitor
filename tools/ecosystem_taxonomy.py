#!/usr/bin/env python3
"""
ecosystem_taxonomy.py — one taxonomy for everything the observatory catalogs.

WHY THIS EXISTS
    Two entry points feed the ecosystem catalog: backfill_github.py (the
    one-shot historical sweep) and scan_github.py (the weekly delta). If each
    carried its own category list and heuristics they would drift, and the
    same repo would land in a different category depending on which script
    found it first. This module is the single source of truth both import.

WHAT IT IS NOT
    scan_github.py's core/news split answers a MAPPING question ("could this
    repo ever need a CORE_NAME_MAPPING entry?") — that is why Arcade-* counts
    as 'news' there. The catalog answers a LIBRARY question ("what is this
    repo to a MiSTer user?"), where an arcade core is simply a core. The two
    classifications coexist on purpose; do not unify them.

CATEGORIES
    They mirror how the misterfpga.org forum organises the ecosystem, plus
    'core' for Part A (FPGA cores as repos, official or not). Category ids
    are a stable contract — the published JSON and the page key on them.
    Labels are presentation only and free to change.

CLASSIFIER CONTRACT
    classify(repo) -> (category, confidence, reasons)
        category    one of CATEGORIES, or UNCLASSIFIED, or None (= not
                    MiSTer-related; drop it, do not queue it)
        confidence  'high' (catalog-grade) | 'low' (goes to the pending
                    review queue) | None when category is None
        reasons     list of short strings explaining the verdict, so the
                    human reviewing pending.json sees WHY, not just WHAT
    Heuristics feed a review queue by design: 'low' is an honest answer,
    not a failure mode. Precision matters more than looking decisive.
"""

import re

# ---------------------------------------------------------------------------
# The taxonomy. Ordered: on a score tie the earlier category wins, so the
# order encodes "most specific first" (a repo mentioning both 'toolchain'
# and 'tool' is core development, not a generic utility).
# ---------------------------------------------------------------------------
CATEGORIES = {
    "core":               "FPGA cores",
    "hardware_clones":    "Hardware: clones & base boards",
    "hardware_addons":    "Hardware: add-ons & expansions",
    "cases_3d":           "Cases, mounts & 3D printing",
    "input_devices":      "Input devices",
    "displays_companion": "Displays & companion devices",
    "core_dev":           "Core development",
    "docs_media":         "Documentation & media",
    "scripts_utilities":  "Scripts & utilities",
}

# Pseudo-category: clearly MiSTer-related, matches no bucket. Only ever valid
# inside the pending queue — publishing it in catalog.json is a bug.
UNCLASSIFIED = "unclassified"

# The <Core>_MiSTer convention, case-insensitive for the same reason as in
# scan_github.py: 'ZX-Spectrum_MISTer' and 'X16_Mister' both exist.
CORE_NAME_RE = re.compile(r"_mister$", re.I)

# 'mister' as a WORD, or the one-word compound 'misterfpga'. A plain substring
# test ("mister" in text) was the single biggest source of noise in the
# backfill sample: it fired on 'mistertoy', 'misterioso', 'misterix',
# 'misterbeef' — dozens of forks of a coding-bootcamp exercise and assorted
# mystery-themed games, none of them MiSTer FPGA. A word boundary drops all of
# those while still matching 'mister-de1-soc', '0t4ku-mister-scripts' and
# (via the alternative) the community's one-word 'MiSTerFPGA'. Separators are
# normalised to spaces before this runs, so 'mister-fpga' is caught by \bmister\b
# and only the truly unseparated 'misterfpga' needs spelling out.
_MISTER_WORD_RE = re.compile(r"\bmister\b|misterfpga", re.I)

HDL_LANGUAGES = {"verilog", "systemverilog", "vhdl"}

# Languages that, absent any other signal, suggest "software around MiSTer"
# rather than gateware. Used only to pre-fill a guess for the review queue.
SCRIPTING_LANGUAGES = {"python", "shell", "go", "rust", "javascript",
                       "typescript", "c#", "java", "php", "ruby", "lua"}

# Owners whose presence alone makes a repo MiSTer-related. Relatedness only —
# this is NOT scan_github.py's COVERED_OWNERS, which encodes database
# coverage for the mapping pipeline.
MISTER_ORGS = {
    "mister-devel", "mister-db9", "mister-llapi", "mister-unstable-nightlies",
    "misterfpga", "misteraddons",
}

TOPIC_SIGNALS = {"mister-fpga", "misterfpga", "mister_fpga"}

# Words that, next to 'mister', settle the Mr./FPGA ambiguity. Same rationale
# as scan_github.py's NEWSY_RE: no \b before 'fpga' because the community
# writes 'MisterFPGA' as one word. 'core' and 'arcade' are borrowed from
# NEWSY_RE after a live test dropped iequalshane/megaplay_mister_beta
# ("...Sega Mega Play MiSTer core") for lacking them. NEWSY_RE's 'retro' and
# 'analogue' are deliberately NOT borrowed: next to a name merely containing
# 'mister' they admit too much (retro-themed anything), and the repos they
# would rescue also say core/fpga in practice.
#
# de1/sockit/quartus/cyclone were added after the backfill sample: they are the
# Terasic boards MiSTer forks target (DE1-SoC, SoCKit), Intel's FPGA IDE, and
# the FPGA family on the DE10-Nano — all high-precision, they essentially never
# appear in a non-FPGA repo. \bde1\b is bounded so it does not fire inside
# 'de10', which is listed separately.
_CONTEXT_RE = re.compile(r"(fpga|de10|de-10|\bde1\b|sockit|quartus|cyclone|"
                         r"update_all|tty2oled|sidi|mistex|\brbf\b|\bmra\b|"
                         r"\bcores?\b|arcade|snac|misterfpga)", re.I)

# ---------------------------------------------------------------------------
# Per-category signals: (kind, value, weight).
#   kw       keyword matched with a LEADING word boundary only, so 'updater',
#            'launchers' and '3d printed' all count, while 'showcase' does
#            not trip 'case'. Hit in name or topics scores full weight; a
#            hit only in the description scores 1 (descriptions ramble).
#   lang     GitHub's primary language, exact (lowercased).
#   name_re  regex against the bare repo name.
# ---------------------------------------------------------------------------
_SIGNALS = {
    "core": [
        ("name_re", CORE_NAME_RE, 3),
        ("lang", HDL_LANGUAGES, 3),
        ("kw", "fpga core", 2), ("kw", "rbf", 2), ("kw", "mra", 2),
        ("kw", "arcade core", 2),
    ],
    "hardware_clones": [
        ("kw", "mister pi", 3), ("kw", "misterpi", 3),
        ("kw", "multisystem", 3), ("kw", "qmtech", 3),
        ("kw", "mainboard", 2), ("kw", "motherboard", 2), ("kw", "clone", 1),
    ],
    "hardware_addons": [
        ("kw", "io board", 3), ("kw", "i/o board", 3), ("kw", "ioboard", 3),
        ("kw", "analog io", 3), ("kw", "sdram", 3), ("kw", "ram board", 3),
        ("kw", "db9", 3), ("kw", "snac", 3), ("kw", "rtc", 2),
        ("kw", "expansion", 2), ("kw", "addon", 2), ("kw", "add-on", 2),
        ("kw", "daughterboard", 2), ("kw", "hat", 1), ("kw", "shield", 1),
        ("kw", "pcb", 1),
    ],
    "cases_3d": [
        ("lang", {"openscad"}, 3),
        ("kw", "3d print", 3), ("kw", "3d-print", 3), ("kw", "stl", 3),
        ("kw", "enclosure", 3), ("kw", "case", 2), ("kw", "printable", 2),
        ("kw", "faceplate", 2), ("kw", "stand", 1), ("kw", "bracket", 1),
    ],
    "input_devices": [
        ("kw", "controller", 3), ("kw", "gamepad", 3), ("kw", "joystick", 3),
        ("kw", "lightgun", 3), ("kw", "light gun", 3), ("kw", "spinner", 3),
        ("kw", "paddle", 2), ("kw", "trackball", 2), ("kw", "keyboard", 2),
        ("kw", "mouse", 2), ("kw", "input lag", 2), ("kw", "wiimote", 2),
        ("kw", "sinden", 2), ("kw", "bluetooth", 1),
    ],
    "displays_companion": [
        ("kw", "tty2oled", 3), ("kw", "tty2", 3), ("kw", "oled", 3),
        ("kw", "companion", 3), ("kw", "marquee", 3), ("kw", "vfd", 3),
        ("kw", "e-paper", 3), ("kw", "epaper", 3), ("kw", "lcd", 2),
        ("kw", "second screen", 2), ("kw", "video filter", 2),
        ("kw", "scanline", 2), ("kw", "shadow mask", 2), ("kw", "display", 1),
        ("kw", "screen", 1),
    ],
    "core_dev": [
        ("kw", "toolchain", 3), ("kw", "boilerplate", 3),
        ("kw", "testbench", 3), ("kw", "quartus", 3), ("kw", "devkit", 2),
        ("kw", "framework", 2), ("kw", "template", 2), ("kw", "sdk", 2),
        ("kw", "hdl", 2), ("kw", "simulation", 1),
    ],
    "docs_media": [
        ("kw", "wiki", 3), ("kw", "documentation", 3), ("kw", "cheatsheet", 3),
        ("kw", "cheat sheet", 3), ("kw", "awesome", 3), ("kw", "wallpaper", 3),
        ("kw", "artwork", 3), ("kw", "guide", 2), ("kw", "manual", 2),
        ("kw", "tutorial", 2), ("kw", "translation", 2), ("kw", "font", 2),
        ("kw", "curated", 2), ("kw", "faq", 2), ("kw", "docs", 1),
        ("kw", "notes", 1),
    ],
    "scripts_utilities": [
        ("kw", "updater", 3), ("kw", "update_all", 3), ("kw", "update all", 3),
        ("kw", "downloader", 3), ("kw", "launcher", 3), ("kw", "frontend", 3),
        ("kw", "installer", 2), ("kw", "script", 2), ("kw", "manager", 2),
        ("kw", "automation", 2), ("kw", "cli", 2), ("kw", "daemon", 2),
        ("kw", "backup", 2), ("kw", "sync", 1), ("kw", "utility", 1),
        ("kw", "tool", 1), ("kw", "server", 1), ("kw", "generator", 1),
        ("kw", "bot", 1), ("kw", "rom", 1),
    ],
}


def _kw_hit(kw, hay):
    """Leading word boundary, open ending: 'updaters' and '3d printed' count,
    'showcase' does not trip 'case'."""
    return re.search(r"\b" + re.escape(kw), hay) is not None


def classify(repo):
    """See CLASSIFIER CONTRACT in the module docstring."""
    name = (repo.get("name") or "")
    desc = (repo.get("description") or "")
    owner = ((repo.get("owner") or {}).get("login") or "").lower()
    topics = [t.lower() for t in (repo.get("topics") or [])]
    lang = (repo.get("language") or "").lower()

    # Normalise separators so 'tty2oled_sender' exposes its words.
    hay_name = re.sub(r"[-_./]+", " ", name.lower())
    hay_topics = " ".join(t.replace("-", " ") for t in topics)
    hay_desc = desc.lower()
    hay_all = " ".join((hay_name, hay_topics, hay_desc))

    reasons = []

    # -- relatedness -------------------------------------------------------
    strong = False
    if any(t in TOPIC_SIGNALS for t in topics):
        strong = True
        reasons.append("topic says mister-fpga")
    if CORE_NAME_RE.search(name):
        strong = True
        reasons.append("name follows <Core>_MiSTer")
    if owner in MISTER_ORGS:
        strong = True
        reasons.append(f"owner {owner} is a MiSTer org")
    has_mister = bool(_MISTER_WORD_RE.search(hay_all))
    if not strong and has_mister and _CONTEXT_RE.search(hay_all):
        strong = True
        reasons.append("says 'mister' next to FPGA context")

    # 'mister' can be glued to its suffix with no separator, and then the word
    # boundary above cannot see it: 'mymistertemplate', 'mister2mega65'. At the
    # text level these are indistinguishable from the noise the word boundary
    # correctly drops ('mistertoy', 'misterix') — the tell is the language.
    # An HDL repo (Verilog/SystemVerilog/VHDL) is almost always FPGA, so a
    # glued 'mister' mention in HDL is almost certainly a MiSTer core, while the
    # noise repos are JavaScript/PHP/etc. Substring match on purpose here: this
    # path exists precisely to catch the glued names the boundary misses. It
    # grants relatedness, not strength — these land in pending for a human to
    # confirm, never straight in the catalog.
    mentions_mister = "mister" in hay_all
    hdl_mister = mentions_mister and lang in HDL_LANGUAGES
    if hdl_mister and not has_mister:
        reasons.append(f"HDL ({lang}) repo with a glued 'mister' in the name")

    # -- category scoring --------------------------------------------------
    scores = {}
    hits = {}
    for cat, signals in _SIGNALS.items():
        s = 0
        why = []
        for kind, value, weight in signals:
            if kind == "kw":
                if _kw_hit(value, hay_name) or _kw_hit(value, hay_topics):
                    s += weight
                    why.append(f"'{value}' in name/topics")
                elif _kw_hit(value, hay_desc):
                    s += 1
                    why.append(f"'{value}' in description")
            elif kind == "lang" and lang in value:
                s += weight
                why.append(f"language {lang}")
            elif kind == "name_re" and value.search(name):
                s += weight
                why.append("name matches _MiSTer")
        if s:
            scores[cat] = s
            hits[cat] = why

    # -- verdict -----------------------------------------------------------
    # Not related at all: no strong signal, no weak 'mister'-word + category
    # combination, and not an HDL repo with a glued 'mister'. This is the
    # mister-midpines/star-wars filter — drop, never queue, or the review
    # queue becomes the noise.
    if not strong and not (has_mister and scores) and not hdl_mister:
        return None, None, ["no MiSTer signal"]

    if not scores:
        # Related but bucket-less. Pre-fill a guess when the language leans
        # one way, so the reviewer confirms instead of picking from scratch.
        if lang in HDL_LANGUAGES:
            return "core", "low", reasons + [f"HDL ({lang}) but no other signal"]
        if lang in SCRIPTING_LANGUAGES:
            return "scripts_utilities", "low", reasons + \
                [f"{lang} project, no category keyword"]
        return UNCLASSIFIED, "low", reasons + ["no category keyword matched"]

    # Highest score wins; CATEGORIES order breaks ties (specific beats
    # generic — dict order is the tiebreak by construction).
    best = max(CATEGORIES, key=lambda c: scores.get(c, 0))
    reasons += hits[best]
    confidence = "high" if (strong and scores[best] >= 3) else "low"
    return best, confidence, reasons


def make_entry(repo, category, source, today):
    """The catalog record. One shape everywhere: catalog.json, pending queue
    and latest.json all carry these fields, so a reviewed pending entry moves
    into the catalog verbatim (extra review fields are ignored downstream)."""
    return {
        "id": repo["full_name"].lower(),          # stable key, dedupe on this
        "name": repo.get("name") or "",
        "owner": (repo.get("owner") or {}).get("login", ""),
        "url": repo.get("html_url") or "",
        "homepage": (repo.get("homepage") or "").strip(),
        "description": (repo.get("description") or "").strip(),
        "category": category,
        "language": repo.get("language") or "",
        "topics": repo.get("topics") or [],
        "stars": repo.get("stargazers_count", 0),
        "archived": bool(repo.get("archived", False)),
        "created": (repo.get("created_at") or "")[:10],
        "pushed": (repo.get("pushed_at") or repo.get("created_at") or "")[:10],
        "added": today,                           # when it entered the catalog
        "source": source,                         # 'backfill' | 'weekly'
    }
