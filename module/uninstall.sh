#!/system/bin/sh
# bybus uninstall: stop the daemon and drop runtime-only state.
#
# Imported archives, sources and generations live in /data/adb/bybus and are
# kept by default: a reinstall must not lose the user's imported packs. Purge
# only when the user created the explicit marker first.

STATE=/data/adb/bybus
MODDIR=${0%/*}

if [ -x "$MODDIR/bin/bybusctl" ]; then
    "$MODDIR/bin/bybusctl" shutdown >/dev/null 2>&1
fi

# A crashed daemon can leave a stale PID. Never signal it during uninstall;
# shutdown through the module's private control socket above is authoritative.
rm -f "$STATE/bybus.sock" 2>/dev/null
rm -rf "$STATE/run" 2>/dev/null

if [ -f "$STATE/.purge-on-uninstall" ]; then
    rm -rf "$STATE" 2>/dev/null
fi

exit 0
