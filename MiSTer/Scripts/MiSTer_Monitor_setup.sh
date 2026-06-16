#!/bin/bash
#
# MiSTer Monitor — setup
#
# Run this ONCE from the MiSTer Scripts menu after the Downloader (Update All)
# has installed the MiSTer Monitor files. It performs the one-time system
# integration that the Downloader cannot do by itself:
#
#   1. Makes start_monitor.sh executable.
#   2. Adds the auto-start line to user-startup.sh (if not already there).
#   3. Ensures log_file_entry=1 in MiSTer.ini (only flips an existing 0 -> 1).
#   4. Starts the server.
#
# It is idempotent: running it again is safe and simply re-asserts the above.
#

set -e

SCRIPTS_DIR="/media/fat/Scripts"
CONFIG_DIR="${SCRIPTS_DIR}/.config/mister_monitor"
STARTUP_FILE="/media/fat/linux/user-startup.sh"
START_SCRIPT="${SCRIPTS_DIR}/start_monitor.sh"
SERVER_PY="${CONFIG_DIR}/mister_status_server.py"
MISTER_INI="/media/fat/MiSTer.ini"

STARTUP_COMMENT="# MiSTer Monitor — added by MiSTer_Monitor_setup.sh"
STARTUP_LINE="${START_SCRIPT} start"

echo "MiSTer Monitor setup"
echo "===================="
echo

# ===== Sanity checks =====
# The Downloader must have installed the files before this runs.
if [ ! -f "${START_SCRIPT}" ] || [ ! -f "${SERVER_PY}" ]; then
    echo "ERROR: MiSTer Monitor files not found."
    echo "       Expected:"
    echo "         ${START_SCRIPT}"
    echo "         ${SERVER_PY}"
    echo
    echo "Run 'Update All' (or 'Downloader') first so the Downloader installs"
    echo "the files, then run this setup again."
    exit 1
fi

# ===== 1. Make the launcher executable =====
echo "Making start_monitor.sh executable..."
chmod +x "${START_SCRIPT}"

# ===== 2. Configure auto-start in user-startup.sh =====
# Create the file if it does not exist (this is the standard MiSTer hook file).
if [ ! -f "${STARTUP_FILE}" ]; then
    echo "Creating ${STARTUP_FILE}..."
    mkdir -p "$(dirname "${STARTUP_FILE}")"
    printf '#!/bin/bash\n# user-startup.sh — runs at MiSTer boot.\n' > "${STARTUP_FILE}"
    chmod +x "${STARTUP_FILE}"
fi

if grep -qF "${STARTUP_LINE}" "${STARTUP_FILE}"; then
    echo "Auto-start already configured in user-startup.sh"
else
    echo "Adding auto-start line to user-startup.sh..."
    printf '\n%s\n%s\n' "${STARTUP_COMMENT}" "${STARTUP_LINE}" >> "${STARTUP_FILE}"
fi

# ===== 3. Ensure log_file_entry=1 in MiSTer.ini =====
# We ONLY flip an existing log_file_entry=0 to 1, and ONLY inside the [MiSTer]
# section. We never create the key, and we touch nothing else in the file.
# A section-aware awk pass is used (not a global sed) so an identically-named
# key under another section — e.g. a core-specific [nes] — is never altered.
if [ -f "${MISTER_INI}" ]; then
    if grep -qiE '^[[:space:]]*log_file_entry[[:space:]]*=[[:space:]]*1' "${MISTER_INI}"; then
        echo "MiSTer.ini already has log_file_entry=1"
    elif grep -qiE '^[[:space:]]*log_file_entry[[:space:]]*=[[:space:]]*0' "${MISTER_INI}"; then
        echo "Setting log_file_entry=1 in MiSTer.ini (was 0)..."
        # Back up before touching a shared, sensitive file.
        cp "${MISTER_INI}" "${MISTER_INI}.mmon.bak"
        # We only commit the rewrite when a change actually happened, so a
        # failed pass can never truncate the INI. Python is used (not sed/awk)
        # because it preserves the file byte-for-byte — including whether the
        # file ends without a trailing newline — changing only the target line.
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
    else
        echo "WARNING: log_file_entry not found in MiSTer.ini."
        echo "         MiSTer Monitor needs 'log_file_entry=1' under the [MiSTer]"
        echo "         section to detect core/game changes. Please add it manually."
    fi
else
    echo "WARNING: ${MISTER_INI} not found."
    echo "         Please ensure 'log_file_entry=1' is set so the monitor can"
    echo "         detect core/game changes."
fi

# ===== 4. Start the server =====
echo
echo "Starting MiSTer Monitor server..."
"${START_SCRIPT}" start

# ===== Done =====
echo
echo "========================================"
echo "Setup complete."
echo "========================================"
echo
echo "The server is running and will start automatically on boot."
echo
echo "To uninstall (deactivate) later, run 'MiSTer_Monitor_uninstall' from the"
echo "Scripts menu. To check status:  ${START_SCRIPT} status"
echo
