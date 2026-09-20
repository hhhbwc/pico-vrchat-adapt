#!/usr/bin/env bash
# =============================================================================
#  diagnose.sh -- one-shot diagnostic capture for the PICO 4 + Steam Frame
#                 VRChat setup.  Useful before opening an issue.
#
#  Usage: bash tools/diagnose.sh [adb-serial]
#         ADB=/path/to/adb bash tools/diagnose.sh
# =============================================================================
set -euo pipefail

ADB_BIN="${ADB:-adb}"
SERIAL="${1:-}"
[ -n "$SERIAL" ] && ADB_BIN="$ADB_BIN -s $SERIAL"
OUT="vrchat_diagnose_$(date +%Y%m%d_%H%M%S).txt"

echo "== clearing logcat"
"$ADB_BIN" logcat -c

echo "== killing and relaunching VRChat"
"$ADB_BIN" shell am force-stop com.vrchat.android 2>/dev/null || true
sleep 2
"$ADB_BIN" shell monkey -p com.vrchat.android -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1 || true

echo "== capturing 45s of logs"
sleep 45

{
  echo "########## PicoXrAlias (shim) ##########"
  "$ADB_BIN" logcat -d -s PicoXrAlias:V
  echo
  echo "########## Unity (XR / bindings) ##########"
  "$ADB_BIN" logcat -d -s Unity:V | grep -iE "xr|profile|binding|interaction|controller" || true
  echo
  echo "########## XR runtime (APxrRuntime / xrt) ##########"
  "$ADB_BIN" logcat -d | grep -iE "APxrRuntime|xrt_session|interaction profile|supported interaction" | tail -100 || true
  echo
  echo "########## device info ##########"
  "$ADB_BIN" shell getprop | grep -iE "pico|pvr" || true
} > "$OUT"

echo "DONE -> $OUT"
echo "Attach this file when opening an issue."
