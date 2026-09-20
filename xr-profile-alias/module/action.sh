#!/system/bin/sh
# Magisk "Action" button: show current state and the tail of the shim log.
CONF=/system/etc/pico_xr_alias.conf
echo "=== pico_xr_profile_alias ==="
echo "--- $CONF"
cat "$CONF" 2>/dev/null
echo "--- mounted library"
ls -lZ /system/lib64/libopenxr_forwardloader.so /system/lib64/libopenxr_forwardloader_orig.so 2>/dev/null
echo "--- log tag"
logcat -d -s PicoXrAlias 2>/dev/null | tail -60
echo
echo "Change the alias target: edit 'to=' in $CONF, then reboot."
