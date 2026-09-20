#!/bin/bash
# MiSTer Monitor — MiSTer-side server
# Copyright (C) 2025-2026 chipster6502
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as
# published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public
# License along with this program.  If not, see
# <https://www.gnu.org/licenses/>.

#
# MiSTer Monitor — setup and launcher
#
# No arguments (Scripts menu): configures auto-start and log_file_entry, then
# restarts the server. Idempotent; safe to run again at any time.
#
# start|stop|restart|status: manages the server process only, without touching
# any configuration. This is what the auto-start line uses.
#

set -e

SCRIPTS_DIR="/media/fat/Scripts"
CONFIG_DIR="${SCRIPTS_DIR}/.config/mister_monitor"
SERVER_PY="${CONFIG_DIR}/mister_status_server.py"
SELF="${SCRIPTS_DIR}/MiSTer_Monitor.sh"
STARTUP_FILE="/media/fat/linux/user-startup.sh"
MISTER_INI="/media/fat/MiSTer.ini"
PID_FILE="/tmp/mister_monitor.pid"
LOG_FILE="/tmp/mister_monitor.log"

STARTUP_COMMENT="# MiSTer Monitor — added by MiSTer_Monitor.sh"
# Invoked through bash so the file never needs the executable bit, which does
# not survive plain HTTP downloads or SMB copies.
STARTUP_LINE="bash ${SELF} start"

# Auto-start lines written by earlier versions, replaced on sight.
LEGACY_COMMENT="# MiSTer Monitor — added by MiSTer_Monitor_setup.sh"
LEGACY_LINE="${SCRIPTS_DIR}/start_monitor.sh start"

start_server() {
    if [ -f "${PID_FILE}" ]; then
        PID=$(cat "${PID_FILE}")
        if kill -0 "${PID}" 2>/dev/null; then
            echo "The server is already running (PID: ${PID})"
            return 0
        fi
    fi
    echo "Starting MiSTer Monitor server..."
    cd "${CONFIG_DIR}"
    nohup python3 -u mister_status_server.py > "${LOG_FILE}" 2>&1 &
    echo $! > "${PID_FILE}"
    echo "Server started (PID: $(cat "${PID_FILE}"))"
}

stop_server() {
    if [ -f "${PID_FILE}" ]; then
        PID=$(cat "${PID_FILE}")
        if kill -0 "${PID}" 2>/dev/null; then
            kill "${PID}"
            rm -f "${PID_FILE}"
            echo "Server stopped"
        else
            echo "The server was not running"
            rm -f "${PID_FILE}"
        fi
    else
        echo "The server was not running"
    fi
}

server_status() {
    if [ -f "${PID_FILE}" ]; then
        PID=$(cat "${PID_FILE}")
        if kill -0 "${PID}" 2>/dev/null; then
            echo "Server running (PID: ${PID})"
            return 0
        fi
    fi
    echo "Server is not running"
}

configure_autostart() {
    if [ ! -f "${STARTUP_FILE}" ]; then
        echo "Creating ${STARTUP_FILE}..."
        mkdir -p "$(dirname "${STARTUP_FILE}")"
        printf '#!/bin/bash\n# user-startup.sh — runs at MiSTer boot.\n' > "${STARTUP_FILE}"
        chmod +x "${STARTUP_FILE}"
    fi

    if grep -qF "${LEGACY_LINE}" "${STARTUP_FILE}"; then
        echo "Migrating the auto-start line in user-startup.sh..."
        sed -i \
            -e "\|^${LEGACY_COMMENT}\$|d" \
            -e "\|${LEGACY_LINE}|d" \
            "${STARTUP_FILE}"
    fi

    if grep -qF "${STARTUP_LINE}" "${STARTUP_FILE}"; then
        echo "Auto-start already configured in user-startup.sh"
    else
        echo "Adding auto-start line to user-startup.sh..."
        printf '\n%s\n%s\n' "${STARTUP_COMMENT}" "${STARTUP_LINE}" >> "${STARTUP_FILE}"
    fi
}

configure_ini() {
    # Evaluates log_file_entry the way MiSTer does: only keys under [MiSTer]
    # count, and the last one wins. When the effective value is 0, the
    # existing lines are flipped to 1. The key is never created or moved, and
    # nothing else in the file is touched.
    if [ ! -f "${MISTER_INI}" ]; then
        echo "WARNING: ${MISTER_INI} not found."
        echo "         Please ensure 'log_file_entry=1' is set so the monitor can"
        echo "         detect core/game changes."
        return 0
    fi

    # Python is used (not sed/awk) because it preserves the file byte-for-byte,
    # changing only the target lines. Exit code 10 means a rewrite is ready.
    tmp_ini="$(mktemp)"
    rc=0
    python3 - "${MISTER_INI}" "${tmp_ini}" <<'PYEOF' || rc=$?
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
KEY = 'log_file_entry'

# latin-1 maps every byte to one character, so any encoding round-trips intact.
with open(src, 'rb') as f:
    lines = f.read().decode('latin-1').split('\n')

# Characters the MiSTer INI reader keeps; everything else is dropped.
invalid_re = re.compile(r'[^A-Za-z0-9\[\]()\-+/=#$@_,.!*:~ \t]')
# Groups: key and separators / value / trailing blanks, comment and CR.
raw_re = re.compile(
    r'^((?:\xef\xbb\xbf)?[ \t]*' + KEY + r'[ \t=]+)([^;\r]*?)([ \t]*(?:;.*)?\r?)$',
    re.IGNORECASE)


def normalize(raw):
    # Comment stripped, invalid characters dropped, outer blanks trimmed.
    return invalid_re.sub('', raw.split(';', 1)[0]).strip(' \t')


def to_flag(text):
    # strtoul(text, 0) clamped to 0..1; non-numeric text reads as 0.
    m = re.match(r'0[xX][0-9a-fA-F]+', text)
    if m:
        return int(int(m.group(0), 16) > 0)
    m = re.match(r'[0-9]+', text)
    return int(int(m.group(0), 10) > 0) if m else 0


def is_main(name):
    return name is not None and name.lower() == 'mister'


section = None      # None until the first [section] header
found = []          # (line index, section name, value)
for i, raw in enumerate(lines):
    text = normalize(raw)
    if text.startswith('['):
        section = text[1:].split(']', 1)[0]
        continue
    m = re.match(r'([^= \t]+)[= \t]*(.*)$', text)
    if m and m.group(1).lower() == KEY:
        found.append((i, section, to_flag(m.group(2))))

inside = [o for o in found if is_main(o[1])]
outside = [o for o in found if not is_main(o[1])]
rewrite = False

if not inside:
    print("WARNING: log_file_entry is not set under the [MiSTer] section.")
    print("         MiSTer Monitor needs 'log_file_entry=1' there to detect game")
    print("         changes. Please add it manually, right below the [MiSTer] line.")
else:
    if len(inside) > 1:
        nums = ', '.join(str(o[0] + 1) for o in inside)
        print(f"NOTE: log_file_entry appears {len(inside)} times under [MiSTer] (lines {nums}).")
        print("      MiSTer uses the last one.")
    if inside[-1][2] == 1:
        print("MiSTer.ini already has log_file_entry=1")
    else:
        done, failed = [], []
        for i, _, value in inside:
            if value != 0:
                continue
            m = raw_re.match(lines[i])
            if m:
                lines[i] = m.group(1) + '1' + m.group(3)
                done.append(i + 1)
            else:
                failed.append(i + 1)
        if done and not failed:
            where = ('line ' if len(done) == 1 else 'lines ') + ', '.join(str(n) for n in done)
            print(f"Setting log_file_entry=1 in MiSTer.ini ({where}, was 0)...")
            rewrite = True
        else:
            nums = ', '.join(str(n) for n in failed)
            print(f"WARNING: log_file_entry is 0 and line {nums} could not be updated.")
            print("         Please set it to 1 manually.")

if outside:
    print("NOTE: log_file_entry also appears outside [MiSTer]:")
    for i, name, value in outside:
        if name is None:
            print(f"        line {i + 1} (={value}), before any [section]: MiSTer ignores it.")
        else:
            effect = "turns it off for" if value == 0 else "only applies to"
            print(f"        line {i + 1} (={value}), under [{name}]: {effect} that core or video mode.")

if rewrite:
    with open(dst, 'wb') as f:
        f.write('\n'.join(lines).encode('latin-1'))
    sys.exit(10)
sys.exit(0)
PYEOF

    if [ "${rc}" -eq 10 ]; then
        cp "${MISTER_INI}" "${MISTER_INI}.mmon.bak"
        mv "${tmp_ini}" "${MISTER_INI}"
        echo "  Done. A backup was saved to ${MISTER_INI}.mmon.bak"
        return 0
    fi

    rm -f "${tmp_ini}"
    if [ "${rc}" -ne 0 ]; then
        echo "WARNING: MiSTer.ini could not be checked. Please make sure"
        echo "         'log_file_entry=1' is set under the [MiSTer] section."
    fi
}

setup_and_run() {
    echo "MiSTer Monitor"
    echo "=============="
    echo

    if [ ! -f "${SERVER_PY}" ]; then
        echo "ERROR: MiSTer Monitor files not found."
        echo "       Expected: ${SERVER_PY}"
        echo
        echo "Run 'Update All' (or 'Downloader') first so the files are installed,"
        echo "then run this again."
        exit 1
    fi

    configure_autostart
    configure_ini

    # Always restart, so an update installed by the Downloader takes effect
    # without waiting for a reboot.
    echo
    stop_server
    sleep 2
    start_server

    echo
    echo "========================================"
    echo "Setup complete."
    echo "========================================"
    echo
    echo "The server is running and will start automatically on boot."
    echo
    echo "To deactivate later, run 'MiSTer_Monitor_uninstall' from the Scripts"
    echo "menu. To check status:  bash ${SELF} status"
    echo
}

case "$1" in
    start)   start_server ;;
    stop)    stop_server ;;
    restart) stop_server; sleep 2; start_server ;;
    status)  server_status ;;
    "")      setup_and_run ;;
    *)
        echo "Use: $0 {start|stop|restart|status}"
        echo "     $0            (configure and restart)"
        exit 1
        ;;
esac
