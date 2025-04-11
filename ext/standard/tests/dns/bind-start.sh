#!/usr/bin/bash

set -euo pipefail

# Resolve script location
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZONES_DIR="$SCRIPT_DIR/zones"
NAMED_CONF="named.conf.local"
PID_FILE="$SCRIPT_DIR/named.pid"
LOG_FILE="$SCRIPT_DIR/named.log"

# Default mode: background
FOREGROUND=false
if [[ "${1:-}" == "-f" ]]; then
    FOREGROUND=true
fi

# Ensure zones directory exists
if [ ! -d "$ZONES_DIR" ]; then
    echo "Zone directory $ZONES_DIR not found."
    exit 1
fi

# Clean up any leftover journal or PID files
rm -f "$ZONES_DIR"/*.jnl "$PID_FILE"

# Print what we're doing
echo "Starting BIND from $SCRIPT_DIR"

if $FOREGROUND; then
    echo "(running in foreground)"
    exec named -c "$NAMED_CONF" -p 53 -u "$(whoami)" -t "$SCRIPT_DIR" -g -d 1
else
    echo "(running in background)"
    named -c "$NAMED_CONF" -p 53 -u "$(whoami)" -t "$SCRIPT_DIR" -P "$PID_FILE" > "$LOG_FILE" 2>&1

    # Wait a moment and confirm startup
    sleep 1
    if [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
        echo "BIND started in background with PID $(cat "$PID_FILE")"
    else
        echo "Failed to start BIND. See $LOG_FILE for details."
        exit 1
    fi
fi
