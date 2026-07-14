# pvr-neo2-shim

Compatibility shim that lets PicoXR Platform-based Pico Neo 3 games render on a Pico Neo 2.

Audio and controllers already work on Neo 2 because they go through legacy services that are still present. The display does not work because the XR Platform runtime path expects a PicoXR OpenXR runtime that Neo 2 does not provide. This shim sits in the middle: it keeps the original `libPvr_UnitySDK.so` for sensor/controller/audio logic and intercepts the display-related calls to apply Neo 2-compatible parameters.

## How it works

1. The C patcher (`src/patcher/`) reads the input APK, checks for `lib/arm64-v8a/libPvr_UnitySDK.so`, and extracts it.
2. It parses the ELF export table to find all `Pvr_`/`PVR_`/`Java_`/`JNI_` functions.
3. It generates arm64 assembly trampolines and a C++ resolver table for every exported function.
4. It builds the shim `.so` with the Android NDK.
5. `src/hooks.cpp` overrides the subset of functions that affect display initialization (`Pvr_Init`, `Pvr_ChangeScreenParameters`, `Pvr_SetInitActivity`, `Pvr_SetCurrentHMDType`, etc.).
6. `src/loader.cpp` loads the renamed original `libPvr_UnitySDK_orig.so` and resolves all function pointers at load time.
7. The patcher decodes the APK with apktool, swaps the library, fixes the manifest, rebuilds, and re-signs.

## Project layout

```
pvr-neo2-shim/
  CMakeLists.txt          # shim .so build (cross-compiled for arm64)
  build.sh                # builds both the patcher and (optionally) the shim
  README.md
  src/
    loader.cpp / loader.h
    hooks.cpp / hooks.h
    log.h
    generated/            # populated by the patcher or generate_forward_stubs.py
      forward_stubs.S
      forward_vars.cpp
    patcher/              # host-side C patcher tool
      main.c
      apk_check.c / .h    # ZIP reading, compatibility check
      elf_symbols.c / .h  # ELF export table parsing
      stub_gen.c / .h     # assembly + C++ stub generation
      CMakeLists.txt
  tools/                  # legacy Python tools (still work, patcher replaces them)
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

This builds the C patcher. To also pre-build the shim for a specific APK, pass it as an argument: `./build.sh game.apk`.

## Patch a game

```bash
src/patcher/build/pvr-patcher <input.apk> [output.apk]
```

The patcher will:
1. Check that the APK has `lib/arm64-v8a/libPvr_UnitySDK.so` (compatibility check)
2. Extract exported symbols from the original `.so`
3. Generate and build the shim
4. Decode the APK, swap the library, fix the manifest
5. Rebuild, zipalign, and sign the output

If no output path is given, it appends `_neo2.apk` to the input name.

Options:
```
  --ndk <path>      Android NDK path (default: /opt/android-sdk/ndk/27.0.12077973)
  --shim-src <dir>  Shim source dir (default: auto-detect)
  --work <dir>      Temp work dir (default: ~/.pvr-neo2-shim/work)
  --keystore <ks>   Debug keystore (default: ~/.pvr-neo2-shim/debug.keystore)
```

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
