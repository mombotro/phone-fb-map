#!/bin/bash
# Fetches the reading fonts into tools/fonts/. All three have the SIL Open Font License, which allows copying them to a phone.
#   OpenDyslexic            https://github.com/antijingoist/opendyslexic        (weighted letter bottoms, for dyslexic readers)
#   Atkinson Hyperlegible   https://github.com/googlefonts/atkinson-hyperlegible (made by the Braille Institute for low vision)
#   Lexend                  https://github.com/googlefonts/lexend                (made to ease reading)
# The font files are not kept in this repository. Run tools/build_payload.sh afterwards.
set -e
cd "$(dirname "$0")"; mkdir -p fonts; cd fonts
get() { [ -s "$2" ] || curl -sSL -o "$2" "$1"; [ -s "$2" ] || { echo "could not fetch $2" >&2; exit 1; }; }
get https://github.com/antijingoist/opendyslexic/raw/main/compiled/OpenDyslexic-Regular.otf OpenDyslexic.otf
get https://raw.githubusercontent.com/googlefonts/atkinson-hyperlegible/main/fonts/ttf/AtkinsonHyperlegible-Regular.ttf AtkinsonHyperlegible.ttf
get https://github.com/googlefonts/lexend/raw/main/fonts/lexend/ttf/Lexend-Regular.ttf Lexend.ttf
ls -l | awk 'NR>1 {print $5, $9}'
