# Runs on an Android phone as root at boot (started by /system/etc/install-recovery-2.sh): run the app picker as a kiosk.
# It waits until Android has finished booting, then runs the picker with no Exit tile (FB_KIOSK=1). If the picker ever
# stops, it starts again. The phone has no screen way out. adb still works.
# Safety: nothing starts if /data/local/tmp/fbboot_off exists. After 3 runs in a row that end within 20 s, it gives up
# and leaves Android alone, so a broken picker cannot lock the phone.
D=/data/local/tmp
[ -f $D/fbboot_off ] && exit 0
echo "boot job start $(date)" >> $D/boot.log
fails=0
while [ $fails -lt 3 ]; do
    [ -f $D/fbboot_off ] && break
    t0=$(date +%s)
    FB_KIOSK=1 sh $D/run_launcher.sh 31536000 fblauncher
    t1=$(date +%s)
    if [ $((t1 - t0)) -lt 20 ]; then fails=$((fails + 1)); else fails=0; fi
    sleep 2
done
echo "boot job end $(date) fails=$fails" >> $D/boot.log
