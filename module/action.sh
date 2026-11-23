#!/system/bin/sh
# bybus action: KernelSU "Action" button. Prints a short status summary; it does
# not start long tasks (those belong to the WebUI so progress survives closing).

MODDIR=${0%/*}
CTL="$MODDIR/bin/bybusctl"

if [ ! -x "$CTL" ]; then
    echo "bybusctl missing"
    exit 0
fi

echo "bybus status"
"$CTL" status 2>/dev/null
