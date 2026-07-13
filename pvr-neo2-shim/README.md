# pvr-neo2-shim

Compatibility shim that lets early PicoVR-based "Pico Neo 3" games render on a Pico Neo 2.

Audio and controllers already work on Neo 2 because they go through legacy services that are still present. The display does not work because the VR compositor / display-initialization path expects either the newer PicoXR OpenXR runtime (for new-SDK games) or the legacy `UnityPlayerNativeActivityPico` path configured for a device the Neo 2 runtime does not recognize (for old-SDK games). This shim sits in the middle: it keeps the original `libPvr_UnitySDK.so` for sensor/controller/audio logic and intercepts the display-related calls to apply Neo 2-compatible parameters.

## How it works

1. `tools/extract_pvr_functions.py` reads the game's IL2CPP binary (`libil2cpp.so`) and the PicoVR SDK C# scripts to discover exactly which `Pvr_*` functions the game imports from `libPvr_UnitySDK.so`.
2. `tools/generate_forward_stubs.py` builds an arm64 assembly stub file plus a C++ resolver table. Every discovered function gets a tiny trampoline that jumps to the corresponding original function pointer.
3. `src/hooks.cpp` overrides the subset of those functions that affect display initialization (`Pvr_Init`, `Pvr_ChangeScreenParameters`, `Pvr_SetInitActivity`, `Pvr_SetCurrentHMDType`, etc.). The hook calls the original, then applies or fixes Neo 2 parameters.
4. `src/loader.cpp` loads the renamed original `libPvr_UnitySDK_orig.so` and resolves all function pointers at load time.
5. `tools/patch_apk.py` repackages a target APK: it renames the original `libPvr_UnitySDK.so` to `libPvr_UnitySDK_orig.so`, injects the shim as `libPvr_UnitySDK.so`, fixes manifest metadata, and re-signs.

## Project layout

```
pvr-neo2-shim/
  CMakeLists.txt
  build.sh
  README.md
  src/
    loader.cpp / loader.h
    hooks.cpp / hooks.h
    log.h
    generated/          # populated by generate_forward_stubs.py
      forward_stubs.S
      forward_vars.cpp
  tools/
    extract_pvr_functions.py
    generate_forward_stubs.py
    patch_apk.py
  docs/
    design.md
```

## Build

```bash
cd pvr-neo2-shim
./build.sh
```

`build.sh` uses the Android NDK installed at `/opt/android-sdk/ndk`. It defaults to `arm64-v8a` and API 27, which matches the provided game APKs.

## Patch a game

```bash
./tools/patch_apk.py \
  --apk /path/to/Super_Hot_1.90_FNAL.apk \
  --shim build/arm64-v8a/libPvr_UnitySDK.so \
  --out /path/to/Super_Hot_1.90_FNAL_neo2.apk
```

The script will decode the APK, swap the library, fix the manifest, rebuild, and re-sign with a debug keystore.

## Status

This is the first cut of the shim. It is not yet tested on a live Neo 2 because the headset is currently in use. The architecture is in place; the exact Neo 2 display parameters and the precise set of calls to intercept will be finalized once we can run logcat and iterate on device.

## Next steps

- Capture `adb logcat` from the patched APK running on Neo 2.
- Identify which intercepted call still fails or returns Neo 3 parameters.
- Fill in the Neo 2 screen/lens parameters in `src/hooks.cpp`.
- Later: extend the same approach to OpenXR-based games using the new Pico Integration SDK.
