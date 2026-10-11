#!/bin/bash
# Builds launcher/android_apk/FBTools.apk (the home-screen icon) and ../../build/fbstart_android (the detached starter).
# Needs a JDK and the Android SDK (platform, build-tools) and the NDK. The signing key is made once in ../../build/fbtools.keystore.
set -e
cd "$(dirname "$0")"
SDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}
JAR=$(ls -d "$SDK"/platforms/android-*/android.jar | sort -V | tail -1)
BT=$(ls -d "$SDK"/build-tools/*/ | sort -V | tail -1)
NDK=$(ls -d "$SDK"/ndk/27.* | head -1)
KS=../../build/fbtools.keystore
mkdir -p ../../build out && rm -rf out/* res/drawable-xhdpi/icon.png
python3 - <<'PY'
from PIL import Image, ImageDraw
im = Image.new("RGBA", (96, 96), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
d.rounded_rectangle((4, 4, 92, 92), radius=18, fill=(26, 40, 66, 255), outline=(76, 141, 219, 255), width=4)
d.rectangle((22, 28, 74, 34), fill=(76, 141, 219, 255)); d.rectangle((22, 44, 56, 50), fill=(63, 178, 127, 255)); d.rectangle((22, 60, 66, 66), fill=(224, 86, 76, 255))
im.save("res/drawable-xhdpi/icon.png")
PY
"$BT/aapt2" compile --dir res -o out/res.zip
"$BT/aapt2" link -o out/base.apk -I "$JAR" --manifest AndroidManifest.xml --min-sdk-version 16 --target-sdk-version 28 out/res.zip
javac --release 8 -cp "$JAR" -d out/classes src/com/fbtools/launcher/FBToolsActivity.java 2>&1 | grep -v "^Note:\|warning" || true
"$BT/d8" --min-api 16 --lib "$JAR" --output out out/classes/com/fbtools/launcher/*.class
(cd out && zip -q base.apk classes.dex)
"$BT/zipalign" -f 4 out/base.apk out/aligned.apk
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -alias fbtools -storepass fbtools1 -keypass fbtools1 -dname "CN=FB Tools" -keyalg RSA -keysize 2048 -validity 10000 >/dev/null 2>&1
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:fbtools1 --min-sdk-version 16 --out FBTools.apk out/aligned.apk
"$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/armv7a-linux-androideabi21-clang" -O2 -static -fno-pie -fno-emulated-tls -o ../../build/fbstart_android fbstart.c
ls -l FBTools.apk ../../build/fbstart_android | awk '{print $5, $9}'
