#!/bin/bash
# Turn "start on FB Tools" on or off for a rooted Android phone connected with adb.
# usage: launcher/android_boot.sh on|off|status
# The picker runs as a kiosk: no Exit tile, and it restarts if it stops. The only ways out are adb (run "off", or
# create /data/local/tmp/fbboot_off, then restart) or a hard reset with the power button.
# "on" copies the scripts and makes /system/etc/install-recovery-2.sh start the picker at boot. SuperSU's
# install-recovery.sh already calls that file, so no other system file changes. "off" removes the hook.
# To stop it without the Mac, create /data/local/tmp/fbboot_off. adb works at all times.
cd "$(dirname "$0")/.."
sush() { adb shell "su -c '$1'" | tr -d '\r'; }       # adb shell adds a carriage return to every line
case $1 in
  on)
    adb push common/run_launcher.sh /data/local/tmp/run_launcher.sh >/dev/null
    adb push launcher/run_boot_android.sh /data/local/tmp/run_boot.sh >/dev/null
    printf '#!/system/bin/sh\n# FB Tools: start the app picker at boot\nsh /data/local/tmp/run_boot.sh > /dev/null 2>&1 &\n' > /tmp/fbboot_hook.sh
    adb push /tmp/fbboot_hook.sh /data/local/tmp/fbboot_hook.sh >/dev/null
    # this phone's shell has no cp, and rm takes no -f: use cat and test
    sush '[ -f /data/local/tmp/fbboot_off ] && rm /data/local/tmp/fbboot_off; mount -o remount,rw /system; cat /data/local/tmp/fbboot_hook.sh > /system/etc/install-recovery-2.sh; chmod 755 /system/etc/install-recovery-2.sh; mount -o remount,ro /system'
    if sush 'ls /system/etc/install-recovery-2.sh' 2>&1 | grep -q "^/system/etc/install-recovery-2.sh$"; then
        echo "boot job ON. Restart the phone to try it."
    else echo "FAILED: the hook file is not in /system" >&2; exit 1; fi ;;
  off)
    sush 'mount -o remount,rw /system; [ -f /system/etc/install-recovery-2.sh ] && rm /system/etc/install-recovery-2.sh; mount -o remount,ro /system; echo off > /data/local/tmp/fbboot_off'
    if sush 'ls /system/etc/install-recovery-2.sh' 2>&1 | grep -q "No such file"; then echo "boot job OFF."; else echo "FAILED: the hook file is still there" >&2; exit 1; fi ;;
  status)
    sush 'ls -l /system/etc/install-recovery-2.sh /data/local/tmp/fbboot_off /data/local/tmp/boot.log 2>&1; cat /data/local/tmp/boot.log 2>/dev/null' ;;
  *) sed -n 2,6p "$0"; exit 1 ;;
esac
