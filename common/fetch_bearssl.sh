#!/bin/bash
# Fetches BearSSL (a small TLS library, MIT licence, https://bearssl.org) into common/third_party/bearssl. The source is not
# kept in this repository. The Longhouse chat app uses it to talk HTTPS to the signaling server.
set -e
cd "$(dirname "$0")"
mkdir -p third_party
[ -d third_party/bearssl/.git ] || git clone --quiet https://www.bearssl.org/git/BearSSL third_party/bearssl
git -C third_party/bearssl log -1 --format='BearSSL %h %cd'
