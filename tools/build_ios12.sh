#!/bin/bash
# Builds the iPhone 5s (arm64, iOS 12) programs into build/ios12/. They are NOT signed here: the Mac's ldid is too old for
# iOS 12. tools/deploy_ios12.sh copies them to the phone and signs them there.
# The iOS 9.3 SDK describes its libraries in a form the linker takes for the simulator when it links arm64, so
# CoreFoundation is linked from a small stub file that tools/make_cf_stub.py writes.
set -e
cd "$(dirname "$0")/.."
SDK=${IOS_SDK:-$HOME/sdks/iPhoneOS9.3.sdk}
mkdir -p build/ios12 build/stubs
common/fetch_stb_truetype.sh >/dev/null
[ -f build/stubs/CoreFoundation.tbd ] || python3 tools/make_cf_stub.py "$SDK" build/stubs/CoreFoundation.tbd
for t in paint calib fbreset notes timer fbshell bible reader pixel calendar; do
    out=$(clang -target arm64-apple-ios9.0 -O2 -isysroot "$SDK" -Wl,-w -Wl,-undefined,dynamic_lookup \
          -o "build/ios12/$t" "common/$t.c" build/stubs/CoreFoundation.tbd 2>&1); rc=$?
    printf '%s\n' "$out" | grep -v 'ld: warning' || true
    [ $rc -eq 0 ] || { echo "BUILD FAILED: $t" >&2; rm -f "build/ios12/$t"; exit 1; }
done
common/fetch_stb_image.sh >/dev/null
[ -f build/bearssl/ios12/libbearssl.a ] || common/build_bearssl.sh ios12 >/dev/null
out=$(clang -target arm64-apple-ios9.0 -O2 -isysroot "$SDK" -Wl,-w -Wl,-undefined,dynamic_lookup -I common -I common/third_party/bearssl/inc \
      -o build/ios12/longhouse common/longhouse.c build/bearssl/ios12/libbearssl.a build/stubs/CoreFoundation.tbd 2>&1) || { echo "$out" >&2; echo "BUILD FAILED: longhouse" >&2; exit 1; }
out=$(clang -target arm64-apple-ios9.0 -O2 -isysroot "$SDK" -Wl,-w -Wl,-undefined,dynamic_lookup \
      -o build/ios12/fblauncher launcher/launcher.c build/stubs/CoreFoundation.tbd 2>&1) || { echo "$out" >&2; echo "BUILD FAILED: launcher" >&2; exit 1; }
ls -l build/ios12 | awk 'NR>1 {print $5, $9}'
