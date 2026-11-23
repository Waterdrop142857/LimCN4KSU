#!/system/bin/sh
# KernelSU sources this after extracting the module. Its default installer
# applies the correct WebUI permissions and SELinux context.
set_perm "$MODPATH/bin/bybusd" 0 0 0755
set_perm "$MODPATH/bin/bybusctl" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/action.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755
