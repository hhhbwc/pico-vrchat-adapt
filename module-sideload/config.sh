#!/system/bin/sh
# ============================================================
#  pico-vrchat-adapt —— 可编辑配置
#  修改后重启模块（或重启设备）生效。
# ============================================================

# 目标应用包名（空格分隔，支持多个）
# 当前只有一个包：Steam Frame 版 VRChat。
# 装过一体机版共存方案的话可以两行都留（授予失败只是记日志，无副作用）。
PKGS="com.vrchat.android"

# 伪装成的 Pico OS 版本（5.5 区间，策略最宽松）
# 部分 Pico 版本的 VR 运行时会读取系统属性来切换“严格/宽松”策略。
SPOOF_OS_VERSION="5.5.0"

# 是否禁用 OTA 更新（防止夜间自动升级回更封闭版本）。
# 默认关闭：禁用 OTA 包同样是持久状态（pm disable-user），虽然风险低于
# 校验器/显示模式，但本项目默认不写任何会影响系统启动的状态。
# 如果你希望 OS 保持当前版本（模块已验证的那个），把它改成 1。
DISABLE_OTA=0

# 是否尝试禁用 Pico 安全校验器（com.pvr.verify）
# 警告：默认关闭。启用会 pm disable-user com.pvr.verify，而 Virtual Desktop
# 依赖平台 VerifyApp 有服务响应——校验器被禁后 VD 会超时不可用。
# VD 需要的不是"放行属性"，而是校验器本身开着。
# 本模块的 uninstall.sh 会在卸载时自动重新启用校验器。
DISABLE_VERIFIER=0

# 是否伪装 ro.pvr.internal.version（含 sv5.13.7 标记的版本属性）。
# 风险较高（运行时会解析此字符串），默认关闭；仅当日志显示运行时按版本拒绝时开启。
SPOOF_INTERNAL=0

# ============================================================
#  遗留全局设置 —— 默认全部关闭，除非你明确知道自己在做什么
#
#  这两项写的是「全局设置数据库」，属于持久状态：Magisk 不回滚它们，
#  一旦残留会直接影响系统自身的启动流程。真实事故：残留的
#  vr_display_mode=1 让头显反复卡在开机 logo，删掉模块也没用，
#  必须手动清理 + 重启才恢复。
#
#  VRChat 的放行靠属性伪装 + 权限授予就够了，不需要它们。
# ============================================================

# settings put global/secure vr_display_mode 1
# 头号嫌疑：这是系统级显示模式开关，残留会导致开机卡 logo。
FORCE_VR_DISPLAY=0

# settings put global force_resizable_activities 1
# 与本项目目标无关的全局副作用，保留仅为兼容旧版本。
FORCE_RESIZABLE_ACTIVITIES=0

# 日志输出路径（需可写）
LOG="/sdcard/AdaptModule.log"
