#!/system/bin/sh
# ============================================================
#  post-fs-data.sh —— 开机早期执行（在 system_server 读取属性之前）
#  目的：在运行时策略生效前，把“OS 版本 / 侧载开关 / 签名校验”相关属性
#        伪装成 5.5 的宽松状态，让 VR 运行时对未知来源应用放行。
#  说明：Pico 各版本属性名不完全一致，这里对所有候选名做一次尝试，
#        仅当该属性“当前有值”时才覆盖，避免误伤无关属性。
# ============================================================
MODDIR=${0%/*}
. "$MODDIR/config.sh"

log() { echo "[adapt:post-fs-data] $*" >> "$LOG" 2>/dev/null; }
log "=== run @ $(date) ==="

# ---- 1) 伪装 Pico OS 版本（宽松策略区间）----
OS_PROPS="ro.pico.os.version ro.pico.build.version ro.pico.pui.version ro.product.pico.version ro.pico.os.display.version ro.pico.software.version"
for p in $OS_PROPS; do
  cur=$(getprop "$p" 2>/dev/null)
  if [ -n "$cur" ]; then
    resetprop "$p" "$SPOOF_OS_VERSION" 2>/dev/null
    log "spoof $p: $cur -> $SPOOF_OS_VERSION"
  fi
done

# ---- 2) 打开“允许未知来源 / 侧载”候选开关 ----
ALLOW_PROPS="ro.pico.unknownsource.allow ro.pico.vr.unknownsource ro.pico.side_load ro.pico.allow_sideload ro.pico.unknown_app.enabled"
for p in $ALLOW_PROPS; do
  resetprop "$p" "1" 2>/dev/null && log "set $p=1"
done

# ---- 3) 关闭“严格签名校验”候选开关 ----
SIG_PROPS="ro.pico.signature.strict ro.pico.verifier.enforce ro.pico.security.verifier.enabled ro.pico.verifier.strict"
for p in $SIG_PROPS; do
  resetprop "$p" "0" 2>/dev/null && log "set $p=0"
done

# ---- 4) 确保 PVR 权限自动授予（设备已有则无副作用）----
resetprop persist.pvrpermission.autogrant 1 2>/dev/null && log "set persist.pvrpermission.autogrant=1"

# ---- 5) 可选：伪装 ro.pvr.internal.version（含 sv5.13.7 标记）----
if [ "${SPOOF_INTERNAL:-0}" = "1" ]; then
  iv=$(getprop ro.pvr.internal.version 2>/dev/null)
  if [ -n "$iv" ]; then
    nv=$(echo "$iv" | sed 's/sv[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*_/sv5.5.0_/')
    resetprop ro.pvr.internal.version "$nv" 2>/dev/null && log "spoof internal version: $iv -> $nv"
  fi
fi

log "=== post-fs-data done ==="
