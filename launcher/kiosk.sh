#!/bin/bash
# Controls kiosk mode on the iPhone from the Mac. Needs the iproxy tunnel on port 2222.
# usage: launcher/kiosk.sh status | off | on | start-now | install-boot | remove-boot | log
#   off          turn kiosk off: stop the picker and every tool, clear the screen. The phone is normal again.
#   on           turn kiosk back on and start it now.
#   start-now    start the kiosk loop now (for a hand test, no boot job needed).
#   install-boot start kiosk at every boot.   remove-boot  stop starting it at boot.
SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cd "$(dirname "$SELF")"
SO=(-o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o StrictHostKeyChecking=no -o BatchMode=yes -o ConnectTimeout=10)
rs() { ssh -p 2222 "${SO[@]}" root@127.0.0.1 "$@"; }
TOOLS="fblauncher paint tic80 musicplayer fbshell calib condemo"
case "$1" in
  status)
    rs 'if [ -f /var/root/kiosk_off ]; then echo "kiosk switch: OFF"; else echo "kiosk switch: ON"; fi
        [ -f /var/mobile/fbkiosk_on ] && echo "SpringBoard marker: present" || echo "SpringBoard marker: absent"
        for n in fblauncher paint tic80 musicplayer fbshell calib condemo; do killall -0 $n >/dev/null 2>&1 && echo "running: $n"; done
        [ -f /Library/LaunchDaemons/com.local.fbkiosk.plist ] && echo "boot job: installed" || echo "boot job: not installed"' ;;
  off)
    rs "touch /var/root/kiosk_off; rm -f /var/mobile/fbkiosk_on /tmp/fbkiosk_wanted; for n in $TOOLS; do killall \$n >/dev/null 2>&1; done; sleep 2; /var/root/fbreset; echo 'kiosk is off: the phone is normal again'" ;;
  on)
    rs "rm -f /var/root/kiosk_off; touch /var/mobile/fbkiosk_on; chmod 644 /var/mobile/fbkiosk_on; echo 'kiosk switch is ON'"
    "$SELF" start-now ;;
  start-now)
    rs 'killall -0 fblauncher >/dev/null 2>&1 && { echo "already running"; exit 0; }; (sh /var/root/run_kiosk_ios.sh ${2:-3} </dev/null >/dev/null 2>&1 &); echo "kiosk starting" ' ;;
  install-boot)
    scp -P 2222 "${SO[@]}" com.local.fbkiosk.plist root@127.0.0.1:/Library/LaunchDaemons/com.local.fbkiosk.plist
    rs 'chown root:wheel /Library/LaunchDaemons/com.local.fbkiosk.plist; chmod 644 /Library/LaunchDaemons/com.local.fbkiosk.plist; echo "boot job installed. It starts at the next boot. kiosk switch is $( [ -f /var/root/kiosk_off ] && echo OFF || echo ON )"' ;;
  remove-boot)
    rs 'launchctl unload /Library/LaunchDaemons/com.local.fbkiosk.plist 2>/dev/null; rm -f /Library/LaunchDaemons/com.local.fbkiosk.plist; echo "boot job removed"' ;;
  log) rs 'cat /var/root/kiosk.log; echo "--- boot log"; cat /var/root/kiosk.boot.log 2>/dev/null' ;;
  *) sed -n '2,8p' "$0"; exit 2 ;;
esac
