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
    # Only flip an existing log_file_entry=0 to 1, and only inside [MiSTer].
    # The key is never created and nothing else in the file is touched.
    if [ ! -f "${MISTER_INI}" ]; then
        echo "WARNING: ${MISTER_INI} not found."
        echo "         Please ensure 'log_file_entry=1' is set so the monitor can"
        echo "         detect core/game changes."
        return 0
    fi

    if grep -qiE '^[[:space:]]*log_file_entry[[:space:]]*=[[:space:]]*1' "${MISTER_INI}"; then
        echo "MiSTer.ini already has log_file_entry=1"
        return 0
    fi

    if ! grep -qiE '^[[:space:]]*log_file_entry[[:space:]]*=[[:space:]]*0' "${MISTER_INI}"; then
        echo "WARNING: log_file_entry not found in MiSTer.ini."
        echo "         MiSTer Monitor needs 'log_file_entry=1' under the [MiSTer]"
        echo "         section to detect core/game changes. Please add it manually."
        return 0
    fi

    echo "Setting log_file_entry=1 in MiSTer.ini (was 0)..."
    cp "${MISTER_INI}" "${MISTER_INI}.mmon.bak"
    # The rewrite is only committed when a change actually happened, so a failed
    # pass can never truncate the INI. Python is used (not sed/awk) because it
    # preserves the file byte-for-byte — including whether it ends without a
    # trailing newline — changing only the target line.
    tmp_ini="$(mktemp)"
    if python3 - "${MISTER_INI}" "${tmp_ini}" <<'PYEOF'
import re
import sys

src, dst = sys.argv[1], sys.argv[2]

with open(src, 'r', newline='') as f:
    content = f.read()

had_trailing_newline = content.endswith('\n')
lines = content.split('\n')
if had_trailing_newline:
    lines = lines[:-1]   # drop the empty element produced by the final '\n'

sec_re = re.compile(r'^\s*\[(.+?)\]\s*$')
key_re = re.compile(r'^\s*log_file_entry\s*=\s*0\s*$')

in_mister = False
changed = False
for i, line in enumerate(lines):
    m = sec_re.match(line)
    if m:
        in_mister = (m.group(1).strip().lower() == 'mister')
        continue
    # Only flip the value, only inside [MiSTer]. Leave indentation intact.
    if in_mister and key_re.match(line):
        lines[i] = re.sub(r'(=\s*)0(\s*)$', r'\g<1>1\g<2>', line)
        changed = True

if not changed:
    sys.exit(9)

out = '\n'.join(lines)
if had_trailing_newline:
    out += '\n'
with open(dst, 'w', newline='') as f:
    f.write(out)
sys.exit(0)
PYEOF
    then
        mv "${tmp_ini}" "${MISTER_INI}"
        echo "  Done. A backup was saved to ${MISTER_INI}.mmon.bak"
    else
        rm -f "${tmp_ini}"
        echo "  NOTE: log_file_entry=0 was only found outside the [MiSTer]"
        echo "        section; MiSTer.ini was left unchanged. Please make sure"
        echo "        log_file_entry=1 is set under [MiSTer] manually."
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
