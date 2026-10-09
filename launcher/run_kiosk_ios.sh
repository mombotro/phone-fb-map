# Runs on the iPhone as root. KIOSK MODE: start the app picker and keep it running, with no way out to the home screen.
# usage: run_kiosk_ios.sh [extra_seconds]     (default 4: a short margin after the FBMode tweak reports it is loaded)
#
# The switch is the file /var/mobile/fbkiosk_on (SpringBoard can read it, so the tweak knows from its first moment).
# Escape hatch: create the file /var/root/kiosk_off (over SSH: launcher/kiosk.sh off). The loop then ends and
# the boot job does nothing. Crash-loop guard: ten quick failures in a row (each under 10 s) turn kiosk off.
OFF=/var/root/kiosk_off
LOG=/var/root/kiosk.log
[ -f $OFF ] && exit 0

stamp() { echo "$(date +%H:%M:%S) $1" >> $LOG; }
stamp "boot script began"

# Only one copy of this loop may run. The tweak, the FB Tools icon and the boot job can all start it.
PIDF=/var/root/kiosk.pid
if [ -f $PIDF ] && kill -0 "$(cat $PIDF)" 2>/dev/null; then stamp "another kiosk loop is running: leaving"; exit 0; fi
echo $$ > $PIDF
trap 'rm -f $PIDF' EXIT

# Wait for the FBMode tweak. It creates /tmp/fbmode.log the moment it loads into SpringBoard, so this is a
# better trigger than a fixed sleep. Its own timer needs about 2 seconds after that to start watching the flag.
i=0
until [ -f /tmp/fbmode.log ]; do
    sleep 1; i=$((i + 1))
    [ $i -gt 240 ] && { stamp "the tweak never loaded: giving up"; exit 1; }
done
stamp "tweak loaded after ${i}s of waiting"
sleep ${1:-4}
[ -f $OFF ] && exit 0

stamp "starting the launcher"
quick=0
while [ ! -f $OFF ]; do
    t0=$(date +%s)
    FB_KIOSK=1 /var/root/fblauncher 31536000 >> $LOG 2>&1
    t1=$(date +%s)
    [ -f $OFF ] && break
    if [ $((t1 - t0)) -lt 10 ]; then quick=$((quick + 1)); else quick=0; fi
    if [ $quick -ge 10 ]; then
        stamp "crash loop: kiosk turned off"
        touch $OFF
        rm -f /var/mobile/fbkiosk_on
        break
    fi
    sleep 2
done
stamp "kiosk ended"
