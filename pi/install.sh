#!/bin/sh
# A314RTG Pi-side installer.
#
#   sudo ./install.sh [a314-shared-dir]
#
# Installs the rtg service into an existing a314 installation:
#   - copies rtg.py to /opt/a314/rtg.py
#   - registers the "rtg" service in a314d.conf (idempotent; /etc/opt/a314/
#     or the other usual places, or CONF=/path/to/a314d.conf)
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
PYTHON="$A314_DIR/venv/bin/python3"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# a314d.conf: where the a314 installer puts it, else the other usual places
CONF=${CONF:-}
if [ -z "$CONF" ]; then
    for c in /etc/opt/a314/a314d.conf "$A314_DIR/a314d.conf" /etc/a314d.conf /etc/a314/a314d.conf; do
        [ -f "$c" ] && { CONF=$c; break; }
    done
fi
[ -n "$CONF" ] && [ -f "$CONF" ] || {
    echo "ERROR: a314d.conf not found - pass it as: sudo CONF=/path/to/a314d.conf ./install.sh"; exit 1; }

# The Amiga-side files: a release package has them in the drawer above pi/
# (monitor already named a314rtg); a source checkout has them in ../rtg with
# the monitor built as p96monitor.
if [ -f "$SCRIPT_DIR/../a314rtg.card" ]; then
    AMIGA_DIR="$SCRIPT_DIR/.."
    MONITOR="$AMIGA_DIR/a314rtg"
else
    AMIGA_DIR="$SCRIPT_DIR/../rtg"
    MONITOR="$AMIGA_DIR/p96monitor"
fi

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
    for f in a314rtg.card a314rtg.info Install_A314RTG; do
        [ -f "$AMIGA_DIR/$f" ] || { echo "ERROR: $AMIGA_DIR/$f not found - Amiga files NOT staged"; exit 1; }
    done
    [ -f "$MONITOR" ] || { echo "ERROR: $MONITOR not found - Amiga files NOT staged"; exit 1; }
    install -m 644 "$AMIGA_DIR/a314rtg.card" "$SHARE/rtg/a314rtg.card"
    # DEVS:Monitors file is the stock board-agnostic P96 loader, deployed
    # WITHOUT an extension so it can be copied straight to DEVS:Monitors/
    # 755: it is a program (a314fs keeps AmigaDOS bits in its own metadata,
    # so this matters only to other ways of sharing the dir, e.g. Samba)
    install -m 755 "$MONITOR"               "$SHARE/rtg/a314rtg"
    install -m 644 "$AMIGA_DIR/a314rtg.info" "$SHARE/rtg/a314rtg.info"
    # AmigaDOS chokes on CR line endings - normalize while staging
    tr -d '\r' < "$AMIGA_DIR/Install_A314RTG" > "$SHARE/rtg/Install_A314RTG"
    chmod 644 "$SHARE/rtg/Install_A314RTG"
    STAGED="a314rtg.card a314rtg a314rtg.info Install_A314RTG"
    if [ -f "$AMIGA_DIR/Install_A314RTG.info" ]; then
        install -m 644 "$AMIGA_DIR/Install_A314RTG.info" "$SHARE/rtg/Install_A314RTG.info"
        STAGED="$STAGED Install_A314RTG.info"
    fi
    # a314fs cannot open root-owned files (verified on hardware 2026-07-24):
    # they LIST fine but every Open fails with "object not found". Hand the
    # staged files to the share's owner.
    SHARE_OWNER=$(stat -c "%U:%G" "$SHARE")
    (cd "$SHARE/rtg" && chown "$SHARE_OWNER" $STAGED)
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
