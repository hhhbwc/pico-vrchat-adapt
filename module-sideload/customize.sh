#!/system/bin/sh
# Magisk install script for the pico-vrchat-adapt sideload helper module.
MODDIR=${0%/*}

ui_print "- pico-vrchat-adapt: sideload helper for Steam Frame VRChat"
ui_print "- granting + spoofing happen at boot (post-fs-data.sh / service.sh)"

set_perm_recursive $MODDIR 0 0 0755 0644

# 这三个是开机脚本，必须可执行。
set_perm $MODDIR/post-fs-data.sh 0 0 0755
set_perm $MODDIR/service.sh     0 0 0755
set_perm $MODDIR/config.sh      0 0 0755

# 卸载/回滚脚本同样必须可执行：Magisk 是 execve 直接调用 uninstall.sh 的，
# 漏设权限的后果是卸载时回滚脚本静默不执行，持久状态全部残留
# （真实事故：残留的 vr_display_mode=1 导致头显卡在开机 logo）。
set_perm $MODDIR/uninstall.sh   0 0 0755
set_perm $MODDIR/revert.sh      0 0 0755

ui_print "- installed. Reboot to apply."
ui_print "- Log: /sdcard/AdaptModule.log"
ui_print "- Trouble booting? See revert.sh in the module folder."
