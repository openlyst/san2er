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
    pxr_stub = sys.argv[4]
    platform_stub = sys.argv[5]
    out = sys.argv[6] if len(sys.argv) > 6 else apk.replace(".apk", "_neo2.apk")

    work = tempfile.mkdtemp(prefix="ww1-patch-")
    try:
        decoded = os.path.join(work, "decoded")
        run(["apktool", "decode", "-f", "-o", decoded, apk])

        lib_dir = Path(decoded) / "lib" / "arm64-v8a"

        # 1. Remove WaveVR libs (not needed on Pico)
        for f in ["libGfxWXRUnity.so",
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

        # 3. Add old PVR SDK deps (tracking_module needed by Pvr_UnitySDK)
        shutil.copy2(old_lib_dir / "libtracking_module.so", lib_dir / "libtracking_module.so")
        # libnative.so already exists in the new game, but let's overwrite with old one
        shutil.copy2(old_lib_dir / "libnative.so", lib_dir / "libnative.so")

        # 4. Add PVR SDK shim + original
        # Original from old game -> libPvr_UnitySDK_orig.so
        shutil.copy2(old_lib_dir / "libPvr_UnitySDK.so", lib_dir / "libPvr_UnitySDK_orig.so")
        # Shim -> libPvr_UnitySDK.so
        shutil.copy2(shim, lib_dir / "libPvr_UnitySDK.so")

        # 4b. Keep original libpxr_api.so - the original libPxrPlatform.so
        # calls into it for rendering/tracking and expects real implementations.
        # Stubbing it causes null pointer crashes in libPxrPlatform.so.

        # 4c. Keep original libPxrPlatform.so - it has UnityPluginLoad which
        # registers the PICO Display/PICO Input subsystems with Unity's XR
        # SDK. Without this, PXR_Loader.Initialize() fails with "Unable to
        # start PICO Plugin" because no display subsystem is registered.

        print("added libtracking_module.so, libPvr_UnitySDK.so (shim), libPvr_UnitySDK_orig.so, kept original libpxr_api.so and libPxrPlatform.so")

        # 5b. Merge old game's Java classes needed by libPvr_UnitySDK_orig.so
        # The old PVR SDK calls FindClass for com.psmart.vrlib.VrActivity, etc.
        # These classes exist in the old game's dex but not the new game.
        old_decoded = os.path.join(work, "old_decoded")
        run(["apktool", "decode", "-f", "-o", old_decoded, old_apk])

        old_smali = Path(old_decoded) / "smali"
        new_smali = Path(decoded) / "smali"
        # Copy all smali files from old game that don't exist in the new game.
        # This covers vrlib, pvrservice, controller, hummingbird, and any other
        # dependencies the old plugin needs.
        copied = 0
        for f in old_smali.rglob("*.smali"):
            rel = f.relative_to(old_smali)
            target = new_smali / rel
            if not target.exists():
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(f, target)
                copied += 1
        print(f"merged {copied} smali files from old game")

        # 5c. Inject System.loadLibrary("Pvr_UnitySDK") into UnityPlayerActivity
        # The old libPvr_UnitySDK_orig.so needs JNI_OnLoad to be called by the
        # VM to initialize VrLibJavaVM. The shim forwards JNI_OnLoad to the
        # original, but JNI_OnLoad is only called when System.loadLibrary
        # loads the .so. The XR loader uses dlopen which bypasses JNI_OnLoad.
        unity_activity = new_smali / "com" / "unity3d" / "player" / "UnityPlayerActivity.smali"
        if unity_activity.exists():
            content = unity_activity.read_text()
            clinit = (
                "# direct methods\n"
                ".method static constructor <clinit>()V\n"
                "    .locals 1\n\n"
                "    const-string v0, \"Pvr_UnitySDK\"\n\n"
                "    invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V\n\n"
                "    return-void\n"
                ".end method\n\n"
            )
            if "<clinit>" not in content:
                content = content.replace("# direct methods\n", clinit, 1)
                unity_activity.write_text(content)
                print("injected System.loadLibrary(Pvr_UnitySDK) into UnityPlayerActivity")

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
