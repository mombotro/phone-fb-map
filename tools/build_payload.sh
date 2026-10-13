#!/bin/bash
# Collects everything the Phone Files app can install on a phone into tools/payload/ (and builds what is missing).
# Run it before tools/build_phonefiles_app.sh. Needs the same tools as the builds it calls.
set -e
cd "$(dirname "$0")/.."
SDK=${IOS_SDK:-$HOME/sdks/iPhoneOS9.3.sdk}
echo "== building programs"
./build.sh >/dev/null
tools/build_ios12.sh >/dev/null
music/build.sh >/dev/null 2>&1 || echo "  (iPhone music player not rebuilt: music/third_party missing?)"
music/build_android.sh >/dev/null
launcher/android_apk/build.sh >/dev/null
NDK=$(ls -d "$HOME"/Library/Android/sdk/ndk/27.* | head -1)
CC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/armv7a-linux-androideabi21-clang"
$CC -O2 -static -fno-pie -fno-emulated-tls -o build/fbls_android tools/fbls.c
$CC -O2 -static -fno-pie -fno-emulated-tls -o build/fbtime_android tools/fbtime.c
$CC -O2 -Wall -static -fno-pie -fno-emulated-tls -o build/fbrun_android launcher/fbrun.c

P=tools/payload; rm -rf $P; mkdir -p $P/ios7/bin $P/ios7/FBTools.app $P/ios12/bin $P/ios12/FBTools.app $P/android/bin
echo "== iPhone 4 (iOS 7)"
for f in fblauncher:launcher paint:paint fbshell:fbshell notes:notes timer:timer bible:bible reader:reader pixel:pixel calendar:calendar longhouse:longhouse musicplayer:musicplayer tic80:tic80 calib:calib fbreset:fbreset; do
    src=build/${f##*:}_ios; [ -f "$src" ] && cp "$src" $P/ios7/bin/${f%%:*} || echo "  missing ${f##*:}_ios"
done
cp launcher/run_launcher_ios.sh launcher/run_kiosk_ios.sh launcher/com.local.fbkiosk.plist $P/ios7/
cp tweak/FBMode.plist $P/ios7/ && [ -f tweak/FBMode.dylib ] && cp tweak/FBMode.dylib $P/ios7/ || echo "  missing tweak/FBMode.dylib (build it as shown in docs/DEVICE-MAP.md)"
clang -arch armv7 -O2 -miphoneos-version-min=7.0 -isysroot "$SDK" -Wl,-w -o $P/ios7/FBTools.app/FBTools launcher/fbtools_stub.c && ldid -S $P/ios7/FBTools.app/FBTools
cp launcher/Info.plist $P/ios7/FBTools.app/Info.plist; python3 launcher/make_icon.py $P/ios7/FBTools.app
echo "== iPhone 5s (iOS 12)"
for f in build/ios12/*; do cp "$f" $P/ios12/bin/; done
cp common/ios12.entitlements.plist common/run_ios12.sh $P/ios12/
clang -target arm64-apple-ios9.0 -O2 -DIOS12 -isysroot "$SDK" -Wl,-w -o $P/ios12/FBTools.app/FBTools launcher/fbtools_stub.c
cp launcher/Info.plist $P/ios12/FBTools.app/Info.plist; python3 launcher/make_icon.py $P/ios12/FBTools.app
echo "== iPhone 5s tweak (our own loader and the SpringBoard tweak)"
mkdir -p $P/ios12/lib
clang -target arm64-apple-ios9.0 -O2 -dynamiclib -isysroot "$SDK" -Wl,-w -Wl,-undefined,dynamic_lookup -Wl,-not_for_dyld_shared_cache \
    -install_name /usr/lib/TweakInject.dylib -o $P/ios12/lib/TweakInject.dylib tweak/ios12/TweakInject.c
clang -target arm64-apple-ios9.0 -O2 -dynamiclib -isysroot "$SDK" -Wl,-w -Wl,-undefined,dynamic_lookup -Wl,-not_for_dyld_shared_cache \
    -install_name /usr/lib/FBMode12.dylib -o $P/ios12/lib/FBMode12.dylib -x objective-c tweak/ios12/FBMode12.m -x none build/stubs/CoreFoundation.tbd
echo "== Android"
for f in fblauncher:launcher paint:paint fbshell:fbshell notes:notes timer:timer bible:bible reader:reader pixel:pixel calendar:calendar longhouse:longhouse musicplayer:musicplayer tic80:tic80 condemo:condemo; do
    cp build/${f##*:}_android $P/android/bin/${f%%:*}
done
cp build/fbls_android $P/android/bin/fbls; cp build/fbtime_android $P/android/bin/fbtime; cp build/fbrun_android $P/android/bin/fbrun
cp music/android/helper.jar $P/android/musichelper.jar
cp launcher/android_apk/FBTools.apk $P/android/FBTools.apk
if [ -f tools/bible_asv.txt ]; then      # the Bible text, made by tools/make_bible.py from the asv folder
    for d in ios7 ios12 android; do mkdir -p $P/$d/data/bible; cp tools/bible_asv.txt $P/$d/data/bible/asv.txt; done
else echo "  no tools/bible_asv.txt: run tools/make_bible.py to add the Bible"; fi
if [ -f tools/emoji/NotoEmoji.ttf ]; then for d in ios7 ios12 android; do mkdir -p $P/$d/data/longhouse; cp tools/emoji/NotoEmoji.ttf $P/$d/data/longhouse/emoji.ttf; done     # the emoji font of the Longhouse app
else echo "  no tools/emoji/NotoEmoji.ttf: run tools/fetch_emoji_font.sh so that every emoji shows in Longhouse"; fi
if [ -f tools/bible_net.txt ]; then for d in ios7 ios12 android; do mkdir -p $P/$d/data/bible; cp tools/bible_net.txt $P/$d/data/bible/net.txt; done; fi       # the NET Bible, from tools/fetch_net_bible.py
if [ -d tools/fonts ] && ls tools/fonts/* >/dev/null 2>&1; then      # the reading fonts, from tools/fetch_fonts.sh
    for d in ios7 ios12 android; do mkdir -p $P/$d/data/fonts; cp tools/fonts/* $P/$d/data/fonts/; done
else echo "  no tools/fonts: run tools/fetch_fonts.sh to add the reading fonts"; fi
for d in ios7 android; do mkdir -p $P/$d/carts; cp tic80/carts/*.lua $P/$d/carts/; done       # the sample TIC-80 carts
echo "payload ready: $(find $P -type f | wc -l | tr -d ' ') files"
