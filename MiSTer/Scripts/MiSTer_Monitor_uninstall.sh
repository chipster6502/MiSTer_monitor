#!/bin/bash
#
# MiSTer Monitor — uninstall (deactivate)
#
# This DEACTIVATES MiSTer Monitor: it stops the server and removes the
# system integration, but it deliberately LEAVES THE FILES in place and does
# NOT touch the Downloader's state. This is the safe, reversible model used
# across the MiSTer ecosystem.
#
#   - Reactivate later: run 'MiSTer_Monitor_setup' again (no re-download).
#   - Remove completely: run this, then delete the drop-in
#     'downloader_chipster6502_MiSTer_monitor_DB.ini' from the root of your SD
#     so the Downloader stops tracking/updating the files.
#
# log_file_entry in MiSTer.ini is intentionally NOT reverted (other tools may
# rely on it); we only inform you about it below.
#

set -e

SCRIPTS_DIR="/media/fat/Scripts"
START_SCRIPT="${SCRIPTS_DIR}/start_monitor.sh"
STARTUP_FILE="/media/fat/linux/user-startup.sh"

STARTUP_COMMENT="# MiSTer Monitor — added by MiSTer_Monitor_setup.sh"
STARTUP_LINE="${START_SCRIPT} start"

echo "MiSTer Monitor uninstall (deactivate)"
echo "====================================="
echo

# ===== 1. Stop the server =====
if [ -f "${START_SCRIPT}" ]; then
    echo "Stopping server..."
    "${START_SCRIPT}" stop 2>/dev/null || true
    sleep 1
fi

# ===== 2. Remove auto-start from user-startup.sh =====
# Remove both the comment line and the command line, leaving everything else
# in the file untouched.
if [ -f "${STARTUP_FILE}" ]; then
    if grep -qF "${STARTUP_LINE}" "${STARTUP_FILE}"; then
        echo "Removing auto-start line from user-startup.sh..."
        sed -i \
            -e "\|^${STARTUP_COMMENT}\$|d" \
            -e "\|${STARTUP_LINE}|d" \
            "${STARTUP_FILE}"
    else
        echo "No auto-start line found in user-startup.sh (already removed)."
    fi
fi

# ===== 3. Clean up runtime files =====
rm -f /tmp/mister_monitor.pid
rm -f /tmp/mister_monitor.log

# ===== Done =====
echo
echo "========================================"
echo "MiSTer Monitor deactivated."
echo "========================================"
echo
echo "The server is stopped and will no longer start on boot."
echo
echo "The program files were KEPT in place, so you can reactivate at any time"
echo "by running 'MiSTer_Monitor_setup' from the Scripts menu — no re-download."
echo
echo "Notes:"
echo "  - To remove MiSTer Monitor COMPLETELY, also delete the drop-in file"
echo "    'downloader_chipster6502_MiSTer_monitor_DB.ini' from the root of your"
echo "    SD card (/media/fat/) so the Downloader stops tracking it."
echo "  - log_file_entry=1 in MiSTer.ini was NOT changed (other tools may rely"
echo "    on it). Edit it manually if you want it back to 0."
echo "  - The display firmware on your device was NOT touched."
echo
