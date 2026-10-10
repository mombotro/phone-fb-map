# Runs on an Android device as root: stop Android, run the app picker on the framebuffer, always restore Android.
# usage: run_launcher.sh [seconds]
# Blank/unblank before start resets the MDP pipes so Android comes back onto a clean display.
cleanup() {
    echo 4 > /sys/class/graphics/fb0/blank 2>/dev/null
    sleep 1
    echo 0 > /sys/class/graphics/fb0/blank 2>/dev/null
    start
}
trap cleanup EXIT INT TERM HUP
# Never stop Android while it is still booting: killing SurfaceFlinger mid-init wedges the display.
wait_ready() {
    i=0
    while [ $i -lt 90 ]; do
        r=$(service check window 2>/dev/null)
        b=$(getprop init.svc.bootanim)
        case "$r" in *"not found"*) ;; *found*) [ "$b" = "stopped" ] && break;; esac
        sleep 1; i=$((i+1))
    done
    sleep 3
}
wait_ready
T=${1:-3600}
# The watchdog kills THIS run only, and is cancelled when the run ends.
stop
/data/local/tmp/fblauncher $T > /data/local/tmp/fblauncher.out 2>&1 &
PID=$!
( sleep $((T + 30)); kill -9 $PID ) >/dev/null 2>&1 &
WD=$!
wait $PID
kill $WD >/dev/null 2>&1
echo done >> /data/local/tmp/fblauncher.out
