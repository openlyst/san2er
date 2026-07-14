# pvr-neo2-shim

Compatibility shim that lets PicoXR Platform-based Pico Neo 3 games render on a Pico Neo 2.

Audio and controllers already work on Neo 2 because they go through legacy services that are still present. The display does not work because the XR Platform runtime path expects a PicoXR OpenXR runtime that Neo 2 does not provide. This shim sits in the middle: it keeps the original `libPvr_UnitySDK.so` for sensor/controller/audio logic and intercepts the display-related calls to apply Neo 2-compatible parameters.

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
  --apk /path/to/Warplanes_Battle_over_Pacific_1.1.2.1.apk \
  --shim build/arm64-v8a/libPvr_UnitySDK.so \
  --out /path/to/Warplanes_Battle_over_Pacific_neo2.apk
```

The script will decode the APK, swap the library, fix the manifest, rebuild, and re-sign with a debug keystore.

## Status

Working on Pico Neo 2. The shim has been tested with Warplanes: Battles Over Pacific
(`pvr.sdk.version=XR Platform_1.2.4.7`) and renders at 72 FPS with TimeWarp active.
Audio, controllers, and head tracking all function.

## Next steps

- Test with additional Neo 3 titles to verify broad compatibility.
- Fill in Neo 2-specific screen/lens parameters in `src/hooks.cpp` if any game
  queries them and gets wrong values.
- Later: extend the same approach to OpenXR-based games using the new Pico
  Integration SDK 2.0.5.
