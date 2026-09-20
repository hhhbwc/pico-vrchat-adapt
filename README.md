# pico-vrchat-adapt

让 **Steam Frame 版 VRChat**（`com.vrchat.android`）在 **PICO 4**（PICO OS 5.13.7 实测）上正常进 VR 并识别手柄。

**两个 Magisk 模块 + 一个可选 LSPosed 兜底**。不改 VRChat APK 的任何一个字节，不向游戏进程注入任何代码 —— 所有补丁都位于 Pico 自己的 OpenXR 加载链和系统属性层。停用/卸载模块即完全还原。

> ⚠️ 仅适用于**你自己已 root 的设备**，用于个人互操作性研究。请勿分发被修改的 VRChat 安装包。

---

## 背景：两层问题

| # | 现象 | 根因 | 解决模块 |
|---|------|------|----------|
| 1 | APK 装得上、启动卡死/黑屏，进不了 VR | PICO OS 5.8+ 收紧侧载 VR 应用的运行许可（OpenXR 会话不发放给未知来源应用） | `module-sideload` |
| 2 | 能进 VR（6DoF 正常），但手柄完全没反应、没有手柄模型 | Steam Frame 构建只建议 `/interaction_profiles/valve/frame_controller_valve` 这个交互 profile，而 Pico 运行时（Monado 派生）不认识它 —— 按 OpenXR 规范 `xrSuggestInteractionProfileBindings` 会**整体失败并丢弃全部绑定** | `xr-profile-alias` |

完整的取证与根因推导见 [docs/FINDINGS.md](docs/FINDINGS.md)（决定性日志证据）和 [docs/ROOT_CAUSE.md](docs/ROOT_CAUSE.md)(v1 分析 + 被推翻的过程记录)。

## 仓库结构

```
pico-vrchat-adapt/
├── module-sideload/        # 模块一：侧载放行 + OS 版本伪装 + 权限授予
├── xr-profile-alias/       # 模块二（核心）：OpenXR interaction-profile 别名 shim
│   ├── src/pico_xr_profile_alias.c
│   ├── build.sh            # NDK 交叉编译
│   ├── tools/
│   │   ├── extract_orig.sh # 从你自己的头显提取原版系统库（仓库不分发）
│   │   ├── make_module.py  # 组装 Magisk 模块 zip
│   │   └── diagnose.sh     # 一键抓取诊断日志
│   └── module/             # Magisk 模块模板（customize.sh / action.sh / conf …）
├── lsposed-hook/           # 可选兜底：LSPosed 放行签名校验（源码）
└── docs/                   # 根因分析全文
```

## 快速开始

### 0. 前置条件

- PICO 4 已 root，已装 Magisk（v26+），USB 调试开启
- 电脑可 `adb devices` 识别头显
- VRChat Steam Frame 版 APK 已 `adb install`（本仓库不提供 APK）
- Android NDK（r26+，用于编译模块二；`ANDROID_NDK_HOME` 指向 NDK 根目录）

### 1. 模块一：侧载放行（module-sideload）

```bash
adb push module-sideload /data/adb/modules/pico-vrchat-adapt
adb shell chmod -R 755 /data/adb/modules/pico-vrchat-adapt
adb reboot
```

它做的事（开机早期 `resetprop`）：把 OS 版本属性伪装进 5.5 宽松区间、打开侧载候选开关、关闭严格签名校验候选开关；开机完成后给 VRChat 显式授权（含 `org.khronos.openxr.permission.OPENXR_SYSTEM`），可选冻结 OTA / 安全校验器。所有开关见模块内 `config.sh`。

### 2. 模块二：手柄 profile 别名（xr-profile-alias）

```bash
cd xr-profile-alias

# 2.1 从你自己的头显提取原版系统库（版权原因仓库不含它，见下文"为什么没有预编译"）
bash tools/extract_orig.sh            # 需已 root；产物: lib/libopenxr_forwardloader_orig.so

# 2.2 编译 shim
bash build.sh                         # 产物: build/libopenxr_forwardloader.so

# 2.3 打包 Magisk 模块
python tools/make_module.py           # 产物: dist/pico_xr_profile_alias.zip
```

然后把 zip 传进头显，在 **Magisk App → 模块 → 从存储安装**，安装后**重启**。

### 3. 验证

```bash
adb logcat -s PicoXrAlias
```

启动 VRChat，关键日志：

```
---- shim init pid=... from="valve/frame_controller_valve" to="bytedance/pico4_controller"
shim v1.7 runtime GIPA wrapped (rewrite only on token match)
xrSuggestInteractionProfileBindings: profile="..." kept 37/48
```

`kept 37/48` 表示 48 条建议绑定修剪后保留了 37 条（其余是目标 profile 没有的组件）。戴上头显：扳机 / 握持 / 摇杆 / A·B·X·Y / 菜单键应有响应。

## 工作原理

### 加载链与补丁位置

```
VRChat（自带 libopenxr_loader.so）
   └─ active_runtime.json → dlopen("/system/lib64/libopenxr_forwardloader.so")   ← ★ Magisk 覆盖为 shim
        └─ shim: xrNegotiateLoaderRuntimeInterface → dlopen 原版(*_orig.so) → 拿到运行时 getInstanceProcAddr
             └─ 包装以下函数后交还 Khronos loader：
                  xrStringToPath                       改写 profile token（正向别名）
                  xrPathToString                       还原 app 视角的名字（rmap 反查）
                  xrSuggestInteractionProfileBindings  逐条探测、修剪目标 profile 没有的组件
                  xrGetCurrentInteractionProfile       反向别名：运行时上报的
                                                       bytedance/pico4_controller 改回
                                                       valve/frame_controller_valve
                                                       （否则 VRChat 不渲染手柄模型）
                  xrCreateInstance                     强制追加 XR_BD_controller_interaction
                                                       （Pico 自家 profile 被该扩展门控）
```

### 为什么安全

- **token 匹配才改写**：只处理字面包含 `valve/frame_controller_valve` 的路径，Pico 系统组件和其他应用的行为逐字节不变。
- **原版库原样保留**，shim 用 `android_dlopen_ext(fd)` 加载转发（绕开 `public.libraries.txt` 白名单限制）。
- **绝不向 `/system/etc/public.libraries.txt` 添加条目** —— zygote 会预加载该文件里的每一个库，缺一个就卡在开机 logo（这个坑已经替你踩过了）。
- 修改 APK / 注入游戏进程：都没有。

### 配置 `pico_xr_alias.conf`

安装后位于 `/system/etc/pico_xr_alias.conf`（模块自带），改完重启生效：

| 键 | 默认值 | 说明 |
|---|---|---|
| `from=` | `valve/frame_controller_valve` | app 建议的、运行时不认识的 profile token |
| `to=` | `bytedance/pico4_controller` | 替换目标，Pico 运行时实际支持的 profile（可选 `oculus/touch_controller`、`valve/index_controller` 等） |
| `map=` | `view→menu` 等 4 条 | 组件改名/换手规则（Steam Frame 与 Touch 命名不同） |
| `forceext=` | `1` | 向 `xrCreateInstance` 强制追加 `XR_BD_controller_interaction` |
| `discover=` | `0` | `1` 时探测目标 profile 的完整组件字典并打日志（写 `map=` 规则用） |
| `debug=` | `1` | 逐绑定探测结果日志 |

## 结论：与一体机版共存 —— 改包名路线**不可行**

一体机版与 Steam Frame 版**包名相同**（都是 `com.vrchat.android`），装一个覆盖另一个。直觉解法是「给其中一个改包名」，但这条路已被实测堵死：**VRChat 官方包自带防篡改校验**。

### 现象

改名重签后安装成功、能启动，但永远进不去游戏 —— 进程活着、CPU 归零、无任何崩溃弹窗：

```
I [CORE]  : server fused app token: 65b570d5-2be0-4ebd-8c96-b3a6bb3675d4
E libsigchain: xr_default_handler signal: 11, signo: 11     # 打印 token 后 5~10ms 内 SIGSEGV
```

`[CORE]` 是 `libloader.so` 里的混淆加载器。正常启动时 token 会打印两次并继续进 Unity；被篡改时打印一次就被 `xr_default_handler` 吞掉信号，主线程假死（**不会生成 tombstone，进程还在** —— 只看 `pidof` 会误判成「启动成功」）。

### 已排除的三条路线

| 尝试 | 结果 | 卡点 |
|---|---|---|
| apktool 改包名 + 重签 | ✗ 假死 | 校验锁定签名/包名身份 |
| 同长度原地补丁（除包名字符串外与原包逐字节一致、dex 头校验和重算、仅重签） | ✗ 仍假死 | 同上：**不是内容完整性问题** |
| Android 多用户（PICO 支持 4 用户） | ✗ `INSTALL_FAILED_UPDATE_INCOMPATIBLE` | 同名包跨用户要求签名一致 |

关键事实：两版都是各自渠道的**官方原包**，签名证书不同（Steam Frame 版 = VRChat 官方密钥 `BC:97:61…`；一体机版 = 另一渠道密钥 `7E:E1:9F…`），`[CORE]` 各自校验自己渠道的证书 —— 所以**重签哪一版都必死**，改名却是重签的必经之路。

### 如果仍要共存

只剩「绕过 `[CORE]` 校验」：运行时 hook（Zygisk/LSPosed 在应用读取自身签名时伪造回原证书）或 native patch `libloader.so`。两者都要先用 frida 动态追踪定位校验点（`libloader.so` 字符串全混淆、且未链接 `libcrypto`，静态看不出来），且 VRChat 每次更新都要重做。**本项目不提供、也不鼓励这条路** —— 它超出了「不改 APK、不注入游戏进程」的原则。

> 实践建议：二选一安装。本仓库的 shim 与属性伪装按 profile token / 系统属性工作，不依赖包名，对任一版都生效。

## 常见问题

**为什么仓库里没有 .so 和 .zip？**
模块需要把 Pico 的原版系统库 `libopenxr_forwardloader.so` 改名打包，这是 PICO OS 的版权文件，本仓库不分发 —— 请用 `tools/extract_orig.sh` 从**你自己的设备**提取。

**改了 conf 不生效？**
conf 在进程加载时读取一次，需要重启（或至少杀掉 VRChat 进程重开；`forceext` 相关状态随 zygote，保险起见整机重启）。

**日志文件是空的？**
VRChat 是 untrusted_app 域，SELinux 禁止它写 `/data/local/tmp/`。会话日志请用 `adb logcat -s PicoXrAlias`。

**进世界后联机超时？**
与本项目无关，是网络问题（Photon UDP 被墙）。任何支持 hysteria2 的代理客户端 + 对应节点即可解决，不在本仓库范围内。

**卸载 / 回滚？**
Magisk → 模块 → 移除 → 重启。systemless 挂载（`system/lib64/libopenxr_forwardloader.so`）与 `system.prop` 由 Magisk 自动回滚，`resetprop` 改的 `ro.*` 属性重启即恢复 —— 这些**无残留**。

但 `module-sideload` 在运行期还会改几处**写在磁盘上、Magisk 不跟踪**的状态，靠模块自带的 `uninstall.sh` 回滚（2026-09-21 起提供；更早版本没有这个脚本，卸载后状态会残留）：

| 改动 | 落盘位置 | 回滚方式 |
|---|---|---|
| `pm disable-user`（OTA / 校验器） | `package-restrictions.xml` | `pm enable <pkg>` |
| `pm grant` | `runtime-permissions.xml` | `pm revoke <pkg> <perm>` |
| `settings put`（`vr_display_mode` 等） | 设置数据库 | `settings delete global <key>` |
| `persist.pvrpermission.autogrant` | `persistent_properties` | `resetprop --delete <key>` |

> 想确认设备上还残留了什么：`adb shell pm list packages -d`（被禁用的包）、`adb shell settings list global | grep vr_`。

**如果还是被签名校验拦？**
`lsposed-hook/` 提供了 LSPosed 兜底方案（hook Pico 签名校验，对 VRChat 放行），见该目录 README。

## 已知限制

- 仅在 PICO 4 + PICO OS 5.13.7 + VRChat Steam Frame 构建（`2026.3.2p1`，仅 arm64-v8a）上验证；其他 OS 版本请先跑 `tools/diagnose.sh` 抓日志确认根因一致。
- 手柄模型依赖 v1.7 的反向别名（`xrGetCurrentInteractionProfile` 包装）；若你的 VRChat 版本模型仍不出现，开 `debug=1` 抓日志提 issue。
- `discover=1` 的探测逻辑借用 app 自己的 action 句柄，仅在 app 已完成 Suggest 调用后运行一次。

## 关于 `DISABLE_VERIFIER` 与 Virtual Desktop（重要）

`module-sideload` 默认会 `pm disable-user com.pvr.verify`（`DISABLE_VERIFIER=1`），这是**兜底**手段 —— VRChat 的放行主要靠属性伪装 + 权限授予，禁用校验器只是「以防万一」。

副作用：部分侧载应用的平台校验（`VerifyApp`）需要这个服务**在线**才有响应。实测 **Virtual Desktop** 在「校验器被禁用 + 放行属性缺失」时会报 `VerifyApp call timed out`（表现为 `Unable to retrieve your computers`）—— 典型发生在**卸载模块之后**，因为 `pm disable-user` 的状态是持久的。

三种组合的实测结果：

| 校验器 | 放行属性 | Virtual Desktop | VRChat Steam Frame |
|---|---|---|---|
| 开 | 无 | 正常 | 进不了 VR（未装模块时的基线） |
| 关 | 有（装模块） | 正常 | 正常 |
| 关 | 无（卸模块后、旧版无 uninstall.sh） | **超时报错** | 进不了 VR |

建议：

- 同时用 VD 的话，把 `config.sh` 里 `DISABLE_VERIFIER` 改成 `0`，只靠属性放行；若 VRChat 仍进不去 VR 再改回 `1`。
- v1.2 起自带 `uninstall.sh`，卸载会自动 `pm enable` 回来 —— 用旧版本卸载过的话，手动执行一次：`pm enable com.pvr.verify`。

## License

[MIT](LICENSE) © wzy —— 源码自由使用。**不包含也不授权分发**任何 PICO OS / VRChat 专有二进制。
