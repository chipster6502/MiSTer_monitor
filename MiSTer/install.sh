#!/bin/bash
#
# MiSTer Monitor — installer
#
# Usage: copy the entire MiSTer/ folder from this repository to a
# temporary location on your MiSTer (e.g. /tmp/), then run:
#
#   bash /tmp/MiSTer/install.sh
#
# The installer copies all files to their canonical locations,
# configures auto-start, and launches the server immediately.
#

set -e

# ===== Paths =====
SCRIPTS_DIR="/media/fat/Scripts"
CONFIG_DIR="${SCRIPTS_DIR}/.config/mister_monitor"
STARTUP_FILE="/media/fat/linux/user-startup.sh"

# Resolve the directory where THIS script lives, so we can find the
# files relative to it whether the user runs it from the repo, /tmp,
# or any other location.
INSTALLER_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
SOURCE_SCRIPTS_DIR="${INSTALLER_DIR}/Scripts"

# ===== Sanity checks =====
echo "MiSTer Monitor installer"
echo "========================"
echo

if [ ! -d "/media/fat" ]; then
    echo "ERROR: /media/fat not found. Are you running this on a MiSTer?"
    exit 1
fi

if [ ! -f "${SOURCE_SCRIPTS_DIR}/start_monitor.sh" ]; then
    echo "ERROR: cannot find start_monitor.sh at ${SOURCE_SCRIPTS_DIR}/"
    echo "Make sure you copied the entire MiSTer/ folder, not just install.sh."
    exit 1
fi

if [ ! -f "${SOURCE_SCRIPTS_DIR}/.config/mister_monitor/mister_status_server.py" ]; then
    echo "ERROR: cannot find mister_status_server.py at ${SOURCE_SCRIPTS_DIR}/.config/mister_monitor/"
    exit 1
fi

# ===== Stop any running instance =====
if [ -f "${SCRIPTS_DIR}/start_monitor.sh" ]; then
    echo "Existing installation found, stopping current server..."
    "${SCRIPTS_DIR}/start_monitor.sh" stop 2>/dev/null || true
    sleep 1
fi

# ===== Create directory structure =====
echo "Creating ${CONFIG_DIR}..."
mkdir -p "${CONFIG_DIR}"

# ===== Copy files =====
echo "Installing mister_status_server.py..."
cp "${SOURCE_SCRIPTS_DIR}/.config/mister_monitor/mister_status_server.py" "${CONFIG_DIR}/"

echo "Installing start_monitor.sh..."
cp "${SOURCE_SCRIPTS_DIR}/start_monitor.sh" "${SCRIPTS_DIR}/"
chmod +x "${SCRIPTS_DIR}/start_monitor.sh"

# ===== Configure auto-start =====
STARTUP_LINE="${SCRIPTS_DIR}/start_monitor.sh start"

if [ ! -f "${STARTUP_FILE}" ]; then
    echo "Creating ${STARTUP_FILE}..."
    mkdir -p "$(dirname "${STARTUP_FILE}")"
    cat > "${STARTUP_FILE}" <<'EOF'
#!/bin/bash
# user-startup.sh — runs at MiSTer boot.

EOF
    chmod +x "${STARTUP_FILE}"
fi

if grep -qF "${STARTUP_LINE}" "${STARTUP_FILE}"; then
    echo "Auto-start already configured in ${STARTUP_FILE}"
else
    echo "Adding auto-start line to ${STARTUP_FILE}..."
    echo "" >> "${STARTUP_FILE}"
    echo "# MiSTer Monitor — added by install.sh" >> "${STARTUP_FILE}"
    echo "${STARTUP_LINE}" >> "${STARTUP_FILE}"
fi

# ===== Verify MiSTer.ini setting =====
MISTER_INI="/media/fat/MiSTer.ini"
if [ -f "${MISTER_INI}" ]; then
    if grep -qE '^\s*log_file_entry\s*=\s*1' "${MISTER_INI}"; then
        echo "MiSTer.ini already has log_file_entry=1"
    else
        echo
        echo "WARNING: log_file_entry=1 is NOT set in ${MISTER_INI}"
        echo "         The monitor needs this to detect core/game changes."
        echo "         Please add the following line under the [MiSTer] section:"
        echo "             log_file_entry=1"
        echo
    fi
fi

# ===== Start the server now =====
echo
echo "Starting MiSTer Monitor server..."
"${SCRIPTS_DIR}/start_monitor.sh" start

# ===== Final message =====
echo
echo "========================================"
echo "Installation complete."
echo "========================================"
echo
echo "The monitor server is running and will start automatically on boot."
echo
echo "Next steps:"
echo "  1. Make sure your MiSTer.ini has log_file_entry=1 (see above)."
echo "  2. Configure config.ini on your Tab5 SD card with WiFi/MiSTer IP/ScreenScraper credentials."
echo "  3. Flash the firmware to your Tab5 (see README for details)."
echo
echo "To check the server status:    ${SCRIPTS_DIR}/start_monitor.sh status"
echo "To stop the server:            ${SCRIPTS_DIR}/start_monitor.sh stop"
echo "To uninstall:                  bash ${INSTALLER_DIR}/uninstall.sh"
echo