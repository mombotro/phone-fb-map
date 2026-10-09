# Runs on the iPhone as root: freeze SpringBoard, run the player on the framebuffer, always respawn SpringBoard.
# usage: run_tic80_ios.sh <cart> [seconds]   (wait ~20s after any respring before running)
cleanup() { killall -9 SpringBoard; }
trap cleanup EXIT INT TERM HUP
(sleep $(( ${2:-60} + 40 )); killall -9 tic80) >/dev/null 2>&1 &
killall -STOP SpringBoard
/var/root/tic80 "$1" ${2:-60} > /var/root/tic80.out 2>&1
echo done >> /var/root/tic80.out
