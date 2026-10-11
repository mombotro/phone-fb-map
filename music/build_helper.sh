#!/bin/bash
# Builds the Android audio helper (music/android/helper.jar). Needs a JDK and the Android SDK (platform + build-tools).
set -e
cd "$(dirname "$0")/android"
SDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}
JAR=$(ls -d "$SDK"/platforms/android-*/android.jar | sort -V | tail -1)
D8=$(ls -d "$SDK"/build-tools/*/d8 | sort -V | tail -1)
rm -rf out && mkdir out
javac --release 8 -cp "$JAR" -d out Helper.java Pcm.java 2>&1 | grep -v "^Note:\|warning" || true
[ -f out/Helper.class ] && [ -f out/Pcm.class ] || { echo "javac failed" >&2; exit 1; }
"$D8" --min-api 16 --lib "$JAR" --output out out/*.class
(cd out && zip -q ../helper.jar classes.dex)
ls -l helper.jar
