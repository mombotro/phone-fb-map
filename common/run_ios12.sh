# Runs on the iPhone 5s (iOS 12) as root: run one program on the framebuffer with SpringBoard frozen.
# usage: run_ios12.sh PROGRAM [args...]       (the program path is relative to /var/root)
# SpringBoard keeps a copy of every touch, so it is frozen while the program runs (no taps reach the icons) and
# killed afterwards for a clean respring. A watchdog stops the program after FB_MAX seconds (default 3600).
# On iOS 12 a frozen SpringBoard lived for at least 4 minutes in a test (iOS 7 killed it after about 2 minutes).
# The watchdog kills THIS run only.
PROG=$1; shift
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
