#!/bin/bash
# Builds the TIC-80 player for iOS and Android into ../build. Run build_core.sh first.
set -e
cd "$(dirname "$0")"
../common/fetch_stb_truetype.sh >/dev/null   # draw.h includes the font code (ttf.h)

SDK=${IOS_SDK:-$HOME/sdks/iPhoneOS9.3.sdk}
NDK=$(ls -d "$HOME"/Library/Android/sdk/ndk/27.* | head -1)
ACC=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/armv7a-linux-androideabi21-clang
mkdir -p ../build
out=$(clang -arch armv7 -O2 -miphoneos-version-min=7.0 -isysroot "$SDK" -Wl,-w -Wl,-undefined,dynamic_lookup \
      -framework CoreFoundation -I src/include -I src/src -Wall -Wno-misleading-indentation \
      player.c lib/libtic80core_ios.a -lc++ -o ../build/tic80_ios 2>&1) || { printf '%s\n' "$out" | grep -v '^ld: warning'; echo "BUILD FAILED: tic80 (ios)" >&2; rm -f ../build/tic80_ios; exit 1; }
printf '%s\n' "$out" | grep -v '^ld: warning' || true
ldid -S../common/hid.entitlements.plist ../build/tic80_ios
"$ACC" -O2 -static -fno-pie -fno-emulated-tls -I src/include -I src/src -Wall -Wno-misleading-indentation \
      player.c lib/libtic80core_android.a -lm -lc++_static -lc++abi -o ../build/tic80_android
ls -l ../build/tic80_ios ../build/tic80_android | awk '{print $5, $9}'
