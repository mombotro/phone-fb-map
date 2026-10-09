#!/bin/bash
# Builds the Mac app "Send to Phone.app" next to this script. Drag it to the Dock, then drop music on it.
# The app calls send_to_phone.py by absolute path, so build it again if you move the project.
set -e
cd "$(dirname "$0")"
SCRIPT="$PWD/send_to_phone.py"
sed "s#__SCRIPT_PATH__#$SCRIPT#" send_to_phone.applescript > /tmp/send_to_phone_build.applescript
rm -rf "Send to Phone.app"
osacompile -o "Send to Phone.app" /tmp/send_to_phone_build.applescript
rm -f /tmp/send_to_phone_build.applescript
echo "built: $PWD/Send to Phone.app"
