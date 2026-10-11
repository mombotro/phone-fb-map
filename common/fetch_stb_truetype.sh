#!/bin/bash
# Fetches stb_truetype.h (public domain, https://github.com/nothings/stb) into common/third_party/. The Reader and the Bible
# use it to draw reading fonts. The source is not kept in this repository.
set -e
cd "$(dirname "$0")"; mkdir -p third_party
[ -f third_party/stb_truetype.h ] || curl -sSL -o third_party/stb_truetype.h https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h
grep -m1 "stb_truetype.h - v" third_party/stb_truetype.h; shasum -a 256 third_party/stb_truetype.h | cut -c1-16
