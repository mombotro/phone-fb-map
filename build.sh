#!/bin/bash
# Builds every tool into build/. Needs: Xcode command line tools (clang), ldid (brew install ldid),
# an iOS SDK (git clone https://github.com/theos/sdks ; set IOS_SDK, default ~/sdks/iPhoneOS9.3.sdk;
# its usr/lib/libSystem.tbd must not re-export the missing liblaunch), and Android NDK r27.
set -e
cd "$(dirname "$0")"
SDK=${IOS_SDK:-$HOME/sdks/iPhoneOS9.3.sdk}
NDK=$(ls -d "$HOME"/Library/Android/sdk/ndk/27.* | head -1)
ACC=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/armv7a-linux-androideabi21-clang
mkdir -p build

# iOS 7 armv7, jailbroken. Entitlements are signed in with ldid. Private symbols resolve at run time.
ios() {
    local out rc
    out=$(clang -arch armv7 -O2 -miphoneos-version-min=7.0 -isysroot "$SDK" -Wl,-w -Wl,-undefined,dynamic_lookup \
          -framework CoreFoundation -o "build/$2_ios" "$1" 2>&1); rc=$?
    printf '%s\n' "$out" | grep -v '^ld: warning' || true
    [ $rc -eq 0 ] || { echo "BUILD FAILED: $1 (ios)" >&2; rm -f "build/$2_ios"; exit 1; }
    ldid -Scommon/hid.entitlements.plist "build/$2_ios"
}
# Android API 16+ : static, because NDK r27 cannot target API 16 dynamically. -fno-emulated-tls and the
# aligned TLS variable in plat_android.h keep bionic's static startup from rejecting the TLS segment.
android() { "$ACC" -O2 -static -fno-pie -fno-emulated-tls -o "build/$2_android" "$1" -lm; }

# The Longhouse chat app needs BearSSL (HTTPS) and stb_image (JPEG). common/fetch_bearssl.sh and common/fetch_stb_image.sh get the source.
common/fetch_stb_image.sh >/dev/null
for p in ios7 android; do [ -f build/bearssl/$p/libbearssl.a ] || common/build_bearssl.sh $p >/dev/null; done
BR=common/third_party/bearssl/inc
ios fbdemo.c fbdemo
for t in paint condemo fbshell calib fbreset notes timer bible; do ios common/$t.c $t; done
ios launcher/launcher.c launcher
LHOUT=$(clang -arch armv7 -O2 -miphoneos-version-min=7.0 -isysroot "$SDK" -Wl,-w -Wl,-undefined,dynamic_lookup -framework CoreFoundation \
        -I common -I $BR -o build/longhouse_ios common/longhouse.c build/bearssl/ios7/libbearssl.a 2>&1) || { printf '%s\n' "$LHOUT" | grep -v '^ld: warning'; echo "BUILD FAILED: longhouse (ios)" >&2; rm -f build/longhouse_ios; exit 1; }
ldid -Scommon/hid.entitlements.plist build/longhouse_ios
android android/fbdemo.c fbdemo
for t in paint condemo fbshell notes timer bible; do android common/$t.c $t; done
android launcher/launcher.c launcher
"$ACC" -O2 -static -fno-pie -fno-emulated-tls -I common -I $BR -o build/longhouse_android common/longhouse.c build/bearssl/android/libbearssl.a -lm || { echo "BUILD FAILED: longhouse (android)" >&2; rm -f build/longhouse_android; exit 1; }
ls -l build | awk 'NR>1 {print $5, $9}'
