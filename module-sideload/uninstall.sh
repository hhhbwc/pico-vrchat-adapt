#!/system/bin/sh
# ============================================================
#  uninstall.sh —— 模块被 Magisk 移除时执行一次
#
#  真正的回滚逻辑在 revert.sh（幂等，也可手动执行）。这里只做派发，
#  因为 Magisk 调用本脚本的时机不确定：
#
#    * 系统已经起来（magisk --remove-modules，或 App 里移除后系统在跑）
#      -> pm / settings 可用，直接执行 revert.sh
#    * post-fs-data 阶段（App 里标记移除后重启，Magisk 在开机早期清算）
#      -> system_server 还没起来，pm / settings 连不上，此刻直接回滚
#         必然全部失败。把 revert.sh 投放到 /data/adb/service.d/，
#         Magisk 会在系统启动完成后补跑一次。
#
#  两个坑（都真踩过）：
#    1. 日志不能写 /sdcard —— post-fs-data 阶段 /sdcard 尚未挂载，
#       写进去的日志会静默丢失，事后无从排查。
#    2. 本脚本必须带可执行位。旧版 customize.sh 漏了设权限，
#       Magisk 用 execve 直接执行，没有 x 位就完全没跑 ——
#       这正是一次「卸载后卡在开机 logo」事故的根因。
# ============================================================
MODDIR=${0%/*}
REVERT="$MODDIR/revert.sh"
FALLBACK=/data/adb/service.d/zz-adapt-revert.sh
LOG=/data/local/tmp/adapt-uninstall.log

log() { echo "[adapt:uninstall] $*" >> "$LOG" 2>/dev/null; }
log "=== uninstall @ $(date) ==="

if [ ! -f "$REVERT" ]; then
    log "ERROR: $REVERT missing, cannot revert persistent state"
    exit 1
fi

if [ "$(getprop sys.boot_completed)" = "1" ]; then
    log "boot completed -> reverting inline via revert.sh"
    sh "$REVERT" >> "$LOG" 2>&1
    log "inline revert finished, rc=$?"
else
    log "boot not completed -> system_server unreachable, deploying fallback"
    mkdir -p /data/adb/service.d 2>/dev/null
    if cp "$REVERT" "$FALLBACK" 2>/dev/null; then
        chmod 755 "$FALLBACK" 2>/dev/null
        log "deployed $FALLBACK (will run after boot, then self-delete)"
    else
        log "ERROR: failed to deploy $FALLBACK -- revert manually:"
        log "       adb shell su -c 'sh /data/local/tmp/revert.sh'"
    fi
fi

log "=== uninstall done ==="
exit 0
