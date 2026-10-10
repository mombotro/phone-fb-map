# Runs on the iPhone 5s (iOS 12) as root: run one program on the framebuffer with SpringBoard frozen.
# usage: run_ios12.sh PROGRAM [args...]       (the program path is relative to /var/root)
# SpringBoard keeps a copy of every touch, so it is frozen while the program runs (no taps reach the icons) and
# killed afterwards for a clean respring. A watchdog stops the program after 100 s, because iOS may kill a
# SpringBoard that stays frozen for longer. The watchdog kills THIS run only.
PROG=$1; shift
cleanup() { killall -CONT SpringBoard 2>/dev/null; killall -9 SpringBoard 2>/dev/null; }
trap cleanup EXIT INT TERM HUP
killall -STOP SpringBoard
/var/root/$PROG "$@" > /var/root/$PROG.out 2>&1 &
PID=$!
( sleep 100; kill -9 $PID ) >/dev/null 2>&1 &
WD=$!
wait $PID
kill $WD >/dev/null 2>&1
echo done >> /var/root/$PROG.out
