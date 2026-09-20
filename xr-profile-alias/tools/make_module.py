#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Assemble the Magisk module zip `pico_xr_profile_alias`.

Self-contained: reads the compiled shim from build/ and the original library
from lib/ (extracted via tools/extract_orig.sh), stages everything under
module/, and writes dist/pico_xr_profile_alias.zip.

Usage:  python tools/make_module.py
"""
import os
import shutil
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))          # tools/
BASE = os.path.dirname(HERE)                               # xr-profile-alias/
MOD = os.path.join(BASE, "module")                         # module template
BUILD = os.path.join(BASE, "build", "libopenxr_forwardloader.so")
ORIG = os.path.join(BASE, "lib", "libopenxr_forwardloader_orig.so")
ZIP = os.path.join(BASE, "dist", "pico_xr_profile_alias.zip")

for p in (BUILD, ORIG):
    if not os.path.exists(p):
        sys.exit("missing %s\n"
                 "  build/... : run build.sh first\n"
                 "  lib/...   : run tools/extract_orig.sh first (needs a rooted headset)"
                 % p)

# --------------------------------------------------------- stage binaries
stage = os.path.join(BASE, "build", "_stage")
if os.path.isdir(stage):
    shutil.rmtree(stage)
os.makedirs(os.path.join(stage, "system", "lib64"))
os.makedirs(os.path.join(stage, "system", "etc"))

shutil.copy2(BUILD, os.path.join(stage, "system", "lib64", "libopenxr_forwardloader.so"))
shutil.copy2(ORIG, os.path.join(stage, "system", "lib64", "libopenxr_forwardloader_orig.so"))

# ------------------------------------- runtime config (world-readable app)
w_conf_path = os.path.join(MOD, "system", "etc", "pico_xr_alias.conf")
if not os.path.exists(w_conf_path):
    sys.exit("missing module template file: %s" % w_conf_path)

# --------------------------------------------------- copy template scripts
for root, dirs, files in os.walk(MOD):
    rel_root = os.path.relpath(root, MOD)
    dst_root = stage if rel_root == "." else os.path.join(stage, rel_root)
    os.makedirs(dst_root, exist_ok=True)
    for fn in files:
        shutil.copy2(os.path.join(root, fn), os.path.join(dst_root, fn))

# ------------------------------------------------------------------- zip
os.makedirs(os.path.dirname(ZIP), exist_ok=True)
if os.path.exists(ZIP):
    os.remove(ZIP)

with zipfile.ZipFile(ZIP, "w", zipfile.ZIP_DEFLATED) as z:
    # directories first, 0755
    for root, dirs, _files in os.walk(stage):
        for d in sorted(dirs):
            full = os.path.join(root, d)
            rel = os.path.relpath(full, stage).replace("\\", "/") + "/"
            zi = zipfile.ZipInfo(rel)
            zi.compress_type = zipfile.ZIP_STORED
            zi.external_attr = 0o40755 << 16
            z.writestr(zi, b"")
    for root, _dirs, files in os.walk(stage):
        for fn in sorted(files):
            full = os.path.join(root, fn)
            rel = os.path.relpath(full, stage).replace("\\", "/")
            zi = zipfile.ZipInfo.from_file(full, rel)
            zi.compress_type = zipfile.ZIP_DEFLATED
            # scripts must be executable inside the zip for Magisk
            zi.external_attr = (0o100755 if fn.endswith(".sh") else 0o100644) << 16
            with open(full, "rb") as src, z.open(zi, "w") as dst:
                dst.write(src.read())

print("module zip:", ZIP, os.path.getsize(ZIP), "bytes")
with zipfile.ZipFile(ZIP) as z:
    for i in z.infolist():
        print("   %-55s %8d" % (i.filename, i.file_size))
