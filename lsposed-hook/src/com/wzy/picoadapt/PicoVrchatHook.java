// ============================================================
//  PicoVrchatHook.java —— LSPosed(Zygisk) 钩子：让侧载 VRChat 拿到手柄输入
//
//  根因（真机日志 + XRRuntime.apk DEX 反编译确认）：
//    Pico OpenXR 运行时按应用计算 controllerable/focusable，
//    侧载应用永远被标记 newly_installed_app=1 且没有 Config Service
//    登记记录 → SetPhyControllerEnableKey(EnableKey 0, isEnable 0)
//    → 头显 6DoF 正常但手柄无输入。
//
//  真实钩子点（类名/签名均由 androguard 从 XRRuntime.apk 确认，非猜测）：
//    com.pico.picoopenxr.ipc.Client
//        boolean NativeCallClientIsNewlyInstalled(android.content.Context)
//        void    NativeCallClientSetNewlyInstalled(android.content.Context, boolean)
//        boolean checkAppHasToBETMetaData(android.content.Context)
//    com.pxr.pxrapi.PxrAppSplashManager
//        static boolean checkAppHasSplashMetaData(android.content.Context, String)
//    com.pvr.configuration.IConfigServiceInterface$Stub$a
//        String queryClientPropertyByPkg(String)  等查询方法
//
//  编译说明：刻意不直接引用 android.* 类型（用 findClass 运行期取 Class），
//  因此只需要 JDK + Xposed API jar 即可编译，不需要完整 Android SDK。
// ============================================================

package com.wzy.picoadapt;

import java.lang.reflect.Method;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XC_MethodReplacement;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

public class PicoVrchatHook implements IXposedHookLoadPackage {

    private static final String TAG = "picoadapt";
    private static final String TARGET = "com.vrchat.android";

    /**
     * 身份克隆源：这些是 Pico 系统自带的“已登记 VR 应用”。
     * 运行时向 Config Service 查询 VRChat 时，我们改查其中一个已登记包，
     * 再把返回 JSON 里的包名替换回 com.vrchat.android，
     * 运行时就会认为 VRChat 拥有控制器权限（has_permission=1 → controllerable=1）。
     * 取第一个能查到数据的包，之后缓存。
     */
    private static final String[] SOURCE_CANDIDATES = {
            "com.pvr.vrshell",       // Pico 桌面外壳（本机确认存在，必为已登记 VR 应用）
            "com.picoxr.xrshell",    // XR Shell
            "com.pvr.home",
            "com.picovr.home",
            "com.pico.mrservice",
            "com.pico.xr.openxr_runtime",
            "com.pvr.pvrlauncher",
            "com.pico.vrshell",
            "com.picovr.vrshell",
            "com.pvr.filebrowser",
    };

    private static volatile String sSourcePkg = null;

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam lpp) throws Throwable {
        final ClassLoader cl = lpp.classLoader;
        XposedBridge.log("[" + TAG + "] ===== loaded into pkg=" + lpp.packageName
                + " proc=" + lpp.processName + " firstApp=" + lpp.isFirstApplication + " =====");

        // ---------- 钩子 1：ipc.Client 的“新安装应用”与 ToBET 元数据 ----------
        // 这是日志里 newly_installed_app: 1 的直接来源，优先级最高。
        try {
            Class<?> client = XposedHelpers.findClass("com.pico.picoopenxr.ipc.Client", cl);
            Class<?> ctx = XposedHelpers.findClass("android.content.Context", cl);

            XposedHelpers.findAndHookMethod(client, "NativeCallClientIsNewlyInstalled",
                    ctx, new XC_MethodReplacement() {
                        @Override
                        protected Object replaceHookedMethod(MethodHookParam param) {
                            XposedBridge.log("[" + TAG + "] NativeCallClientIsNewlyInstalled -> false ("
                                    + Thread.currentThread().getName() + ")");
                            return Boolean.FALSE;
                        }
                    });

            XposedHelpers.findAndHookMethod(client, "NativeCallClientSetNewlyInstalled",
                    ctx, boolean.class, new XC_MethodReplacement() {
                        @Override
                        protected Object replaceHookedMethod(MethodHookParam param) {
                            XposedBridge.log("[" + TAG + "] NativeCallClientSetNewlyInstalled blocked");
                            return null; // 丢弃写入，保持未标记状态
                        }
                    });

            XposedHelpers.findAndHookMethod(client, "checkAppHasToBETMetaData",
                    ctx, new XC_MethodReplacement() {
                        @Override
                        protected Object replaceHookedMethod(MethodHookParam param) {
                            return Boolean.TRUE;
                        }
                    });

            XposedBridge.log("[" + TAG + "] hooked com.pico.picoopenxr.ipc.Client (newlyInstalled / ToBET)");
        } catch (Throwable t) {
            XposedBridge.log("[" + TAG + "] ipc.Client hook failed: " + t);
        }

        // ---------- 钩子 2：splash / ToB 元数据检查（static 方法） ----------
        try {
            Class<?> mgr = XposedHelpers.findClass("com.pxr.pxrapi.PxrAppSplashManager", cl);
            Class<?> ctx = XposedHelpers.findClass("android.content.Context", cl);

            XposedHelpers.findAndHookMethod(mgr, "checkAppHasSplashMetaData",
                    ctx, String.class, new XC_MethodReplacement() {
                        @Override
                        protected Object replaceHookedMethod(MethodHookParam param) throws Throwable {
                            Object pkg = param.args.length > 1 ? param.args[1] : null;
                            if (TARGET.equals(pkg) || pkg == null) {
                                return Boolean.TRUE;
                            }
                            return XposedBridge.invokeOriginalMethod(param.method, param.thisObject, param.args);
                        }
                    });
            XposedBridge.log("[" + TAG + "] hooked PxrAppSplashManager.checkAppHasSplashMetaData");
        } catch (Throwable t) {
            XposedBridge.log("[" + TAG + "] splash hook failed: " + t);
        }

        // ---------- 钩子 3：Config Service 查询 —— 身份克隆 ----------
        // VRChat 没有在 Pico Config Service 里登记，查询返回空 → has_permission=0。
        // 这里把“查 VRChat”重定向为“查已登记系统应用”，再把结果中的包名换回 VRChat。
        try {
            Class<?> stub = XposedHelpers.findClass(
                    "com.pvr.configuration.IConfigServiceInterface$Stub$a", cl);

            String[] queryMethods = {
                    "queryClientPropertyByPkg",
                    "queryPermissionData",
                    "queryPermissionDataByPermissionId",
                    "queryClientProperty",
                    "queryClientPropertyByClientId",
            };

            for (final String mn : queryMethods) {
                try {
                    XposedHelpers.findAndHookMethod(stub, mn, String.class,
                            new XC_MethodReplacement() {
                                @Override
                                protected Object replaceHookedMethod(MethodHookParam param) throws Throwable {
                                    Object arg0 = param.args.length > 0 ? param.args[0] : null;
                                    if (!TARGET.equals(arg0)) {
                                        return XposedBridge.invokeOriginalMethod(
                                                param.method, param.thisObject, param.args);
                                    }
                                    String src = resolveSourcePkg(param);
                                    if (src == null) {
                                        XposedBridge.log("[" + TAG + "] " + mn
                                                + ": no registered source pkg, passthrough");
                                        return XposedBridge.invokeOriginalMethod(
                                                param.method, param.thisObject, param.args);
                                    }
                                    Object res = XposedBridge.invokeOriginalMethod(
                                            param.method, param.thisObject, new Object[]{ src });
                                    if (res instanceof String) {
                                        String s = (String) res;
                                        if (!s.isEmpty() && s.contains(src)) {
                                            s = s.replace(src, TARGET);
                                        }
                                        XposedBridge.log("[" + TAG + "] " + mn + ": cloned from "
                                                + src + " -> " + abbreviate(s));
                                        return s;
                                    }
                                    return res;
                                }
                            });
                    XposedBridge.log("[" + TAG + "] hooked IConfigServiceInterface$Stub$a." + mn);
                } catch (Throwable t) {
                    XposedBridge.log("[" + TAG + "] hook " + mn + " failed: " + t);
                }
            }
        } catch (Throwable t) {
            XposedBridge.log("[" + TAG + "] ConfigService stub hook failed: " + t);
        }
    }

    /** 找一个能在 Config Service 里查到数据、且已登记的包名（找到后缓存）。 */
    private static String resolveSourcePkg(XC_MethodHook.MethodHookParam param) {
        String cached = sSourcePkg;
        if (cached != null) return cached;
        try {
            for (String cand : SOURCE_CANDIDATES) {
                try {
                    Object r = XposedBridge.invokeOriginalMethod(
                            param.method, param.thisObject, new Object[]{ cand });
                    if (r instanceof String && !((String) r).isEmpty()) {
                        sSourcePkg = cand;
                        XposedBridge.log("[" + TAG + "] identity source = " + cand);
                        return cand;
                    }
                } catch (Throwable ignore) {
                    // 该候选不存在/查询失败，继续下一个
                }
            }
        } catch (Throwable ignore) {
            // fallthrough
        }
        return null;
    }

    private static String abbreviate(String s) {
        if (s == null) return "null";
        return s.length() <= 200 ? s : s.substring(0, 200) + "...";
    }
}
