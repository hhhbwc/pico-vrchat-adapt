#!/usr/bin/env bash
# =============================================================================
#  build.sh -- cross-compile the OpenXR interaction-profile alias shim
#               (arm64-v8a, for PICO 4)
#
#  Output : build/libopenxr_forwardloader.so
#  Usage  : bash build.sh
#  Needs  : Android NDK r26+, ANDROID_NDK_HOME pointing at the NDK root.
#           Override the ABI or API level with NDK_ABI / NDK_API if needed.
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")"

NDK_ABI="${NDK_ABI:-arm64-v8a}"
NDK_API="${NDK_API:-26}"

if [ -z "${ANDROID_NDK_HOME:-}" ]; then
  echo "ERROR: ANDROID_NDK_HOME is not set." >&2
  echo "       Point it at your Android NDK root, e.g.:" >&2
  echo "         export ANDROID_NDK_HOME=/opt/android-ndk-r26d        (Linux/macOS)" >&2
  echo "         set ANDROID_NDK_HOME=C:/Android/Sdk/ndk/26.1.10909125 (Windows)" >&2
  exit 1
fi

HOST_TAG="$(uname -s | tr '[:upper:]' '[:lower:]')"
case "$HOST_TAG" in
  linux*)  HOST_TAG=linux-x86_64  ;;
  darwin*) HOST_TAG=darwin-x86_64 ;;
  msys*|mingw*|cygwin*) HOST_TAG=windows-x86_64 ;;
esac

TRIPLE="${NDK_ABI}"
[ "$NDK_ABI" = "arm64-v8a" ] && TRIPLE=aarch64-linux-android

CLANG="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin/${TRIPLE}${NDK_API}-clang"
[ -f "$CLANG" ]            || CLANG="$CLANG.cmd"
[ -f "$CLANG" ]            || { echo "ERROR: clang not found under $NDK" >&2; exit 1; }

mkdir -p build
echo "== clang: $CLANG"
"$CLANG" --version | head -1 || true

echo "== compile $NDK_ABI api=$NDK_API"
"$CLANG" \
  --target="${TRIPLE}-linux-android${NDK_API}" \
  -shared -fPIC -O2 -Wall \
  -fno-asynchronous-unwind-tables \
  -Wl,-soname,libopenxr_forwardloader.so \
  -Wl,-z,max-page-size=4096 \
  -o build/libopenxr_forwardloader.so \
  src/pico_xr_profile_alias.c

echo "== result"
python - <<'PY'
import struct
p = "build/libopenxr_forwardloader.so"
d = open(p, "rb").read()
print("size:", len(d), "bytes")
print("ELF magic:", d[:4], "machine:", hex(struct.unpack_from("<H", d, 18)[0]), "(0xB7 = AArch64)")
PY

echo "DONE -> $(pwd)/build/libopenxr_forwardloader.so"
