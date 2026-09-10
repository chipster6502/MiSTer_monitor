#!/usr/bin/env python3
# bump_firmware_version.py — set a new release version across the project.
# Usage: bump_firmware_version.py <new-version>
# Run from the repo root. Anchored (exactly one match required), binary-safe
# (CRLF/LF preserved untouched), idempotent (SKIP when already bumped).
import re
import sys
import pathlib

TARGETS = [
    ("mister_monitor_CYD28R_ILI9341/mister_monitor_CYD28R_ILI9341.ino",
     b'#define FIRMWARE_VERSION "%s"'),
    ("mister_monitor_CYD28R_ST7789/mister_monitor_CYD28R_ST7789.ino",
     b'#define FIRMWARE_VERSION "%s"'),
    ("mister_monitor_CYD35C/mister_monitor_CYD35C.ino",
     b'#define FIRMWARE_VERSION "%s"'),
    ("mister_monitor_CYD35R/mister_monitor_CYD35R.ino",
     b'#define FIRMWARE_VERSION "%s"'),
    ("mister_monitor_Tab5/mister_monitor_Tab5.ino",
     b'#define FIRMWARE_VERSION "%s"'),
     ("mister_monitor_Guition10inch/mister_monitor_Guition10inch.ino",
     b'#define FIRMWARE_VERSION "%s"'),
    ("MiSTer/Scripts/.config/mister_monitor/mister_status_server.py",
     b'SERVER_VERSION = "%s"'),
]

if len(sys.argv) != 2 or not re.fullmatch(r"\d+\.\d+\.\d+", sys.argv[1]):
    sys.exit("usage: bump_firmware_version.py <new-version>   e.g. 2.8.0")
new = sys.argv[1].encode()

failed = False
for rel, tmpl in TARGETS:
    p = pathlib.Path(rel)
    if not p.exists():
        print(f"FAIL  {rel}  (file not found — run from the repo root)")
        failed = True
        continue
    data = p.read_bytes()
    if data.count(tmpl % new) == 1:
        print(f"SKIP  {rel}  (already {new.decode()})")
        continue
    pattern = re.escape(tmpl % b"\x00").replace(
        re.escape(b"\x00"), rb'([0-9]+\.[0-9]+\.[0-9]+)')
    found = re.findall(pattern, data)
    if len(found) != 1:
        print(f"FAIL  {rel}  ({len(found)} matches, expected 1)")
        failed = True
        continue
    p.write_bytes(data.replace(tmpl % found[0], tmpl % new))
    print(f"OK    {rel}  {found[0].decode()} -> {new.decode()}")

sys.exit(1 if failed else 0)
