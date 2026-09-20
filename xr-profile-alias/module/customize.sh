#!/system/bin/sh
# Magisk install script for pico_xr_profile_alias
MODID=pico_xr_profile_alias
MODDIR=${0%/*}
CONF=/data/adb/pico_xr_alias.conf

ui_print "- $MODID: systemless OpenXR interaction-profile alias"

ui_print "- setting permissions"
set_perm_recursive $MODPATH/system/lib64 0 0 0755 0644
set_perm $MODPATH/system/etc/pico_xr_alias.conf 0 0 0644
chmod 755 $MODPATH/post-fs-data.sh $MODPATH/uninstall.sh $MODPATH/action.sh 2>/dev/null

# NOTE: nothing is ever added to /system/etc/public.libraries.txt -- zygote
# preloads every entry there in both 32- and 64-bit form and aborts the boot
# when one is missing.  The shim uses android_dlopen_ext() with an fd instead.

# runtime configuration ships as a world-readable module file
rm -f /data/adb/pico_xr_alias.conf

rm -f /data/local/tmp/pico_xr_alias.log
ui_print "- installed. Reboot, then launch VRChat."
ui_print "- Log: adb logcat -s PicoXrAlias  |  /data/local/tmp/pico_xr_alias.log"
