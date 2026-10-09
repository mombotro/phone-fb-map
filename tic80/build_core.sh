#!/bin/bash
# Builds libtic80core_{ios,android}.a from the TIC-80 sources in ./src (core + Lua only; no studio, no SDL).
# Dependencies compiled in: lua, zlib, libpng, giflib, blip-buf (git submodules under src/vendor).
set -e
cd "$(dirname "$0")"
SRC=$PWD/src; V=$SRC/vendor
SDK=${IOS_SDK:-$HOME/sdks/iPhoneOS9.3.sdk}
NDK=$(ls -d "$HOME"/Library/Android/sdk/ndk/27.* | head -1)
ACC=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/armv7a-linux-androideabi21-clang
AAR=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-ar

mkdir -p gen lib
cp "$V/libpng/scripts/pnglibconf.h.prebuilt" gen/pnglibconf.h

LUA=""; for f in lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring \
    ltable ltm lundump lvm lzio lauxlib lbaselib lcorolib ldblib liolib lmathlib loslib lstrlib ltablib lutf8lib \
    loadlib linit lbitlib; do [ -f "$V/lua/$f.c" ] && LUA="$LUA $V/lua/$f.c"; done
CORE="fftdata vqtdata core/core core/draw core/io core/sound tic cart tools zip tilesheet script \
      ext/fft ext/vqt ext/vqt_kernel ext/kiss_fft ext/kiss_fftr ext/png ext/gif api/lua api/luaapi api/parse_note studio/project"
FILES=""; for f in $CORE; do FILES="$FILES $SRC/src/$f.c"; done
FILES="$FILES $LUA $V/blip-buf/blip_buf.c"
for f in adler32 compress crc32 deflate inflate infback inftrees inffast trees uncompr zutil; do FILES="$FILES $V/zlib/$f.c"; done
for f in png pngerror pngget pngmem pngpread pngread pngrio pngrtran pngrutil pngset pngtrans pngwio pngwtran pngwrite pngwutil; do FILES="$FILES $V/libpng/$f.c"; done

for f in dgif_lib egif_lib gif_err gif_font gif_hash gifalloc openbsd-reallocarray; do FILES="$FILES $V/giflib/$f.c"; done

# TIC80_FFT_UNSUPPORTED: no audio capture on these targets, so ext/fft.c compiles as stubs (and skips miniaudio).
DEFS="-DTIC80_FFT_UNSUPPORTED -DTIC_RUNTIME_STATIC -DTIC_BUILD_WITH_LUA -DBUILD_DEPRECATED -DLUA_COMPAT_5_2 -DPNG_ARM_NEON_OPT=0"
INC="-I$SRC/include -I$SRC/src -I$V/lua -I$V/blip-buf -I$V/zlib -I$V/libpng -I$V/giflib -I$PWD/gen"

build() {   # build <name> <compiler...>   (compiles every file, 8 at a time)
    name=$1; shift
    rm -rf obj/$name; mkdir -p obj/$name
    i=0
    for f in $FILES; do
        i=$((i+1)); o=obj/$name/$(printf %03d $i)_$(basename "$f" .c).o
        "$@" -std=gnu11 -O2 -w $DEFS $INC -c "$f" -o "$o" &
        [ $((i % 8)) -eq 0 ] && wait
    done
    wait
}

build ios clang -arch armv7 -miphoneos-version-min=7.0 -isysroot "$SDK"
rm -f lib/libtic80core_ios.a; ar rcs lib/libtic80core_ios.a obj/ios/*.o
build android "$ACC" -fno-pie -fno-emulated-tls
rm -f lib/libtic80core_android.a; "$AAR" rcs lib/libtic80core_android.a obj/android/*.o
ls -l lib | awk 'NR>1 {print $5, $9}'
