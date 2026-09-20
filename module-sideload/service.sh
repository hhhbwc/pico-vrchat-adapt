#!/system/bin/sh
# ============================================================
#  service.sh —— 开机完成后执行
#  目的：对 VRChat 显式授权（含 OpenXR 系统权限）、标记为可在 VR 中运行、
#        可选禁用 OTA / 安全校验器包，并落盘实际属性便于排错。
# ============================================================
MODDIR=${0%/*}
. "$MODDIR/config.sh"

log() { echo "[adapt:service] $*" >> "$LOG" 2>/dev/null; }

# 等待开机完成
until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done
sleep 5
log "=== run @ $(date) ==="

# ---- 1) 显式授予关键权限（含 OpenXR 系统级权限）----
PERMS="org.khronos.openxr.permission.OPENXR_SYSTEM
org.khronos.openxr.permission.OPENXR
android.permission.READ_PHONE_STATE
android.permission.READ_EXTERNAL_STORAGE
android.permission.WRITE_EXTERNAL_STORAGE
android.permission.BLUETOOTH
android.permission.BLUETOOTH_CONNECT
android.permission.RECORD_AUDIO
android.permission.WAKE_LOCK
android.permission.INTERNET
android.permission.ACCESS_WIFI_STATE
android.permission.ACCESS_NETWORK_STATE"

for perm in $PERMS; do
  out=$(pm grant "$PKG" "$perm" 2>&1)
  if [ -n "$out" ]; then log "grant $perm -> $out"; fi
done

# ---- 2) 让未知来源应用也能作为 VR 应用被识别（best-effort）----
settings put global vr_display_mode 1 >/dev/null 2>&1
settings put secure vr_display_mode 1 >/dev/null 2>&1
settings put global force_resizable_activities 1 >/dev/null 2>&1

# ---- 3) 可选：禁用 OTA 更新（防止夜间自动升级回更封闭版本）----
if [ "$DISABLE_OTA" = "1" ]; then
  for o in com.pico.ota com.pico.ota.sys com.pico.otacenter; do
    pm disable-user --user 0 "$o" >/dev/null 2>&1 && log "disabled $o"
  done
fi

# ---- 4) 可选：禁用 Pico 安全校验器包（5.13.7 实测包名为 com.pvr.verify）----
if [ "$DISABLE_VERIFIER" = "1" ]; then
  for v in com.pvr.verify com.pico.security.verifier com.pico.security; do
    pm disable-user --user 0 "$v" >/dev/null 2>&1 && log "disabled $v"
  done
fi

# ---- 5) 落盘实际属性，便于排错 ----
log "--- current pico props ---"
getprop 2>/dev/null | grep -i pico >> "$LOG" 2>/dev/null
log "=== service done for $PKG ==="
