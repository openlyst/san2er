#!/usr/bin/env python3
import argparse
import json
import os
import re
import subprocess
import tempfile
import zipfile


def from_il2cpp_binary(path):
    out = subprocess.check_output(["strings", path], text=True)
    funcs = set()
    for line in out.splitlines():
        if re.match(r"^(Pvr_|PVR_|pvr_)[A-Za-z0-9_]+$", line):
            funcs.add(line)
    return funcs


def from_apk(apk_path):
    il2cpp_path = "lib/arm64-v8a/libil2cpp.so"
    with zipfile.ZipFile(apk_path) as z:
        if il2cpp_path not in z.namelist():
            raise RuntimeError(f"{il2cpp_path} not found in {apk_path}")
        with tempfile.NamedTemporaryFile(delete=False) as tmp:
            tmp.write(z.read(il2cpp_path))
            tmp_path = tmp.name
    try:
        return from_il2cpp_binary(tmp_path)
    finally:
        os.unlink(tmp_path)


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
    parser.add_argument("--apk", help="APK containing libil2cpp.so")
    parser.add_argument("--il2cpp", help="Path to libil2cpp.so directly")
    parser.add_argument("--sdk-cs", nargs="*", default=[], help="SDK C# source files")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    funcs = set()
    if args.apk:
        funcs |= from_apk(args.apk)
    if args.il2cpp:
        funcs |= from_il2cpp_binary(args.il2cpp)
    funcs |= from_cs_files(args.sdk_cs)

    with open(args.out, "w") as f:
        json.dump(sorted(funcs), f, indent=2)


if __name__ == "__main__":
    main()
