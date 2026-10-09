#!/bin/bash
# Gets the TIC-80 source (MIT license, by Vadim Grigoruk) at the tested commit, plus the five
# vendor libraries that the core needs. Run this once before build_core.sh.
set -e
cd "$(dirname "$0")"
SHA=a764b03e81956dfd9d4dfe2e334e44261b8225af
if [ ! -d src/.git ]; then
    mkdir -p src
    git -C src init -q
    git -C src remote add origin https://github.com/nesbox/TIC-80
fi
git -C src fetch --depth 1 origin "$SHA"
git -C src checkout -q FETCH_HEAD
git -C src submodule update --init --depth 1 vendor/lua vendor/blip-buf vendor/zlib vendor/libpng vendor/giflib
echo "TIC-80 source ready in tic80/src"
