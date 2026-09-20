#!/system/bin/sh
# ============================================================
#  uninstall.sh —— 模块被移除时由 Magisk 执行一次
#
#  Magisk 只会回滚它自己管理的东西：systemless 挂载（本模块没有）和
#  system.prop。凡是我们脚本在运行时改掉的系统状态，Magisk 不跟踪，
#  必须在这里显式回滚，否则删掉模块后状态依旧残留：
#
#    pm disable-user  -> /data/system/users/0/package-restrictions.xml
#    pm grant         -> /data/system/users/0/runtime-permissions.xml
#    settings put     -> 设置数据库（/data/system/users/0/settings_*.xml）
#    persist.*        -> /data/property/persistent_properties
#
#  resetprop 改的 ro.* 属性是运行时的，重启自动恢复，无需处理。
# ============================================================
MODDIR=${0%/*}
LOG=/sdcard/AdaptModule.log
[ -f "$MODDIR/config.sh" ] && . "$MODDIR/config.sh"

log() { echo "[adapt:uninstall] $*" >> "${LOG:-/sdcard/AdaptModule.log}" 2>/dev/null; }
log "=== revert @ $(date) ==="

# ---- 1) 重新启用我们可能禁用的包 ----
# 注意：如果你在装模块之前就自行禁用过其中某个包，这里也会把它启用回来。
for p in com.pico.ota com.pico.ota.sys com.pico.otacenter \
         com.pvr.verify com.pico.security.verifier com.pico.security; do
  pm enable "$p" >/dev/null 2>&1 && log "enabled $p"
done

# ---- 2) 撤掉我们写入的设置项 ----
settings delete global vr_display_mode >/dev/null 2>&1 && log "deleted global vr_display_mode"
settings delete secure vr_display_mode >/dev/null 2>&1 && log "deleted secure vr_display_mode"
settings delete global force_resizable_activities >/dev/null 2>&1 && log "deleted global force_resizable_activities"

# ---- 3) 删掉我们写入的 persist 属性 ----
resetprop --delete persist.pvrpermission.autogrant >/dev/null 2>&1 && log "deleted persist.pvrpermission.autogrant"

# ---- 4) 撤销我们授予的运行时权限（normal 权限无法撤销，忽略即可）----
for PKG in ${PKGS:-com.vrchat.android}; do
  for perm in android.permission.RECORD_AUDIO \
              android.permission.READ_EXTERNAL_STORAGE \
              android.permission.WRITE_EXTERNAL_STORAGE; do
    pm revoke "$PKG" "$perm" >/dev/null 2>&1 && log "revoked $PKG $perm"
  done
done

log "=== revert done ==="
exit 0
