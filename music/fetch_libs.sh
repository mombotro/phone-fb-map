#!/bin/bash
# Gets the two single-file decoders: dr_flac (public domain / MIT-0) and stb_vorbis (public domain).
set -e
cd "$(dirname "$0")"
mkdir -p third_party
curl -fsSL -o third_party/dr_flac.h https://raw.githubusercontent.com/mackron/dr_libs/master/dr_flac.h
curl -fsSL -o third_party/stb_vorbis.c https://raw.githubusercontent.com/nothings/stb/master/stb_vorbis.c
shasum -a 256 third_party/dr_flac.h third_party/stb_vorbis.c
