#!/usr/bin/env python3
"""
Validate mister_status_server.py the way that actually catches web-editor slips.

A missing quote can produce syntactically VALID Python that silently mangles the
table (a value swallows the next line), so py_compile and a regex parse both
pass while the table is wrong. The only reliable check is to LOAD the module and
inspect the real dict object.
"""
import sys, importlib.util, io, os, contextlib

# The server prints emoji banners at import time. On a Windows console (cp1252)
# those raise UnicodeEncodeError before our check even runs — a false failure,
# since the import itself is fine. Force UTF-8 where we can, and swallow the
# server's own stdout/stderr during import: its diagnostics are not ours to show.
try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

SERVER = sys.argv[1] if len(sys.argv) > 1 else \
    "MiSTer/Scripts/.config/mister_monitor/mister_status_server.py"

# 1. Import for real, so a broken dict literal raises and every boot-time import
#    is exercised — but muffle the module's banner so its emoji can't crash the
#    check on a non-UTF-8 console.
spec = importlib.util.spec_from_file_location("srv", SERVER)
mod = importlib.util.module_from_spec(spec)
_sink = io.StringIO()
try:
    with contextlib.redirect_stdout(_sink), contextlib.redirect_stderr(_sink):
        spec.loader.exec_module(mod)
except Exception as e:
    print(f"FAIL: server does not import: {type(e).__name__}: {e}")
    sys.exit(1)

# 2. Table exists, is a dict, has not collapsed.
m = getattr(mod, "CORE_NAME_MAPPING", None)
if not isinstance(m, dict):
    print("FAIL: CORE_NAME_MAPPING is missing or not a dict")
    sys.exit(1)
if len(m) < 100:
    print(f"FAIL: table collapsed to {len(m)} entries")
    sys.exit(1)

# 3. Every key/value is a non-empty string, one line, sane length. A value that
#    swallowed the next line (the missing-quote signature) trips here.
for k, v in m.items():
    if not isinstance(k, str) or not isinstance(v, str) or not k or not v:
        print(f"FAIL: bad entry {k!r}: {v!r}")
        sys.exit(1)
    if "\n" in k or "\n" in v or len(v) > 60:
        print(f"FAIL: entry {k!r} looks mangled (a quote is probably missing): {v[:50]!r}")
        sys.exit(1)

ph = [k for k, v in m.items() if k == v]
print(f"OK: server imports, table has {len(m)} valid entries")
if ph:
    print(f"note: {len(ph)} still placeholders (harmless): "
          + ", ".join(ph[:12]) + (" ..." if len(ph) > 12 else ""))
