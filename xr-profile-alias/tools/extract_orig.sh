#!/usr/bin/env bash
# =============================================================================
#  extract_orig.sh -- pull the ORIGINAL libopenxr_forwardloader.so out of
#                     your own rooted PICO 4, as libopenxr_forwardloader_orig.so
#
#  The Magisk module overlays /system/lib64/libopenxr_forwardloader.so with the
#  shim; the renamed copy next to it is dlopen()ed at runtime.  The original
#  library is a proprietary PICO OS file, so this repository never ships it:
#  every user extracts it from their own device.
#
#  Usage: bash tools/extract_orig.sh [adb-serial]
#         ADB=/path/to/adb bash tools/extract_orig.sh
# =============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
LIBDIR="$HERE/../lib"
ADB_BIN="${ADB:-adb}"
SERIAL="${1:-}"

[ -n "$SERIAL" ] && ADB_BIN="$ADB_BIN -s $SERIAL"

"$ADB_BIN" get-state >/dev/null 2>&1 || {
  echo "ERROR: no adb device attached ($ADB_BIN)" >&2
  exit 1
}

mkdir -p "$LIBDIR"

# Copy to a world-readable staging path first: /system/lib64 itself is not
# readable by the shell user on all builds.
"$ADB_BIN" shell su -c \
  'cp /system/lib64/libopenxr_forwardloader.so /data/local/tmp/oxr_fl_orig.so && chmod 644 /data/local/tmp/oxr_fl_orig.so'

"$ADB_BIN" pull /data/local/tmp/oxr_fl_orig.so "$LIBDIR/libopenxr_forwardloader_orig.so"

"$ADB_BIN" shell su -c 'rm -f /data/local/tmp/oxr_fl_orig.so'

echo "OK -> $LIBDIR/libopenxr_forwardloader_orig.so"
echo "For personal interoperability use only -- do NOT redistribute this file."
