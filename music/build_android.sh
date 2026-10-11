#!/bin/bash
# Builds the Android music player (../build/musicplayer_android) and the Java audio helper (android/helper.jar).
set -e
cd "$(dirname "$0")"
NDK=$(ls -d "$HOME"/Library/Android/sdk/ndk/27.* | head -1)
ACC=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/armv7a-linux-androideabi21-clang
rm -f ../build/musicplayer_android
"$ACC" -O2 -Wall -Wno-misleading-indentation -Wno-unused-function -static -fno-pie -fno-emulated-tls -o ../build/musicplayer_android player.c \
    || { echo "BUILD FAILED: musicplayer (android)" >&2; exit 1; }
./build_helper.sh
ls -l ../build/musicplayer_android | awk '{print $5, $9}'
