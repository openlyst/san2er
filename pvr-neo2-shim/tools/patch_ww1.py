#!/usr/bin/env python3
"""Patch WW1 Fighters (PXR SDK 2.1.x) to use old PVR Unity XR plugin."""
import os
import shutil
import subprocess
import sys
import tempfile
import json
from pathlib import Path


def run(cmd, **kwargs):
    print("+", " ".join(cmd))
    subprocess.check_call(cmd, **kwargs)


def main():
    apk = sys.argv[1]
    old_apk = sys.argv[2]
    shim = sys.argv[3]
    out = sys.argv[4] if len(sys.argv) > 4 else apk.replace(".apk", "_neo2.apk")

    work = tempfile.mkdtemp(prefix="ww1-patch-")
    try:
        decoded = os.path.join(work, "decoded")
        run(["apktool", "decode", "-f", "-o", decoded, apk])

        lib_dir = Path(decoded) / "lib" / "arm64-v8a"

        # 1. Remove new XR plugin libs (they don't work on Neo 2)
        for f in ["libPxrPlatform.so", "libpxr_api.so", "libpxrplatformloader.so",
                   "libpxr_api.so", "libGfxWXRUnity.so",
                   "libwvr_api.so", "libwvr_client_bootstrap.so",
                   "libwvrassimp.so", "libwvrugl.so", "libwvrunity.so", "libwvrunityxr.so"]:
            p = lib_dir / f
            if p.exists():
                p.unlink()
                print(f"removed {f}")

        # 2. Extract old XR plugin + deps from old game
        tmp_extract = os.path.join(work, "old_libs")
        os.makedirs(tmp_extract)
        run(["unzip", "-o", old_apk,
             "lib/arm64-v8a/libUnityPicoVR.so",
             "lib/arm64-v8a/libtracking_module.so",
             "lib/arm64-v8a/libnative.so",
             "lib/arm64-v8a/libPvr_UnitySDK.so",
             "-d", tmp_extract])

        old_lib_dir = Path(tmp_extract) / "lib" / "arm64-v8a"

        # 3. Add old XR plugin
        shutil.copy2(old_lib_dir / "libUnityPicoVR.so", lib_dir / "libUnityPicoVR.so")
        shutil.copy2(old_lib_dir / "libtracking_module.so", lib_dir / "libtracking_module.so")
        # libnative.so already exists in the new game, but let's overwrite with old one
        shutil.copy2(old_lib_dir / "libnative.so", lib_dir / "libnative.so")

        # 4. Add PVR SDK shim + original
        # Original from old game -> libPvr_UnitySDK_orig.so
        shutil.copy2(old_lib_dir / "libPvr_UnitySDK.so", lib_dir / "libPvr_UnitySDK_orig.so")
        # Shim -> libPvr_UnitySDK.so
        shutil.copy2(shim, lib_dir / "libPvr_UnitySDK.so")

        print("added libUnityPicoVR.so, libtracking_module.so, libPvr_UnitySDK.so (shim), libPvr_UnitySDK_orig.so")

        # 5. Update UnitySubsystemsManifest.json
        manifest_path = Path(decoded) / "assets" / "bin" / "Data" / "UnitySubsystems" / "PxrPlatform" / "UnitySubsystemsManifest.json"
        if manifest_path.exists():
            manifest = json.loads(manifest_path.read_text())
            manifest["name"] = "UnityPicoVR"
            manifest["libraryName"] = "UnityPicoVR"
            manifest_path.write_text(json.dumps(manifest, indent=4))
            print(f"updated UnitySubsystemsManifest: name/libraryName -> UnityPicoVR")

        # Also check if there's a separate UnityPicoVR manifest dir
        pvr_manifest_dir = Path(decoded) / "assets" / "bin" / "Data" / "UnitySubsystems" / "UnityPicoVR"
        if not pvr_manifest_dir.exists():
            pvr_manifest_dir.mkdir(parents=True)
            pvr_manifest = {
                "name": "UnityPicoVR",
                "version": "1.0.0-preview",
                "libraryName": "UnityPicoVR",
                "displays": [
                    {
                        "id": "PicoXR Display",
                        "disablesLegacyVr": True,
                        "supportedMirrorBlitReservedModes": ["leftEye", "rightEye", "sideBySide", "occlusionMesh"]
                    }
                ],
                "inputs": [
                    {"id": "PicoXR Input"}
                ]
            }
            (pvr_manifest_dir / "UnitySubsystemsManifest.json").write_text(
                json.dumps(pvr_manifest, indent=4))
            print("created UnityPicoVR/UnitySubsystemsManifest.json")

        # 6. Build APK
        unsigned = os.path.join(work, "unsigned.apk")
        run(["apktool", "build", "-o", unsigned, decoded])

        # 7. Zipalign
        aligned = os.path.join(work, "aligned.apk")
        run(["zipalign", "-p", "-f", "4", unsigned, aligned])

        # 8. Sign
        ks = os.path.expanduser("~/.pvr-neo2-shim/debug.keystore")
        if not os.path.exists(ks):
            os.makedirs(os.path.dirname(ks), exist_ok=True)
            run(["keytool", "-genkey", "-v", "-keystore", ks,
                 "-alias", "androiddebugkey", "-keyalg", "RSA", "-keysize", "2048",
                 "-validity", "10000", "-dname", "CN=Android Debug,O=Android,C=US",
                 "-storepass", "android", "-keypass", "android"])

        run(["apksigner", "sign", "--ks", ks, "--ks-pass", "pass:android",
             "--key-pass", "pass:android", "--out", out, aligned])

        print(f"\npatched APK: {out}")

    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
