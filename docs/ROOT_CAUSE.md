# 根因分析：VRChat Steam Frame 为何在 Pico OS 5.5 能跑、5.13.7 不能跑

> **编者注（2026-09-20）**：本文是排查过程的原始记录。第一部分（侧载被封禁 →
> `module-sideload` 的属性伪装方案）结论正确。第二部分「手柄不识别的根因」
> （`newly_installed_app` / `controllerable` 门禁 → LSPosed 修法）当时是误读，
> 已被 [FINDINGS.md](FINDINGS.md) 推翻 —— 真正根因是 interaction profile 不被
> 运行时支持，修法是 `xr-profile-alias` 的 native shim。保留全文以完整呈现
> 排查思路，请以 FINDINGS.md 为准。

## 结论（一句话）
**不是 APK 的问题，是 Pico OS 在 5.8 之后逐步封死了“未知来源（侧载）VR 应用”的运行许可。**
到 5.13.7，侧载的第三方 VR 应用无法绑定 Pico 的 OpenXR / VR 运行时，因此装得上、跑不了。

## 已用静态分析确认的事实（针对该 APK）
- 包名 `com.vrchat.android`，版本 `2026.3.2p1-...`，versionCode `939710`。
- `minSdk 29` / `targetSdk 34`，仅含 **arm64-v8a**（64 位），与 Pico 4 架构一致。
- 原生库完整（libil2cpp.so、libopenxr_loader.so、libsteam_api.so、libloader.so 等），
  内置 Pico / Quest(Oculus) / Steam-OpenXR 多平台支持。
- **签名合法**：META-INF 含 VRChat 证书（V1），APK Signing Block 含 **V2 + V3 + 0x42726577**，
  属标准合规签名，不存在“被改包/缺签名”问题。
- Manifest 声明了 `org.khronos.openxr.permission.OPENXR_SYSTEM` 等 VR 权限，
  并带 `pvr.app.type` / `pvr.sdk.version` 等 Pico VR 元数据。
- **APK 内没有任何“系统版本封禁”字符串/逻辑**（无 `UnsupportedPlatform`、`ro.pico.os.version`、
  “not supported on this OS version” 之类的固件/版本拦截提示）。

→ 因此封禁必然发生在 **Pico OS 侧**，而非应用内部。

## Pico OS 侧做了什么（社区/文档佐证）
1. **内核级签名校验**：`com.pico.security.verifier` 的 `PicoSignatureVerifier`
   在日志中报 `Failed: signature chain mismatch with platform trust anchor`，
   白名单位于 `/vendor/etc/pico-signing-whitelist.pem`。5.x（基于 Android 12L）只信任
   “平台信任锚”签名的包——第三方（VRChat 自签）不在此列。
2. **侧载路径收紧**：自 **5.8.x** 起，未知来源应用被 Launcher 隐藏、且 VR 运行时会拒绝为其
   提供 OpenXR 系统会话；社区说法“5.8.2+ 完全封死侧载路径”。
3. **账号/验证闸口**：约 5.8 推送的账号信息验证，导致不少侧载 VR 应用（含串流类）失效。
4. **VR 应用识别**：要真正进入 VR，应用须被系统识别为 VR 应用并拿到 OpenXR 系统权限——
   这正是侧载第三方包被卡的环节。

## 为什么 5.5 能、5.13.7 不能
- 5.5：侧载 VR 应用尚可被 VR 运行时放行、直接绑定 OpenXR → 正常进 VR。
- 5.13.7：运行时/校验器对侧载第三方包执行严格策略 → 无法取得 XR 会话，
  表现为启动卡死、黑屏、或退化为 2D 窗口、或直接被拦截。

## 适配思路（本模块所做的事）
- **伪装 OS 版本属性** 到 5.5 区间，让运行时套用宽松策略（开机早期 `resetprop`）。
- **打开侧载/未知来源开关、关闭严格签名校验开关**（候选属性名逐一尝试）。
- **显式授权** VRChat 的 OpenXR 等权限，并可选禁用 OTA 与安全校验器包。
- 若仍不够，叠加 **LSPosed Hook** 直接放行校验（见 `lsposed-hook/`）。

---

# 手柄不识别的根因（专项，基于 5.13.7 真机日志 + 运行时逆向）

## 现象
头显 6DoF 跟踪正常（能进 VR、画面跟随头部），但 **手柄无输入**——按键、摇杆、扳机、握持全部无响应。

## 关键日志证据（5.13.7 真机，`vrchat_test_verifier_ENABLED.txt`）
1. 会话创建时把 VRChat 标记为 **`newly_installed_app: 1`**
   (`oxr_session_create newly_installed_app: 1` / `oxr_session_begin set newly_installed_app: 1`)。
   侧载应用从不经过 Pico 商店安装，因此永远处于“新安装/未登记”状态。
2. 控制器使能键被关掉：
   `InputDeviceManager: EnableKey Enter SetPhyControllerEnableKey: EnableKey 0 isEnable:0`
   → 运行时主动把 VRChat 会话的物理控制器使能键设为 **禁用**。
3. 会话状态机其实走到了 FOCUSED：
   `oxr_session_manage_state, client: com.vrchat.android, action=4, state=XR_SESSION_STATE_FOCUSED`
   说明 **focus 本身有授予**，问题不在 focus，而在“控制器路由/使能”。

## 门禁位置（运行时原生 + Java 两层）
- 运行时原生库 `libpxrruntime_service.so` 为每个会话维护
  `Session state[focusable:%d, controllerable:%d, is_overlay:%d, ..., package_name:%s]`
  与 `ipc_app_state[..., focusable:%d, controllerable:%d, ...]`。
- `controllerable`（能否拿控制器输入）由以下输入决定：
  - **`newly_installed_app`**（侧载→1）；
  - **Config Service 权限 `has_permission`**：运行时通过 AIDL
    `IConfigServiceInterface` 查询 `queryClientPropertyByPkg(pkg)` /
    `queryPermissionDataByPermissionId(...)`——Pico 商店安装的应用会被写入这条“已登记 VR 应用”
    记录，侧载的 VRChat **没有**这条记录 → `has_permission=0`；
  - **Manifest 元数据 `has_app_splash`**：`PxrAppSplashManager.checkAppHasSplashMetaData` /
    `checkAppHasToBETMetaData`（查 `pvr.app.splash` 等 meta-data，侧载 APK 没有）。
- 三者任一不满足 → `controllerable=0` → `SetPhyControllerEnableKey(EnableKey=0, isEnable=0)`
  → 手柄输入不被路由到 VRChat 的 OpenXR action。

## 为什么“改属性”救不了手柄
全量扫描运行时所有 `.so` 与 `XRRuntime.apk` 的字符串，**没有任何单一系统属性可以翻转
“侧载应用的 controllerable/focusable”**。已有的控制器相关属性只有全局开关：
`pxr.picoController.disable`、`persist.pxr.disable.controllerservice`、
`persist.pxr.picoController.*`——打开它们只会把**整台设备**的控制器关掉，不能“只给 VRChat 开”。
→ 因此必须**在运行时进程内拦截判断逻辑**，这是 Magisk 属性层做不到的。

## 修法（不修改 APK，用模块）
门禁的“查 Config Service / 查 splash meta-data”逻辑在 **运行时的 Java 层**
（`com.pvr.configclientlibrary.*` 的 Config Service 客户端、`com.pxr.pxrapi.PxrAppSplashManager`、
`com.pico.api.app.AppSession`）。用 **LSPosed（Zygisk）Hook 这些 Java 方法**，对
`com.vrchat.android` 一律返回“已登记 / 已授权 / 有 splash”，即可让原生层算出
`has_permission=1` → `controllerable=1` → 手柄正常。APK 本身一字未改。

具体钩子点（类名/签名见 `lsposed-hook/PicoVrchatHook.java`，已由 DEX 反编译确认）：
- `queryClientPropertyByPkg(String)` / `queryPermissionDataByPermissionId(String)`
  （及其 `*Json` 变体）→ 对 `com.vrchat.android` 返回“已授权”数据；
- `checkAppHasSplashMetaData(String)` / `checkAppHasToBETMetaData(String)` → 对 VRChat 返回 `true`；
- 必要时 Hook `getTopAppPkgOnDefaultDisplay` / 焦点相关方法，确保 VRChat 被当作前台 VR 应用。

## 仍需你配合的“最后一步”
由于本机没有 Android SDK（只有 JDK 17，无 `android.jar`/`aapt`/`d8`/`apksigner`），
我无法在此环境把 LSPosed 模块编译成可装 APK。请提供以下任一项，我即可编译并真机安装测试：
- 本机 Android SDK 的 `build-tools` / `platforms/android-xx/android.jar` 路径；或
- 你本地 Android Studio 直接打开 `lsposed-hook/` 工程 `Build → Build Bundle(s)/APK`。
（Magisk 属性层模块已可直接 `adb push` + 重启生效，见 `install.bat`。）
