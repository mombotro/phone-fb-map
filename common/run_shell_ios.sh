# Runs on the iPhone as root: run fbshell on the framebuffer.
# usage: run_shell_ios.sh [seconds] [scale]
#
# LIVE mode (the FBMode tweak is installed): SpringBoard keeps running, and nothing can kill it. While the
# shell runs, the tweak hides every SpringBoard window, drops touches and gestures, and blocks auto-lock.
# fbshell draws on overlay layer 1. There is no time limit.
#
# FROZEN mode (no tweak): freeze SpringBoard and cap the run at 110 seconds. iOS kills a frozen SpringBoard
# after about 2 minutes, and a SpringBoard frozen before it settles is killed early, so wait until the last
# respring is 160 seconds old. Do not freeze backboardd (audio and the display need it) and do not unload
# SpringBoard (the phone then reboots).
TWEAK=/Library/MobileSubstrate/DynamicLibraries/FBMode.dylib

if [ -f $TWEAK ]; then
    # The watchdog kills THIS run's PID only, and is cancelled when the run ends. (An unscoped
    # "killall -9 fbshell" timer once outlived its run and killed the next one.)
    /var/root/fbshell ${1:-3600} ${2:-2} > /var/root/fbshell.out 2>&1 &
    PID=$!
    ( sleep $(( ${1:-3600} + 60 )); kill -9 $PID ) >/dev/null 2>&1 &
    WD=$!
    wait $PID
    kill $WD >/dev/null 2>&1
    echo done >> /var/root/fbshell.out
    exit 0
fi

wait_springboard() {
    last=$(cat /var/root/.last_respring 2>/dev/null || echo 0)
    while [ $(( $(date +%s) - last )) -lt 160 ]; do sleep 2; done
}
cleanup() { date +%s > /var/root/.last_respring; killall -9 SpringBoard; }

T=${1:-110}; [ "$T" -gt 110 ] && T=110
wait_springboard
trap cleanup EXIT INT TERM HUP
killall -STOP SpringBoard
/var/root/fbshell $T ${2:-2} > /var/root/fbshell.out 2>&1 &
PID=$!
( sleep $(( T + 20 )); kill -9 $PID ) >/dev/null 2>&1 &
WD=$!
wait $PID
kill $WD >/dev/null 2>&1
echo done >> /var/root/fbshell.out
