#!/bin/bash
# Fetches stb_image.h (public domain, https://github.com/nothings/stb) into common/third_party/. The Longhouse chat app
# uses it to decode JPEG picture thumbnails. The source is not kept in this repository.
set -e
cd "$(dirname "$0")"; mkdir -p third_party
[ -f third_party/stb_image.h ] || curl -sSL -o third_party/stb_image.h https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
grep -m1 "stb_image - v" third_party/stb_image.h; shasum -a 256 third_party/stb_image.h | cut -c1-16
