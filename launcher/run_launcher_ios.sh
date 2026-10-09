# Runs on the iPhone as root: run the app picker on the framebuffer. usage: run_launcher_ios.sh [seconds]
#
# The launcher starts the other tools itself and needs live mode, so the FBMode tweak must be installed
# (see docs/DEVICE-MAP.md). The watchdog kills only this run's PID and is cancelled when the run ends.
TWEAK=/Library/MobileSubstrate/DynamicLibraries/FBMode.dylib
if [ ! -f $TWEAK ]; then
    echo "The launcher needs the FBMode tweak. Install it first." > /var/root/fblauncher.out
    exit 1
fi
/var/root/fblauncher ${1:-86400} > /var/root/fblauncher.out 2>&1 &
PID=$!
( sleep $(( ${1:-86400} + 60 )); kill -9 $PID ) >/dev/null 2>&1 &
WD=$!
wait $PID
kill $WD >/dev/null 2>&1
echo done >> /var/root/fblauncher.out
