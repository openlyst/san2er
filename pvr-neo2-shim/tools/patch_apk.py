#!/usr/bin/env python3
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

ANDROID_NS = "http://schemas.android.com/apk/res/android"
ET.register_namespace("android", ANDROID_NS)


def run(cmd, **kwargs):
    print("+", " ".join(cmd))
    subprocess.check_call(cmd, **kwargs)


def ensure_keystore(path):
    if path.exists():
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    run(
        [
            "keytool",
            "-genkey",
            "-v",
            "-keystore",
            str(path),
            "-alias",
            "androiddebugkey",
            "-keyalg",
            "RSA",
            "-keysize",
            "2048",
            "-validity",
            "10000",
            "-dname",
            "CN=Android Debug,O=Android,C=US",
            "-storepass",
            "android",
            "-keypass",
            "android",
        ]
    )


def patch_manifest(manifest_path):
    tree = ET.parse(manifest_path)
    root = tree.getroot()

    ns = {"android": ANDROID_NS}

    # Add vr.headtracking if missing.
    has_headtracking = any(
        elem.get(f"{{{ANDROID_NS}}}name") == "android.hardware.vr.headtracking"
        for elem in root.findall("uses-feature")
    )
    if not has_headtracking:
        feature = ET.SubElement(root, "uses-feature")
        feature.set(f"{{{ANDROID_NS}}}name", "android.hardware.vr.headtracking")
        feature.set(f"{{{ANDROID_NS}}}required", "true")
        feature.set(f"{{{ANDROID_NS}}}version", "1")
        print("added android.hardware.vr.headtracking")

    # Fix com.pvr.instructionset if it says 32 but only arm64 libs are present.
    app = root.find("application")
    if app is not None:
        for meta in app.findall("meta-data"):
            if meta.get(f"{{{ANDROID_NS}}}name") == "com.pvr.instructionset":
                value = meta.get(f"{{{ANDROID_NS}}}value")
                if value == "32":
                    meta.set(f"{{{ANDROID_NS}}}value", "64")
                    print("changed com.pvr.instructionset from 32 to 64")
                break

    tree.write(manifest_path, encoding="utf-8", xml_declaration=True)


def inject_shim(decoded_dir, shim_path):
    lib_dir = Path(decoded_dir) / "lib" / "arm64-v8a"
    if not lib_dir.exists():
        raise RuntimeError(f"arm64-v8a lib dir not found: {lib_dir}")

    original = lib_dir / "libPvr_UnitySDK.so"
    if not original.exists():
        raise RuntimeError(f"libPvr_UnitySDK.so not found in {lib_dir}")

    renamed = lib_dir / "libPvr_UnitySDK_orig.so"
    shutil.move(str(original), str(renamed))
    shutil.copy2(str(shim_path), str(original))
    print(f"renamed original to {renamed.name} and injected shim")


def patch_apk(apk_path, shim_path, out_path, keystore, work_dir):
    os.makedirs(work_dir, exist_ok=True)
    work = tempfile.mkdtemp(prefix="pvr-neo2-shim-", dir=work_dir)
    try:
        decoded = os.path.join(work, "decoded")
        run(["apktool", "decode", "-f", "-s", "-o", decoded, str(apk_path)])

        patch_manifest(os.path.join(decoded, "AndroidManifest.xml"))
        inject_shim(decoded, shim_path)

        unsigned = os.path.join(work, "unsigned.apk")
        run(["apktool", "build", "-o", unsigned, decoded])

        aligned = os.path.join(work, "aligned.apk")
        run(["zipalign", "-p", "-f", "4", unsigned, aligned])

        ensure_keystore(Path(keystore))
        run(
            [
                "apksigner",
                "sign",
                "--ks",
                keystore,
                "--ks-pass",
                "pass:android",
                "--key-pass",
                "pass:android",
                "--out",
                str(out_path),
                aligned,
            ]
        )
        print(f"patched APK written to {out_path}")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--apk", required=True, help="Input APK")
    parser.add_argument("--shim", required=True, help="Path to built libPvr_UnitySDK.so shim")
    parser.add_argument("--out", required=True, help="Output patched APK")
    parser.add_argument(
        "--keystore",
        default=os.path.expanduser("~/.pvr-neo2-shim/debug.keystore"),
        help="Debug keystore path",
    )
    parser.add_argument(
        "--work-dir",
        default=os.path.expanduser("~/.pvr-neo2-shim/work"),
        help="Directory for temporary apktool files",
    )
    args = parser.parse_args()

    patch_apk(args.apk, args.shim, args.out, args.keystore, args.work_dir)


if __name__ == "__main__":
    main()
