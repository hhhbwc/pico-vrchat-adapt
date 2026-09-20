#!/system/bin/sh
# Runs early at boot, after Magisk mounted the module files.
MODDIR=${0%/*}
LIBDIR="$MODDIR/system/lib64"

chmod 644 "$LIBDIR"/*.so 2>/dev/null

# The shim replaces an existing system library, so Magisk clones its
# system_lib_file context automatically.  The renamed original is new to
# /system and would keep Magisk's own label, which app processes cannot mmap;
# give it the very same label as any other library in /system/lib64.
chcon u:object_r:system_lib_file:s0 "$LIBDIR/libopenxr_forwardloader_orig.so" 2>/dev/null \
  || chcon u:object_r:system_file:s0 "$LIBDIR/libopenxr_forwardloader_orig.so" 2>/dev/null
chcon u:object_r:system_lib_file:s0 "$LIBDIR/libopenxr_forwardloader.so" 2>/dev/null
chcon u:object_r:system_file:s0 "$MODDIR/system/etc/pico_xr_alias.conf" 2>/dev/null

mkdir -p /data/local/tmp
: > /data/local/tmp/pico_xr_alias.log 2>/dev/null

exit 0
