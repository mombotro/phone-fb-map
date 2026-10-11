#!/bin/bash
# Copies a folder of music to the Android phone with adb. usage: sync_music_android.sh FOLDER
# The player reads /data/local/tmp/music. Needs adb and a rooted phone.
set -e
[ -d "$1" ] || { echo "usage: $0 FOLDER" >&2; exit 1; }
adb shell 'su -c "mkdir -p /data/local/tmp/music && chmod 777 /data/local/tmp/music"'
adb push "$1/." /data/local/tmp/music/
adb shell 'su -c "chmod -R 777 /data/local/tmp/music"'
