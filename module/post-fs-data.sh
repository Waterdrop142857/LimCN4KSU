#!/system/bin/sh
# bybus post-fs-data: remove leftovers of an interrupted run before anything
# starts. Only directories that provably belong to this module are touched.

STATE=/data/adb/bybus

# Stale socket: the daemon recreates it after boot.
rm -f "$STATE/bybus.sock" 2>/dev/null
rm -rf "$STATE/run" 2>/dev/null
mkdir -p "$STATE/run" 2>/dev/null
chmod 0700 "$STATE/run" 2>/dev/null

# Uncommitted import sessions and half-built generations are never usable.
if [ -d "$STATE/imports" ]; then
    find "$STATE/imports" -mindepth 1 -maxdepth 1 -type d -exec rm -rf {} + 2>/dev/null
fi
if [ -d "$STATE/generations" ]; then
    find "$STATE/generations" -mindepth 1 -maxdepth 1 -type d -name '.tmp-*' -exec rm -rf {} + 2>/dev/null
fi

exit 0
