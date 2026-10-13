#!/bin/bash
# Gets the monochrome Noto Emoji font (SIL Open Font License, Google). The Longhouse app draws every emoji that it has no colour picture for from it.
# build_payload.sh copies it to data/longhouse/emoji.ttf on the phone.
set -e
cd "$(dirname "$0")"
mkdir -p emoji
curl -fL -o emoji/NotoEmoji.ttf "https://github.com/google/fonts/raw/main/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf"
ls -l emoji/NotoEmoji.ttf
