#!/bin/bash
# Builds FBTools.app (the home-screen icon for the app picker) and installs it on the iPhone, then respring.
# usage: launcher/install_icon_ios.sh      (needs the iproxy tunnel on port 2222 and the launcher already deployed)
set -e
cd "$(dirname "$0")"
SDK=${IOS_SDK:-$HOME/sdks/iPhoneOS9.3.sdk}
W=$(mktemp -d)/FBTools.app; mkdir -p "$W"
clang -arch armv7 -O2 -miphoneos-version-min=7.0 -isysroot "$SDK" -Wl,-w -o "$W/FBTools" fbtools_stub.c
ldid -S "$W/FBTools"
cp Info.plist "$W/Info.plist"
python3 make_icon.py "$W"
SO=(-o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o StrictHostKeyChecking=no -o BatchMode=yes -o ConnectTimeout=10)
ssh -p 2222 "${SO[@]}" root@127.0.0.1 'rm -rf /var/root/FBTools.app'
# the phone has no sftp-server, so stream the bundle with tar (new scp cannot copy directories to it)
COPYFILE_DISABLE=1 tar -C "$(dirname "$W")" -cf - FBTools.app | ssh -p 2222 "${SO[@]}" root@127.0.0.1 'tar -C /var/root -xf -'
ssh -p 2222 "${SO[@]}" root@127.0.0.1 '
    rm -rf /Applications/FBTools.app
    cp -R /var/root/FBTools.app /Applications/FBTools.app
    chown -R root:wheel /Applications/FBTools.app
    chmod 755 /Applications/FBTools.app
    chmod 4755 /Applications/FBTools.app/FBTools
    # uicache must run as the mobile user. As root it fails with "cannot open cache file".
    su mobile -c uicache
    echo "installed; respringing so the icon appears"
    killall -9 SpringBoard'
