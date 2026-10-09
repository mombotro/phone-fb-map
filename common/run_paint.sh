# Runs on the device as root. Stops Android, runs paint on the framebuffer, always restores Android.
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
(sleep 80; killall -9 paint) >/dev/null 2>&1 &
stop
/data/local/tmp/paint ${1:-60} > /data/local/tmp/paint.out 2>&1
echo done >> /data/local/tmp/paint.out
