#!/system/bin/sh
# Magisk install script for the pico-vrchat-adapt sideload helper module.
MODDIR=${0%/*}

ui_print "- pico-vrchat-adapt: sideload helper for Steam Frame VRChat"
ui_print "- granting + spoofing happen at boot (post-fs-data.sh / service.sh)"

set_perm_recursive $MODDIR 0 0 0755 0644
set_perm $MODDIR/post-fs-data.sh 0 0 0755
set_perm $MODDIR/service.sh 0 0 0755
set_perm $MODDIR/config.sh 0 0 0755

ui_print "- installed. Reboot to apply."
ui_print "- Log: /sdcard/AdaptModule.log"
