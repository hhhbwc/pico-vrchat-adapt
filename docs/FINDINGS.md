# 真机取证结论 v2 —— 手柄失效的真正根因（2026-09-20 07:55 实测）

> 本篇推翻 v1（`ROOT_CAUSE.md` 中「`newly_installed_app` + Config Service 门禁 →
> `SetPhyControllerEnableKey(EnableKey 0)`」）的结论。v1 是误读，证据与更正如下。

## 一、决定性证据（VRChat 进程 pid 15901，Unity + APxrRuntime 日志）

```
07:55:26.045 W/xrt_session: client: com.vrchat.android, action=1, state=XR_SESSION_STATE_UNKNOWN
07:55:26.656 E/APxrRuntime(15901): (suggestedBindings->interactionProfile ==
        "/interaction_profiles/valve/frame_controller_valve") is not a supported interaction profile
07:55:26.656 I/Unity(15901): [XR] xrSuggestInteractionProfileBindings: XR_ERROR_PATH_UNSUPPORTED
07:55:26.790 I/Unity(15901): [XR] OpenXRSession::HandleSessionStateChangedEvent: ... -> XR_SESSION_STATE_FOCUSED
07:55:26.791 I/Unity(15901): [XR] Action Sets (1):  steamframecontroller: ActionCount=24
07:55:26.791 I/Unity(15901): [XR] [FAILURE] xrSuggestInteractionProfileBindings: XR_ERROR_PATH_UNSUPPORTED (1x)
07:55:50.355 E/Unity(15901): [SteamFrameControllerModel] Feature not ready after 15s. IsAvailable=False
```

**链条**：这个 APK 是 Steam Frame 专用构建，只有 **1 个 action set（`steamframecontroller`，24 个动作）**，
且只向运行时建议 **1 个交互 profile**：`/interaction_profiles/valve/frame_controller_valve`。

Pico 运行时**不认识**该 profile → 按 OpenXR 规范，`xrSuggestInteractionProfileBindings`
只要有一个 profile 不支持就整体返回 `XR_ERROR_PATH_UNSUPPORTED` 并**丢弃全部绑定**
→ VRChat 一个动作都没绑定 → 手柄完全无响应（头显 6DoF 不依赖 profile，所以正常）。

## 二、推翻 v1 的两条证据

1. 整段 07:55 测试日志（2.1 MB）中 `SetPhyControllerEnableKey` / `isEnable` **一次都没出现**；
   旧日志里那两条 `EnableKey 0`（07:22:02 / 07:22:13）发生在 VRChat 连接（07:22:29）**之前**，
   是给其他系统应用的 —— 「手柄门禁」不是 VRChat 的问题。
2. 《`newly_installed_app`》在 VRChat 进程内实测为 **0**（`oxr_session_create newly_installed_app: 0`），
   不存在「侧载被标记」的拦截。

## 三、双方 profile 支持面（本地提取，非猜测）

| 来源 | profile |
|---|---|
| VRChat APK（global-metadata.dat）| valve/frame_controller_valve、valve/index_controller、oculus/touch_controller、facebook/touch_controller_pro、htc/vive_controller、hp/mixed_reality_controller、microsoft/motion_controller、khr/simple_controller …（**无任何 Pico profile**）|
| Pico 运行时（libpxrruntime.so 内嵌表）| bytedance/pico4_controller、pico4s、pico_g3、pico/neo3、oculus/touch_controller、valve/index_controller、meta/touch_*、htc/*、microsoft/*、khr/simple_controller …（**无 valve/frame_controller_valve**）|

**交集**：`oculus/touch_controller`、`valve/index_controller`。

## 四、运行时 = Monado 架构（关键实现事实）

- `libpxrruntime.so` 内含 `monado_bytedance_pico4_controller`、
  `bytedance_pico4_controller_profile.json`、`OXR_DEBUG_BINDINGS`、`XRT_INPUT_PICO_*` 等符号
  → Pico 运行时是 **Monado 派生**，交互 profile = 内嵌 JSON 绑定表。
- profile 支持表的判定**硬编码在该 .so 内**（字符串
  `(suggestedBindings->interactionProfile == "%s") is not a supported interaction profile`）。
- 设备上**没有**任何外部 `*_profile.json`、没有 `bindings/` 目录、APK 里也没有 → **无纯数据级修法**。

## 五、加载链（决定补丁投递方式）

```
VRChat(自带 libopenxr_loader.so)
   └─ active_runtime.json → dlopen("/system/lib64/libopenxr_forwardloader.so")   ← 系统库，可被 Magisk 覆盖
        └─ JNI → com.pico.xr.openxr_runtime.DriverLoader（Java，运行在 App 进程内）
             └─ 加载 /system/priv-app/XRRuntime/XRRuntime.apk 内的 libpxrruntime.so  ← 从 APK 内 mmap，改不了（签名）
```

- 实测 `cat /proc/<vrchat>/maps`：`XRRuntime.apk` 以 `r-xp` 形式被 mmap 进 VRChat 进程；
  `/system/priv-app/XRRuntime/lib/arm64/` 为空（`extractNativeLibs=false`）。
- 因此**无法**直接替换 `libpxrruntime.so` 文件（改 APK 会破坏 Pico 平台签名，有让整套 XR 挂掉的风险）。
- 但 `/system/lib64` 在本机**已经是 Magisk tmpfs 覆盖层**（maps 里多个 `.pxr.so` 显示 fd:04，
  `libtrackingclient.pxr.so` 已是某模块替换过的文件）→ **替换 forwardloader 这条路是现成的**。

## 六、可行修法（按推荐度）

### 方案 A（推荐）：替换 `libopenxr_forwardloader.so` 为 shim
在 App 进程的 XR 加载链上做一层极薄的包装：拿到运行时的 `getInstanceProcAddr` 后，
把 `xrSuggestInteractionProfileBindings` 包一层，把 profile 路径
`valve/frame_controller_valve` **改写成 `bytedance/pico4_controller`（或 `oculus/touch_controller`）**，
其余函数原样转发。
- 效果：绑定注册成功 → 24 个标准路径（trigger/squeeze/thumbstick/a/b/system/thumbrest）
  由 Pico 手柄既有组件表满足 → 手柄可动、扳机可用、按键可用、震动可用。
- 不碰 VRChat APK；不向游戏注入业务逻辑（shim 属于系统 XR 加载链，App 本来就加载它）。
- 成本：需要 arm64 交叉编译工具链（zig 或 NDK）打一个 ~50 行的 C 库；
  Magisk 模块覆盖 `/system/lib64/libopenxr_forwardloader.so`，原件改名保留并 dlopen 转发。
- 风险可控：模块可随时停用；出问题只需重启。

### 方案 B：Zygisk 原生模块（在 App 进程内 hook 同一函数）
等效效果，但属于「向游戏进程注入」，用户此前明确不希望。

### 方案 C（零改动，若有条件）：改用 Quest/Android 版 VRChat
Quest 构建的绑定是 `oculus/touch_controller`，**Pico 运行时原生支持**，手柄直接可用，
无需任何 hook/补丁，封号风险为零。前提是能拿到该 APK。

## 七、已完成的副产物（仍然有效）

- `/data/adb/lsposed/cli` + Vector：模块 `com.workbuddy.picoadapt` 已可**成功注入
  `com.pico.xr.openxr_runtime`**（需杀进程重启，持久进程不会回溯注入）。
- 编译桩签名 bug 已修（`findAndHookMethod` 必须返回 `XC_MethodHook.Unhook`，
  否则 Vector LegacyBridge 报 `NoSuchMethodError ... )V`，全部钩子失效）。
  构建脚本：（构建脚本未随仓库发布，见 lsposed-hook/README.md。）
- 该模块**不影响**本次根因（profile 校验在 native 层，Java 钩子够不着），
  但它证明了「Vector 能注入 Pico 系统运行时进程」这一通路。
