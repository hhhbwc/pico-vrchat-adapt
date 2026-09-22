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

它做的事（开机早期 `resetprop`）：把 OS 版本属性伪装进 5.5 宽松区间、打开侧载候选开关、关闭严格签名校验候选开关；开机完成后给 VRChat 显式授权（含 `org.khronos.openxr.permission.OPENXR_SYSTEM`）。

**会写持久状态的开关（`DISABLE_OTA` / `DISABLE_VERIFIER` / `FORCE_VR_DISPLAY` / `FORCE_RESIZABLE_ACTIVITIES`）默认全部关闭** —— 这类状态 Magisk 不回滚，残留会破坏系统启动（见「卡在开机 logo」一节）。需要时在模块内 `config.sh` 里显式打开，回滚逻辑见 `revert.sh`。

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

但 `module-sideload` 在运行期还会改几处**写在磁盘上、Magisk 不跟踪**的状态：

| 改动 | 落盘位置 | 回滚方式 |
|---|---|---|
| `pm disable-user`（OTA / 校验器） | `package-restrictions.xml` | `pm enable <pkg>` |
| `pm grant` | `runtime-permissions.xml` | `pm revoke <pkg> <perm>` |
| `settings put`（`vr_display_mode` 等） | 设置数据库 | `settings delete <key>` |
| `persist.pvrpermission.autogrant` | `persistent_properties` | `setprop ... 0` |

v1.4 起由 `revert.sh` 统一回滚（幂等、可重复执行），三条路径都走它：

1. 卸载时系统已在运行 → `uninstall.sh` 直接调用
2. 卸载发生在开机早期（Magisk 在 post-fs-data 阶段清算，此时 `pm`/`settings` 连不上 system_server）→ `uninstall.sh` 把 `revert.sh` 投放到 `/data/adb/service.d/zz-adapt-revert.sh`，Magisk 开机后补跑，跑完自删
3. 手动救砖 → 见下一节

> ⚠️ **v1.3 及更早版本有 bug**：`customize.sh` 漏给 `uninstall.sh` 设执行权限。Magisk 用 `execve` 调用卸载脚本，没有 x 位就完全没跑 —— 回滚从未生效。用旧版本卸载过的设备，请手动执行下面第 2 条。

**卡在开机 logo 了怎么办（重要）**

症状：装过 `module-sideload` 然后移除，重启后头显反复卡在 PICO 开机 logo，必须长按电源强制关机才停。

根因：旧版回滚脚本没生效，残留的持久状态（`com.pvr.verify` 被禁用、`vr_display_mode=1`）破坏了系统自身的启动流程。**与 `xr-profile-alias` 无关** —— 它是纯 systemless，没有持久状态，卸载即还原。

按顺序试：

1. **进 Magisk 安全模式**：强制关机，开机时**一直按住音量下键**直到进入系统（core-only，自动跳过所有模块）。
2. **手动回滚**（系统能起来时最直接）：
   ```sh
   adb push revert.sh /data/local/tmp/
   adb shell su -c "sh /data/local/tmp/revert.sh"
   adb reboot
   ```
   `revert.sh` 在仓库 `module-sideload/` 目录，每个 Release 的 zip 里也有。
3. **只清可疑项**（不想跑脚本时）：
   ```sh
   adb shell su -c "pm enable com.pvr.verify; settings delete global vr_display_mode; settings delete secure vr_display_mode; settings delete global force_resizable_activities; setprop persist.pvrpermission.autogrant 0"
   adb reboot
   ```
4. **确认已回到干净基线**：
   ```sh
   adb shell settings get global vr_display_mode   # 期望 null
   adb shell settings get secure vr_display_mode   # 期望 null
   adb shell pm list packages -d | grep -E "pvr.verify|pico.ota"   # 期望无输出
   adb shell getprop persist.pvrpermission.autogrant              # 期望 0
   ```

**如果还是被签名校验拦？**
`lsposed-hook/` 提供了 LSPosed 兜底方案（hook Pico 签名校验，对 VRChat 放行），见该目录 README。

## 已知限制

- 仅在 PICO 4 + PICO OS 5.13.7 + VRChat Steam Frame 构建（`2026.3.2p1`，仅 arm64-v8a）上验证；其他 OS 版本请先跑 `tools/diagnose.sh` 抓日志确认根因一致。
- 手柄模型依赖 v1.7 的反向别名（`xrGetCurrentInteractionProfile` 包装）；若你的 VRChat 版本模型仍不出现，开 `debug=1` 抓日志提 issue。
- `discover=1` 的探测逻辑借用 app 自己的 action 句柄，仅在 app 已完成 Suggest 调用后运行一次。

## 关于 `DISABLE_VERIFIER` 与 Virtual Desktop（重要）

`module-sideload` 的 `DISABLE_VERIFIER` **默认关闭（0）**，即不动 `com.pvr.verify`。VRChat 的放行主要靠属性伪装 + 权限授予，禁用校验器只是「以防万一」的兜底，而它的副作用比收益大：

部分侧载应用的平台校验（`VerifyApp`）需要这个服务**在线**才有响应。实测 **Virtual Desktop** 一旦校验器被禁用就会报 `VerifyApp call timed out`（表现为 `Unable to retrieve your computers`）—— **VD 需要的不是放行属性，恰恰是校验器本身开着**。

三种组合的实测结果：

| 校验器 | 放行属性 | Virtual Desktop | VRChat Steam Frame |
|---|---|---|---|
| 开 | 无 | 正常 | 进不了 VR（未装模块时的基线） |
| 开 | 有（v1.3+ 默认） | 正常 | 正常 |
| 关 | 有（v1.2 默认） | **超时报错** | 正常 |

建议：

- **保持默认 `DISABLE_VERIFIER=0`。** 只有当日志明确显示 VRChat 因校验器拦截而进不去 VR 时，才临时改成 `1` 排查。
- 曾用 `1` 跑过的话，改回 `0` 并重启即可恢复；`uninstall.sh` 也会在卸载时自动 `pm enable` 回来。用旧版本卸载过且 VD 异常的，手动执行一次：`pm enable com.pvr.verify` 后重启。

## License

[MIT](LICENSE) © wzy —— 源码自由使用。**不包含也不授权分发**任何 PICO OS / VRChat 专有二进制。
