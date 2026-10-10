#!/usr/bin/env python3
"""Write a version-4 .tbd stub for CoreFoundation with the arm64-ios target, from the SDK's old-format file.
usage: make_cf_stub.py SDK_PATH OUT.tbd"""
import re, sys, os
sdk, out = sys.argv[1], sys.argv[2]
t = open(os.path.join(sdk, "System/Library/Frameworks/CoreFoundation.framework/CoreFoundation.tbd")).read()
body = re.search(r"exports:(.*)", t, re.S).group(1)
syms = sorted({x.strip() for b in re.finditer(r"symbols:\s*\[(.*?)\]", body, re.S) for x in b.group(1).replace("\n", " ").split(",") if x.strip()})
open(out, "w").write("--- !tapi-tbd\ntbd-version: 4\ntargets: [ arm64-ios ]\n"
    "install-name: '/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation'\ncurrent-version: 1290\n"
    "exports:\n  - targets: [ arm64-ios ]\n    symbols: [ " + ", ".join(syms) + " ]\n...\n")
print(len(syms), "symbols")
