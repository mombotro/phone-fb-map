#!/bin/bash
# Builds tools/PhoneFiles.app, a Mac app that opens the Phone Files window. Needs python3, and for the phones:
# libimobiledevice (brew install libimobiledevice) for iPhones, and adb (Android platform-tools) for Android.
set -e
cd "$(dirname "$0")"
APP=PhoneFiles.app
PY=$(command -v python3) || { echo "python3 not found" >&2; exit 1; }
NDK=$(ls -d "$HOME"/Library/Android/sdk/ndk/27.* 2>/dev/null | head -1)
if [ ! -f ../build/fbls_android ] && [ -n "$NDK" ]; then       # the small folder-listing program for Android phones
    mkdir -p ../build
    "$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/armv7a-linux-androideabi21-clang" -O2 -static -fno-pie -fno-emulated-tls -o ../build/fbls_android fbls.c
fi
rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp phonefiles_gui.py phonefiles_gui.html "$APP/Contents/Resources/"
[ -f ../build/fbls_android ] && cp ../build/fbls_android "$APP/Contents/Resources/fbls_android"
cat > "$APP/Contents/MacOS/PhoneFiles" <<LAUNCH
#!/bin/bash
# Finder gives apps a short PATH, so add the places where brew and the Android tools live.
export PATH="/opt/homebrew/bin:/usr/local/bin:\$HOME/Library/Android/sdk/platform-tools:/usr/bin:/bin:/usr/sbin:/sbin"
exec "$PY" "\$(dirname "\$0")/../Resources/phonefiles_gui.py" --app
LAUNCH
chmod +x "$APP/Contents/MacOS/PhoneFiles"
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>Phone Files</string>
<key>CFBundleDisplayName</key><string>Phone Files</string>
<key>CFBundleIdentifier</key><string>local.phonefiles</string>
<key>CFBundleExecutable</key><string>PhoneFiles</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSUIElement</key><false/>
</dict></plist>
PLIST
echo "built tools/$APP"
