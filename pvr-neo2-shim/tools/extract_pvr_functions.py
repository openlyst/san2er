#!/usr/bin/env python3
import argparse
import json
import os
import re
import subprocess
import tempfile
import zipfile


def from_exported_symbols(so_path):
    """Extract exported functions from a .so's dynamic symbol table."""
    out = subprocess.check_output(
        ["readelf", "-sW", so_path], text=True, stderr=subprocess.DEVNULL
    )
    funcs = set()
    for line in out.splitlines():
        if "FUNC" not in line or "GLOBAL" not in line or "DEFAULT" not in line:
            continue
        m = re.search(r"\s((?:Pvr_|PVR_|Java_|JNI_)[A-Za-z][A-Za-z0-9_]+)$", line)
        if m:
            funcs.add(m.group(1))
    return funcs


def from_il2cpp_binary(path):
    out = subprocess.check_output(["strings", path], text=True)
    funcs = set()
    for line in out.splitlines():
        if re.match(r"^(Pvr_|PVR_|pvr_)[A-Za-z0-9_]+$", line):
            funcs.add(line)
    return funcs


def from_apk(apk_path):
    funcs = set()
    with zipfile.ZipFile(apk_path) as z:
        so_path = "lib/arm64-v8a/libPvr_UnitySDK.so"
        if so_path in z.namelist():
            with tempfile.NamedTemporaryFile(delete=False, suffix=".so") as tmp:
                tmp.write(z.read(so_path))
                tmp_so = tmp.name
            try:
                funcs |= from_exported_symbols(tmp_so)
            finally:
                os.unlink(tmp_so)

        il2cpp_path = "lib/arm64-v8a/libil2cpp.so"
        if il2cpp_path in z.namelist():
            with tempfile.NamedTemporaryFile(delete=False) as tmp:
                tmp.write(z.read(il2cpp_path))
                tmp_il2cpp = tmp.name
            try:
                funcs |= from_il2cpp_binary(tmp_il2cpp)
            finally:
                os.unlink(tmp_il2cpp)
    return funcs


def from_cs_files(paths):
    funcs = set()
    for p in paths:
        with open(p) as f:
            text = f.read()
        for m in re.finditer(
            r"(?:private|public|internal|protected)\s+static\s+extern\s+[\w<>,\[\]\s]+\s+(\w+)\s*\(",
            text,
        ):
            name = m.group(1)
            if name.startswith(("Pvr_", "PVR_", "pvr_")):
                funcs.add(name)
    return funcs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--apk", help="APK containing libPvr_UnitySDK.so and/or libil2cpp.so")
    parser.add_argument("--il2cpp", help="Path to libil2cpp.so directly")
    parser.add_argument("--so", help="Path to libPvr_UnitySDK.so directly")
    parser.add_argument("--sdk-cs", nargs="*", default=[], help="SDK C# source files")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    funcs = set()
    if args.apk:
        funcs |= from_apk(args.apk)
    if args.il2cpp:
        funcs |= from_il2cpp_binary(args.il2cpp)
    if args.so:
        funcs |= from_exported_symbols(args.so)
    funcs |= from_cs_files(args.sdk_cs)

    with open(args.out, "w") as f:
        json.dump(sorted(funcs), f, indent=2)


if __name__ == "__main__":
    main()
