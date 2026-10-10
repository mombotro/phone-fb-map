#!/bin/bash
# Copies build/ios12/* to the iPhone 5s (iOS 12, Chimera) and signs them on the phone with its own ldid.
# usage: tools/deploy_ios12.sh [program...]     (default: all)    env: PORT (2223), PHONE_UDID (default: the connected iOS 12 device)
set -e
cd "$(dirname "$0")/.."
PORT=${PORT:-2223}; UDID=$PHONE_UDID
if [ -z "$UDID" ]; then          # find the connected iPhone that runs iOS 12 (an iPhone6,1 or another 64-bit model)
    for u in $(idevice_id -l); do
        [ "$(ideviceinfo -u "$u" -k ProductVersion 2>/dev/null | cut -d. -f1)" = 12 ] && UDID=$u
    done
fi
[ -n "$UDID" ] || { echo "no iOS 12 device found (set PHONE_UDID)" >&2; exit 1; }
SO=(-p "$PORT" -o StrictHostKeyChecking=no -o BatchMode=yes -o ConnectTimeout=8)
nc -z 127.0.0.1 "$PORT" 2>/dev/null || { (iproxy -u "$UDID" "$PORT:22" >/dev/null 2>&1 &); sleep 2; }
progs=("$@"); [ ${#progs[@]} -gt 0 ] || progs=($(ls build/ios12))
ssh "${SO[@]}" root@127.0.0.1 'cat > /var/root/ios12.ent' < common/ios12.entitlements.plist
for p in "${progs[@]}"; do
    ssh "${SO[@]}" root@127.0.0.1 "rm -f /var/root/$p; cat > /var/root/$p && chmod 755 /var/root/$p && ldid -S/var/root/ios12.ent /var/root/$p" < "build/ios12/$p"
    echo "deployed $p"
done
