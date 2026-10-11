# Runs on the iPhone 5s (iOS 12) as root: run one program on the framebuffer.
# usage: run_ios12.sh PROGRAM [args...]       (the program path is relative to /var/root)
#
# LIVE mode (the FBMode12 tweak is installed, see tweak/ios12): SpringBoard keeps running. While a program runs, the
# tweak drops SpringBoard's touches, keeps its windows hidden, and blocks auto-lock. The program draws on overlay layer 1.
# There is no time limit (FB_MAX seconds, default one day, is only a watchdog). SpringBoard is not touched.
#
# FROZEN mode (no tweak): SpringBoard is frozen while the program runs, and killed afterwards for a clean respring.
# iOS stops a frozen SpringBoard after about 2 minutes of touches, so this mode is the fallback only.
# Both watchdogs kill THIS run's PID only.
PROG=$1; shift
if [ -f /usr/lib/FBMode12.dylib ] && [ -f /usr/lib/TweakInject.dylib ] && [ ! -f /var/root/.disable_fbmode ]; then
    /var/root/$PROG "$@" > /var/root/$PROG.out 2>&1 &
    PID=$!
    ( sleep ${FB_MAX:-86400}; kill -9 $PID ) >/dev/null 2>&1 &
    WD=$!
    wait $PID
    kill $WD >/dev/null 2>&1
    echo done >> /var/root/$PROG.out
    exit 0
fi
cleanup() { killall -CONT SpringBoard 2>/dev/null; killall -9 SpringBoard 2>/dev/null; }
trap cleanup EXIT INT TERM HUP
killall -STOP SpringBoard
/var/root/$PROG "$@" > /var/root/$PROG.out 2>&1 &
PID=$!
( sleep ${FB_MAX:-3600}; kill -9 $PID ) >/dev/null 2>&1 &
WD=$!
wait $PID
kill $WD >/dev/null 2>&1
echo done >> /var/root/$PROG.out
