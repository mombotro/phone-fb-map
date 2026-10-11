#!/bin/bash
# Builds BearSSL (common/third_party/bearssl, see fetch_bearssl.sh) as a static library for one platform:
#   usage: common/build_bearssl.sh host|ios7|ios12|android      output: build/bearssl/<platform>/libbearssl.a
set -e
cd "$(dirname "$0")/.."
P=${1:?usage: build_bearssl.sh host|ios7|ios12|android}
SRC=common/third_party/bearssl
[ -d $SRC/src ] || common/fetch_bearssl.sh >/dev/null
SDK=${IOS_SDK:-$HOME/sdks/iPhoneOS9.3.sdk}
case $P in
  host)    CC=(clang -O2) ; AR=(ar rcs) ;;
  ios7)    CC=(clang -arch armv7 -O2 -miphoneos-version-min=7.0 -isysroot "$SDK") ; AR=(ar rcs) ;;
  ios12)   CC=(clang -target arm64-apple-ios9.0 -O2 -isysroot "$SDK") ; AR=(ar rcs) ;;
  android) NDK=$(ls -d "$HOME"/Library/Android/sdk/ndk/27.* | head -1); T=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin
           CC=("$T/armv7a-linux-androideabi21-clang" -O2 -fno-pie) ; AR=("$T/llvm-ar" rcs) ;;
  *) echo "unknown platform $P" >&2; exit 1 ;;
esac
O=build/bearssl/$P; rm -rf "$O"; mkdir -p "$O/obj"
n=0
for f in $(find $SRC/src -name '*.c'); do
    "${CC[@]}" -w -I $SRC/inc -I $SRC/src -c "$f" -o "$O/obj/$(echo "${f#$SRC/src/}" | tr / _).o" && n=$((n + 1))
done
"${AR[@]}" "$O/libbearssl.a" "$O"/obj/*.o
rm -rf "$O/obj"
echo "BearSSL for $P: $n files, $(ls -l $O/libbearssl.a | awk '{print $5}') bytes"
