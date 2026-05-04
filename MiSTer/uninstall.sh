#!/bin/bash
#
# MiSTer Monitor — uninstaller
# Removes the MiSTer-side server and all its files.
#

set -e

SCRIPTS_DIR="/media/fat/Scripts"
CONFIG_DIR="${SCRIPTS_DIR}/.config/mister_monitor"
STARTUP_FILE="/media/fat/linux/user-startup.sh"
START_SCRIPT="${SCRIPTS_DIR}/start_monitor.sh"

echo "MiSTer Monitor uninstaller"
echo "=========================="
echo

# ===== Stop the server =====
if [ -f "${START_SCRIPT}" ]; then
    echo "Stopping server..."
    "${START_SCRIPT}" stop 2>/dev/null || true
    sleep 1
fi

# ===== Remove auto-start line =====
if [ -f "${STARTUP_FILE}" ]; then
    if grep -qF "${START_SCRIPT} start" "${STARTUP_FILE}"; then
        echo "Removing auto-start line from ${STARTUP_FILE}..."
        # Remove both the comment line and the command line
        sed -i \
            -e '/# MiSTer Monitor — added by install.sh/d' \
            -e "\|${START_SCRIPT} start|d" \
            "${STARTUP_FILE}"
    fi
fi

# ===== Remove files =====
if [ -d "${CONFIG_DIR}" ]; then
    echo "Removing ${CONFIG_DIR}..."
    rm -rf "${CONFIG_DIR}"
fi

if [ -f "${START_SCRIPT}" ]; then
    echo "Removing ${START_SCRIPT}..."
    rm -f "${START_SCRIPT}"
fi

# ===== Cleanup runtime files =====
rm -f /tmp/mister_monitor.pid
rm -f /tmp/mister_monitor.log

echo
echo "========================================"
echo "Uninstall complete."
echo "========================================"
echo
echo "The MiSTer-side monitor has been removed."
echo
echo "Notes:"
echo "  - log_file_entry=1 in MiSTer.ini was NOT changed (other tools may rely on it)."
echo "  - The Tab5 firmware on your device was NOT touched (it lives on the Tab5 SD card)."
echo