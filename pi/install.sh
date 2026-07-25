#!/bin/sh
# A314RTG Pi-side installer.
#
#   sudo ./install.sh [a314-shared-dir]
#
# Installs the rtg service into an existing a314 installation:
#   - copies rtg.py to /opt/a314/rtg.py
#   - registers the "rtg" service in /etc/opt/a314/a314d.conf (idempotent)
#   - restarts a314d so the new conf/service take effect
#   - stages the Amiga-side deliverables (a314rtg.card, the monitor loader,
#     its icon and the Install_A314RTG script) into <shared-dir>/rtg/ so the
#     Amiga can copy them straight off PiDisk:
#
# The a314-shared-dir argument is the directory your a314fs exports as
# PiDisk: (default: /home/<invoking user>/a314shared). Pass "-" to skip
# staging entirely.

set -e

A314_DIR=/opt/a314
CONF=/etc/opt/a314/a314d.conf
PYTHON="$A314_DIR/venv/bin/python3"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_RTG="$SCRIPT_DIR/../rtg"

# ---- sanity -------------------------------------------------------------
[ "$(id -u)" = 0 ] || { echo "run me with sudo"; exit 1; }
[ -f "$A314_DIR/a314d.py" ] || {
    echo "ERROR: $A314_DIR/a314d.py not found - install a314 first"; exit 1; }
[ -x "$PYTHON" ] || PYTHON=$(command -v python3)
[ -f "$SCRIPT_DIR/rtg.py" ] || {
    echo "ERROR: rtg.py not found next to install.sh"; exit 1; }

# ---- service ------------------------------------------------------------
install -m 644 "$SCRIPT_DIR/rtg.py" "$A314_DIR/rtg.py"
echo "installed $A314_DIR/rtg.py"

if grep -q "^rtg[[:space:]]" "$CONF" 2>/dev/null; then
    echo "a314d.conf already has an rtg entry - leaving it alone"
else
    printf 'rtg\t%s\t%s\n' "$PYTHON" "$A314_DIR/rtg.py" >> "$CONF"
    echo "registered rtg service in $CONF"
fi

if systemctl is-active --quiet a314d 2>/dev/null; then
    systemctl restart a314d
    echo "restarted a314d"
else
    echo "NOTE: a314d not running under systemd - restart it yourself"
fi

# ---- stage Amiga deliverables on the share ------------------------------
SHARE="${1:-/home/${SUDO_USER:-$(logname 2>/dev/null || echo pi)}/a314shared}"
if [ "$SHARE" = "-" ]; then
    echo "skipping Amiga-file staging (per request)"
elif [ -d "$SHARE" ]; then
    mkdir -p "$SHARE/rtg"
    install -m 644 "$REPO_RTG/a314rtg.card" "$SHARE/rtg/a314rtg.card"
    # DEVS:Monitors file is the stock board-agnostic P96 loader, deployed
    # WITHOUT an extension so it can be copied straight to DEVS:Monitors/
    install -m 644 "$REPO_RTG/p96monitor"   "$SHARE/rtg/a314rtg"
    install -m 644 "$REPO_RTG/a314rtg.info" "$SHARE/rtg/a314rtg.info"
    # AmigaDOS chokes on CR line endings - normalize while staging
    tr -d '\r' < "$REPO_RTG/Install_A314RTG" > "$SHARE/rtg/Install_A314RTG"
    chmod 644 "$SHARE/rtg/Install_A314RTG"
    # a314fs cannot open root-owned files (verified on hardware 2026-07-24):
    # they LIST fine but every Open fails with "object not found". Hand the
    # staged files to the share's owner.
    SHARE_OWNER=$(stat -c "%U:%G" "$SHARE")
    chown "$SHARE_OWNER" "$SHARE/rtg/a314rtg.card" "$SHARE/rtg/a314rtg" \
          "$SHARE/rtg/a314rtg.info" "$SHARE/rtg/Install_A314RTG"
    echo "staged Amiga files in $SHARE/rtg/ (owner $SHARE_OWNER)"
    echo
    echo "On the Amiga (with PiDisk: mounted):"
    echo "    cd PiDisk:rtg"
    echo "    Execute Install_A314RTG"
else
    echo "NOTE: shared dir $SHARE not found - Amiga files NOT staged."
    echo "      rerun as: sudo ./install.sh /path/to/a314shared"
fi

echo "done."
