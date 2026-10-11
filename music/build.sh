#!/bin/bash
# Builds the music player for iOS into ../build/musicplayer_ios. Needs third_party/ (run fetch_libs.sh).
set -e
cd "$(dirname "$0")"
../common/fetch_stb_truetype.sh >/dev/null   # draw.h includes the font code (ttf.h)

SDK=${IOS_SDK:-$HOME/sdks/iPhoneOS9.3.sdk}
O=../build/music_obj; mkdir -p "$O"
cc() { clang -arch armv7 -O2 -miphoneos-version-min=7.0 -isysroot "$SDK" "$@"; }
cc -w -c impl_flac.c   -o "$O/impl_flac.o"
cc -w -c impl_vorbis.c -o "$O/impl_vorbis.o"
cc -Wall -c decode.c   -o "$O/decode.o"
rm -f ../build/musicplayer_ios
out=$(cc -Wall -Wno-misleading-indentation -Wno-unused-function -Wl,-w -Wl,-undefined,dynamic_lookup -framework CoreFoundation \
   player.c "$O/decode.o" "$O/impl_flac.o" "$O/impl_vorbis.o" -o ../build/musicplayer_ios 2>&1) || { printf '%s\n' "$out" | grep -v '^ld: warning'; echo "BUILD FAILED: musicplayer" >&2; exit 1; }
printf '%s\n' "$out" | grep -v '^ld: warning' || true
ldid -S../common/hid.entitlements.plist ../build/musicplayer_ios
ls -l ../build/musicplayer_ios | awk '{print $5, $9}'
