# pvr-neo2-shim

Compatibility shim that lets PicoXR Platform (PXR) based Pico Neo 3 games run on a Pico Neo 2 headset.

The Neo 2 ships with the older PVR Unity SDK runtime, not the PXR Platform / OpenXR runtime that Neo 3 games expect. This shim translates PXR Platform API calls to the PVR runtime calls that Neo 2 provides, so the game loads and renders without needing the actual PXR runtime.

## How it works

1. The C patcher (`src/patcher/`) reads the input APK and checks for `lib/arm64-v8a/libpxr_api.so` (PXR Platform) or `libPvr_UnitySDK.so` (PVR SDK).
2. For PXR Platform games, it injects `libpxr_api.so` — our shim that implements all PXR Platform exports by forwarding to the PVR runtime (`libPvr_UnitySDK_compat.so`).
3. The shim loads the PVR compat library via `dlopen`, resolves all PVR functions via `dlsym`, and translates calls at runtime.
4. Key translations:
   - `Pxr_Initialize` → loads PVR runtime, inits sensor
   - `Pxr_CreateLayer` → creates PVR eye textures, sets up layer data
   - `Pxr_PollEvent` → polls PVR sensor state, submits frames to TimeWarp
   - `Pxr_GetMainSensorState` → wraps PVR sensor state with Neo 2 eye poses
   - `Pxr_GetControllerState*` → wraps PVR controller state
5. The patcher decodes the APK with apktool, injects the shim, fixes the manifest, rebuilds, and re-signs.

## Project layout

```
pvr-neo2-shim/
  CMakeLists.txt          # shim .so build (cross-compiled for arm64)
  build.sh                # builds both the patcher and the shim
  README.md
  src/
    pxr_api_shim.c        # main shim implementation (~2400 lines)
    java/                 # Java helpers (VrActivity for PVR init)
    patcher/              # host-side C patcher tool
      main.c
      apk_check.c / .h
      CMakeLists.txt
  prebuilt/               # prebuilt PVR runtime libs for Neo 2
  docs/
    design.md
```

## Build

```bash
cd pvr-neo2-shim
./build.sh
```

This builds the C patcher and the shim library. The NDK must be installed (default path: `/opt/android-sdk/ndk/27.0.12077973`).

## Patch a game

```bash
src/patcher/build/pvr-patcher <input.apk> [output.apk]
```

The patcher will:
1. Check that the APK has the expected PXR/PVR native libraries
2. Build the shim `.so` for arm64
3. Decode the APK, inject the shim, fix the manifest
4. Rebuild, zipalign, and sign the output

If no output path is given, it appends `_neo2.apk` to the input name.

Options:
```
  --ndk <path>      Android NDK path (default: /opt/android-sdk/ndk/27.0.12077973)
  --shim-src <dir>  Shim source dir (default: auto-detect)
  --work <dir>      Temp work dir (default: ~/.pvr-neo2-shim/work)
  --keystore <ks>   Debug keystore (default: ~/.pvr-neo2-shim/debug.keystore)
```

## Status

Work in progress. The shim successfully patches and runs Touring Karts (PXR Platform based) on Neo 2. Sensor data, controllers, and audio work. Rendering is being debugged — the PVR TimeWarp is active and processing frames, but getting Unity's rendered output to the eye textures requires correct texture ID passing through the PVR SDK's internal struct.

### Current rendering approach

The PVR SDK uses a large internal struct (`up`) to pass texture IDs between render events. From Ghidra decompilation of `UnityRenderEvent_`:
- Event `0x40c` (BOTH_EYE_END_FRAME) reads `up+0xf960` and calls `PVR_CameraEndFrame` which writes `up+0x4c` (left eye tex)
- Event `0x405` (TIMEWARP) reads `up+0xf928` as a frame index (must be <= 63), then reads `up+0x4c` and copies to `up+0x90` (LE_TexID) before calling `pvr_WarpSwap`
- `up+0x68` must be set to 1 for `PVR_CameraEndFrame` to write

The shim wraps `UnityRenderEvent` to set these struct fields before each render event call.

## Next steps

- Get Unity's rendered content to appear in the eye textures
- Test with additional PXR Platform based Neo 3 titles
- Verify 6DoF tracking works correctly with the shimmed sensor state
