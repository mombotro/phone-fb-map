# Runs on the iPhone as root. Freezes SpringBoard, runs fbshell on the framebuffer, always respawns SpringBoard.
# Wait ~20s after any respring before running: stopping SpringBoard while it is still starting can wedge the display.
cleanup() { killall -9 SpringBoard; }
trap cleanup EXIT INT TERM HUP
(sleep 420; killall -9 fbshell) >/dev/null 2>&1 &
killall -STOP SpringBoard
/var/root/fbshell ${1:-300} ${2:-2} > /var/root/fbshell.out 2>&1
echo done >> /var/root/fbshell.out
