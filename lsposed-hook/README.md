# LSPosed hook (optional fallback)

Use only when the two Magisk modules still leave VRChat blocked by Pico's
signature verification (check the logs from `xr-profile-alias/tools/diagnose.sh`
first — in our testing this hook was **not needed**; the block happened at the
native OpenXR layer, out of reach of Java hooks).

## Principle

Pico's `com.pico.security.verifier.PicoSignatureVerifier` checks at VR app
launch whether the signature chains to a platform trust anchor (whitelist:
`/vendor/etc/pico-signing-whitelist.pem`). Third-party signatures (VRChat
self-signed) are not on it, and the OpenXR runtime then refuses to serve an XR
session. This hook makes the check return "pass" for `com.vrchat.android`.

## Build & use

1. Device needs **LSPosed** (Zygisk) installed; scope the module to
   **System Framework / the Pico VR runtime** packages.
2. Compile `src/com/wzy/picoadapt/PicoVrchatHook.java` with the Xposed API
   (`de.robv.android.xposed`) into an APK (Android Studio or your preferred
   build). No Gradle wrapper is shipped — this is a single-file reference hook.
3. `assets/xposed_init` already names the entry class:
   `com.wzy.picoadapt.PicoVrchatHook`.
4. Install the APK as an LSPosed module and reboot.

> Class/method names may need adjusting to match your own device's logs.
> `PicoSignatureVerifier.verify` and `OpenXRRuntime.isAppAllowed` are the usual
> candidates.
