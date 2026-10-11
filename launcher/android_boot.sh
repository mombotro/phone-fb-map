#!/bin/bash
# Turn "start on FB Tools" on or off for a rooted Android phone connected with adb. (The FB Tools tab in Phone Files does the
# same, and more.)  usage: launcher/android_boot.sh on|off|status
# "on" copies fbrun, and makes /system/etc/install-recovery-2.sh start the picker at boot as a kiosk (no Exit tile, and it
# starts again if it stops). SuperSU's install-recovery.sh calls that file on many ROMs, but not on all of them.
# The only ways out are adb ("off", or create /data/local/tmp/fbboot_off and restart) or a hard reset with the power button.
cd "$(dirname "$0")/.."
sush() { adb shell "su -c '$1'" | tr -d '\r'; }       # adb shell adds a carriage return to every line
case $1 in
  on)
    [ -f build/fbrun_android ] || { echo "build/fbrun_android is missing: run ./build.sh and the NDK build in tools/build_payload.sh" >&2; exit 1; }
    adb push build/fbrun_android /data/local/tmp/fbrun >/dev/null && sush 'chmod 755 /data/local/tmp/fbrun'
    printf '#!/system/bin/sh\n# FB Tools: start the app picker at boot\n/data/local/tmp/fbrun -d -k -b 31536000 fblauncher\n' > /tmp/fbboot_hook.sh
    adb push /tmp/fbboot_hook.sh /data/local/tmp/fbboot_hook.sh >/dev/null
    sush 'rm /data/local/tmp/fbboot_off; mount -o remount,rw /system; cat /data/local/tmp/fbboot_hook.sh > /system/etc/install-recovery-2.sh; chmod 755 /system/etc/install-recovery-2.sh; mount -o remount,ro /system'
    if sush 'ls /system/etc/install-recovery-2.sh' | grep -q "^/system/etc/install-recovery-2.sh$"; then
        echo "boot job ON. Restart the phone to try it."
    else echo "FAILED: the hook file is not in /system" >&2; exit 1; fi ;;
  off)
    sush 'mount -o remount,rw /system; rm /system/etc/install-recovery-2.sh; mount -o remount,ro /system; echo off > /data/local/tmp/fbboot_off'
    if sush 'ls /system/etc/install-recovery-2.sh' | grep -q "No such file"; then echo "boot job OFF."; else echo "FAILED: the hook file is still there" >&2; exit 1; fi ;;
  status)
    sush 'ls /system/etc/install-recovery-2.sh /data/local/tmp/fbboot_off /data/local/tmp/boot.log 2>&1; cat /data/local/tmp/boot.log 2>/dev/null' ;;
  *) sed -n 2,7p "$0"; exit 1 ;;
esac
