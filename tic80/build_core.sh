#!/bin/bash
# Builds libtic80core_{ios,android}.a from the TIC-80 sources in ./src (core + Lua and Fennel; no studio, no SDL).
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
      ext/fft ext/vqt ext/vqt_kernel ext/kiss_fft ext/kiss_fftr ext/png ext/gif api/lua api/fennel api/luaapi api/parse_note studio/project"
FILES=""; for f in $CORE; do FILES="$FILES $SRC/src/$f.c"; done

# More script languages for the carts. Each block adds its sources, its -D flags and its include folder.
LANGDEFS=""; LANGINC=""
# JavaScript (QuickJS)
Q=$V/quickjs; for f in quickjs libregexp libunicode dtoa; do FILES="$FILES $Q/$f.c"; done
FILES="$FILES $SRC/src/api/js.c"; LANGDEFS="$LANGDEFS -DTIC_BUILD_WITH_JS"; LANGINC="$LANGINC -I$Q"
# Wren
W=$V/wren/src; for f in optional/wren_opt_meta optional/wren_opt_random vm/wren_compiler vm/wren_core vm/wren_debug vm/wren_primitive vm/wren_utils vm/wren_value vm/wren_vm; do FILES="$FILES $W/$f.c"; done
FILES="$FILES $SRC/src/api/wren.c"; LANGDEFS="$LANGDEFS -DTIC_BUILD_WITH_WREN"; LANGINC="$LANGINC -I$W/include -I$W/optional -I$W/vm"
# Scheme (s7)
FILES="$FILES $V/s7/s7.c $SRC/src/api/scheme.c"; LANGDEFS="$LANGDEFS -DTIC_BUILD_WITH_SCHEME"; LANGINC="$LANGINC -I$V/s7"
# Python (pocketpy)
for f in $(find "$V/pocketpy/src" -name '*.c'); do FILES="$FILES $f"; done
FILES="$FILES $SRC/src/api/python.c"; LANGDEFS="$LANGDEFS -DTIC_BUILD_WITH_PYTHON -DPK_ENABLE_OS=0 -DPK_ENABLE_THREADS=0 -DPK_ENABLE_DETERMINISM=0 -DPK_ENABLE_WATCHDOG=0 -DPK_ENABLE_CUSTOM_SNAME=0"; LANGINC="$LANGINC -I$V/pocketpy/include"
# Janet (the single C file is made by "make build/c/janet.c" in vendor/janet: a Mac C compiler is enough)
[ -f "$V/janet/build/c/janet.c" ] || (cd "$V/janet" && make build/c/janet.c >/dev/null)
FILES="$FILES $V/janet/build/c/janet.c $SRC/src/api/janet.c"; LANGDEFS="$LANGDEFS -DTIC_BUILD_WITH_JANET"; LANGINC="$LANGINC -I$V/janet/src/include -I$V/janet/src/conf"
# Squirrel (C++: compiled as C++ without exceptions or run time type information; the player links the C++ library)
SQ=$V/squirrel; for f in squirrel/sqapi squirrel/sqbaselib squirrel/sqclass squirrel/sqcompiler squirrel/sqdebug squirrel/sqfuncstate squirrel/sqlexer squirrel/sqmem squirrel/sqobject squirrel/sqstate squirrel/sqtable squirrel/sqvm sqstdlib/sqstdaux sqstdlib/sqstdblob sqstdlib/sqstdio sqstdlib/sqstdmath sqstdlib/sqstdrex sqstdlib/sqstdstream sqstdlib/sqstdstring sqstdlib/sqstdsystem; do FILES="$FILES $SQ/$f.cpp"; done
FILES="$FILES $SRC/src/api/squirrel.c"; LANGDEFS="$LANGDEFS -DTIC_BUILD_WITH_SQUIRREL"; LANGINC="$LANGINC -I$SQ/include -I$SQ/squirrel -I$SQ/sqstdlib"
# WebAssembly (wasm3)
for f in m3_bind m3_code m3_compile m3_core m3_parse m3_env m3_exec m3_function m3_info m3_module; do FILES="$FILES $V/wasm3/source/$f.c"; done
FILES="$FILES $SRC/src/api/wasm.c"; LANGDEFS="$LANGDEFS -DTIC_BUILD_WITH_WASM -Dd_m3LogOutput=0"; LANGINC="$LANGINC -I$V/wasm3/source"
# MoonScript (Lua with lpeg and the moonscript compiler)
for f in lpcap lpcode lpprint lptree lpvm lpcset; do FILES="$FILES $V/lpeg/$f.c"; done
FILES="$FILES $SRC/src/api/moonscript.c"; LANGDEFS="$LANGDEFS -DTIC_BUILD_WITH_MOON"; LANGINC="$LANGINC -I$V/moonscript"
FILES="$FILES $LUA $V/blip-buf/blip_buf.c"
for f in adler32 compress crc32 deflate inflate infback inftrees inffast trees uncompr zutil; do FILES="$FILES $V/zlib/$f.c"; done
for f in png pngerror pngget pngmem pngpread pngread pngrio pngrtran pngrutil pngset pngtrans pngwio pngwtran pngwrite pngwutil; do FILES="$FILES $V/libpng/$f.c"; done

for f in dgif_lib egif_lib gif_err gif_font gif_hash gifalloc openbsd-reallocarray; do FILES="$FILES $V/giflib/$f.c"; done

# TIC80_FFT_UNSUPPORTED: no audio capture on these targets, so ext/fft.c compiles as stubs (and skips miniaudio).
DEFS="$LANGDEFS -DTIC80_FFT_UNSUPPORTED -DTIC_RUNTIME_STATIC -DTIC_BUILD_WITH_LUA -DTIC_BUILD_WITH_FENNEL -DBUILD_DEPRECATED -DLUA_COMPAT_5_2 -DPNG_ARM_NEON_OPT=0"
INC="$LANGINC -I$SRC/include -I$SRC/src -I$V/fennel -I$V/lua -I$V/blip-buf -I$V/zlib -I$V/libpng -I$V/giflib -I$PWD/gen"

build() {   # build <name> <compiler...>   (compiles every file, 8 at a time)
    name=$1; shift
    rm -rf obj/$name; mkdir -p obj/$name
    i=0
    for f in $FILES; do
        i=$((i+1)); o=obj/$name/$(printf %03d $i)_$(basename "$f" .c).o
        XF=""; LANGSTD="-std=gnu11"
        case "$f" in
            *.cpp) LANGSTD="-x c++ -std=c++11 -fno-exceptions -fno-rtti"; [ "$name" = ios ] && LANGSTD="$LANGSTD -stdlib=libc++ -isystem /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1";;
            */quickjs/*) XF="-funsigned-char -D_GNU_SOURCE"; [ "$name" = ios ] && XF="$XF -include $PWD/ios_compat.h";;
            */janet/*) XF="-D_GNU_SOURCE -DJANET_SINGLE_THREADED -DJANET_NO_EV -DJANET_NO_NET -DJANET_NO_PROCESSES -DJANET_NO_FFI -DJANET_NO_DYNAMIC_MODULES"; [ "$name" = ios ] && XF="$XF -DMAC_OS_X_VERSION_10_12=101200 -include $PWD/ios_compat.h -I$PWD/shim";;       # one thread, no sockets or child processes
            */wasm3/*) XF="-DM3_HAS_TAIL_CALL=0";;                                   # clang cannot do the tail calls on 32 bit ARM
            */pocketpy/*) [ "$name" = ios ] && XF="-D_Thread_local= -include $PWD/ios_compat.h";;                # iOS 7 has no thread local storage; one thread is enough
            */s7/*) [ "$name" = ios ] && XF="-include stdlib.h -include setjmp.h -include math.h -include pthread.h -include unistd.h -include time.h -include sys/time.h";;       # s7 defines a macro "noreturn" that clashes with the iOS headers
        esac
        ("$@" $LANGSTD -O2 -w $DEFS $INC $XF -c "$f" -o "$o" 2>"$o.err" || { echo "FAILED ($name): $f" >&2; head -3 "$o.err" >&2; }) &
        [ $((i % 8)) -eq 0 ] && wait
    done
    wait
}

build ios clang -arch armv7 -miphoneos-version-min=7.0 -isysroot "$SDK"
rm -f lib/libtic80core_ios.a; ar rcs lib/libtic80core_ios.a obj/ios/*.o
build android "$ACC" -fno-pie -fno-emulated-tls
rm -f lib/libtic80core_android.a; "$AAR" rcs lib/libtic80core_android.a obj/android/*.o
ls -l lib | awk 'NR>1 {print $5, $9}'
