#!/system/bin/sh
# ============================================================
#  revert.sh —— 回滚本模块写入的全部「持久状态」（幂等，可重复执行）
#
#  Magisk 只会回滚它自己管理的东西：systemless 挂载和 system.prop。
#  本模块在运行时改掉的状态，Magisk 不跟踪，必须在这里显式还原：
#
#    pm disable-user  -> /data/system/users/0/package-restrictions.xml
#    pm grant/revoke  -> /data/system/users/0/runtime-permissions.xml
#    settings put     -> 设置数据库（settings_global.xml / settings_secure.xml）
#    persist.*        -> /data/property/persistent_properties
#
#  为什么这份脚本必须存在（真实事故）：
#    旧版 module-sideload 的 customize.sh 漏了给 uninstall.sh 执行权限，
#    Magisk 卸载时那个回滚脚本根本没跑，残留的 vr_display_mode=1 与
#    被禁用的 com.pvr.verify 让头显卡在开机 logo。
#
#  三种调用方式：
#    1. Magisk 卸载模块时由 uninstall.sh 调用（系统已起来的情况）
#    2. 卸载时系统还没起来 -> uninstall.sh 会把它投放到
#       /data/adb/service.d/zz-adapt-revert.sh，由 Magisk 开机后补跑
#    3. 手动救砖（推荐先试这个）：
#         adb push revert.sh /data/local/tmp/
#         adb shell su -c "sh /data/local/tmp/revert.sh"
#         adb reboot
#
#  卡在开机 logo 时的终极手段：开机时长按音量下键进 Magisk 安全模式
#  （core-only，自动跳过所有模块），进系统后再执行本脚本。
# ============================================================

LOG=/data/local/tmp/adapt-revert.log
log() { echo "[adapt:revert] $*" >> "$LOG" 2>/dev/null; }

log "=== revert @ $(date) ==="

# ------------------------------------------------------------------
# 0) 等待系统起来
#    Magisk 可能在 post-fs-data 阶段调用本脚本，那时 system_server
#    还没启动，pm / settings 连不上，直接执行必然全部失败。
#    这里有限等待，不无限阻塞（post-fs-data 有超时保护）。
# ------------------------------------------------------------------
if [ "$(getprop sys.boot_completed)" != "1" ]; then
    i=0
    while [ "$(getprop sys.boot_completed)" != "1" ]; do
        sleep 2
        i=$((i + 1))
        if [ "$i" -gt 90 ]; then
            log "WARN: boot_completed still unset after 180s, trying anyway"
            break
        fi
    done
    sleep 2
fi

if ! command -v pm >/dev/null 2>&1; then
    log "ERROR: 'pm' not found -- cannot revert, run this script manually after boot"
    exit 1
fi

# ------------------------------------------------------------------
# 1) 重新启用被我们禁用的包
#    注意：如果你在装模块之前就自行禁用过其中某个包，这里也会启用回来。
# ------------------------------------------------------------------
for p in com.pico.ota com.pico.ota.sys com.pico.otacenter \
         com.pvr.verify com.pico.security.verifier com.pico.security; do
    out=$(pm enable --user 0 "$p" 2>&1)
    case "$out" in
        *"new state: enabled"*|*"enabled"*) log "enabled $p" ;;
        *) log "enable $p -> $out" ;;
    esac
done

# ------------------------------------------------------------------
# 2) 删除我们写入的设置项
#    vr_display_mode 是「卡在开机 logo」的头号嫌疑：它是持久全局设置，
#    残留后会让系统以异常的显示模式启动。
# ------------------------------------------------------------------
for kv in "global vr_display_mode" "secure vr_display_mode" \
          "global force_resizable_activities"; do
    settings delete $kv >/dev/null 2>&1
    cur=$(settings get $kv 2>/dev/null)
    case "$cur" in
        *null*|"") log "cleared settings $kv" ;;
        *) log "WARN: settings $kv still reads '$cur'" ;;
    esac
done

# ------------------------------------------------------------------
# 3) 复位 persist 属性
#    实测 resetprop --delete 在本机不生效，退回显式赋值 0。
# ------------------------------------------------------------------
if command -v resetprop >/dev/null 2>&1; then
    resetprop --delete persist.pvrpermission.autogrant >/dev/null 2>&1
fi
if [ "$(getprop persist.pvrpermission.autogrant)" = "1" ]; then
    setprop persist.pvrpermission.autogrant 0 >/dev/null 2>&1
    log "persist.pvrpermission.autogrant -> $(getprop persist.pvrpermission.autogrant)"
fi

# ------------------------------------------------------------------
# 4) 撤销我们授予的运行时权限
#    只有 dangerous 权限可撤销，其余（normal / signature）会报错，忽略即可。
# ------------------------------------------------------------------
PKGS="${PKGS:-com.vrchat.android}"
for PKG in $PKGS; do
    for perm in android.permission.RECORD_AUDIO \
                android.permission.READ_EXTERNAL_STORAGE \
                android.permission.WRITE_EXTERNAL_STORAGE; do
        pm revoke "$PKG" "$perm" >/dev/null 2>&1
    done
    log "revoked runtime permissions for $PKG"
done

# ------------------------------------------------------------------
# 5) 若自己是投放过来的补偿脚本，执行完自删
# ------------------------------------------------------------------
case "$0" in
    */service.d/*)
        rm -f "$0" 2>/dev/null
        log "removed fallback script $0"
        ;;
esac

# 6) 顺手清理可能残留的旧日志（仅模块自己的，不动 xr-profile-alias 的）
rm -f /data/local/tmp/.adapt-pending 2>/dev/null

log "=== revert done -- reboot to be safe ==="
echo "[adapt] revert completed. Log: $LOG"
echo "[adapt] Reboot the headset now (adb reboot)."
exit 0
