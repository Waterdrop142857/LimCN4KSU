#!/system/bin/sh
# bybus service: prepare the private state root and start the daemon once.
# KernelSU invokes this in late_start service mode as root.

MODDIR=${0%/*}
STATE=/data/adb/bybus
LOG="$STATE/logs/service.log"

umask 022
mkdir -p "$STATE" "$STATE/sources" "$STATE/archives" "$STATE/imports" \
    "$STATE/generations" "$STATE/jobs" "$STATE/logs" "$STATE/runtime" \
    "$STATE/inbox" "$STATE/run" 2>/dev/null
chown root:root "$STATE" 2>/dev/null
chmod 0700 "$STATE" 2>/dev/null
chmod 0700 "$STATE/sources" "$STATE/archives" "$STATE/imports" "$STATE/generations" \
    "$STATE/jobs" "$STATE/runtime" "$STATE/run" 2>/dev/null
chmod 0755 "$STATE/inbox" 2>/dev/null

# Module files must stay readable by the manager; the state root stays private.
chmod 0755 "$MODDIR/bin" 2>/dev/null
chmod 0755 "$MODDIR/bin/bybusctl" 2>/dev/null
chmod 0755 "$MODDIR/bin/bybusd" 2>/dev/null

if [ ! -x "$MODDIR/bin/bybusd" ]; then
    echo "bybusd missing" >>"$LOG"
    exit 0
fi

# The daemon holds its own instance lock; starting twice is harmless but the
# second instance exits immediately.
"$MODDIR/bin/bybusd" >/dev/null 2>&1 &

exit 0
