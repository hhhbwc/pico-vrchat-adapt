#!/system/bin/sh
# ============================================================
#  pico-vrchat-adapt —— 可编辑配置
#  修改后重启模块（或重启设备）生效。
# ============================================================

# 目标应用包名（空格分隔，支持多个）
# com.vrchat.android   = 一体机原版 VRChat（未改动）
# com.vrchat.steamframe = Steam Frame 版改名共存包（见 README「与一体机版共存」）
# 只装其中一版时，删掉另一行即可。
PKGS="com.vrchat.steamframe com.vrchat.android"

# 伪装成的 Pico OS 版本（5.5 区间，策略最宽松）
# 部分 Pico 版本的 VR 运行时会读取系统属性来切换“严格/宽松”策略。
SPOOF_OS_VERSION="5.5.0"

# 是否禁用 OTA 更新（防止夜间自动升级回更封闭版本）
DISABLE_OTA=1

# 是否尝试禁用 Pico 安全校验器（com.pico.security.verifier）
DISABLE_VERIFIER=1

# 是否伪装 ro.pvr.internal.version（含 sv5.13.7 标记的版本属性）。
# 风险较高（运行时会解析此字符串），默认关闭；仅当日志显示运行时按版本拒绝时开启。
SPOOF_INTERNAL=0

# 日志输出路径（需可写）
LOG="/sdcard/AdaptModule.log"
