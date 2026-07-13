# Shim Design

## Problem statement

Early Pico Neo 3 games built with the legacy PicoVR Unity SDK install on a Pico Neo 2 and have working audio and controllers, but the display stays black. The controllers/audio work because they route through legacy services (`HummingBirdControllerService`, Android audio). The display fails because the VR compositor initialization path is not compatible with the Neo 2 runtime.

## Target scope

This shim is designed for games that:

- Import from `libPvr_UnitySDK.so` (legacy PicoVR SDK, not the new `PxrPlatform` / `pxr_api` libraries).
- Use `com.unity3d.player.UnityPlayerNativeActivityPico`.
- Are compiled with IL2CPP (so the set of imported `Pvr_*` functions is discoverable in `libil2cpp.so`).

New OpenXR-based games are out of scope for this first iteration and will be handled separately.

## Intercept strategy

We replace the game's `libPvr_UnitySDK.so` with our shim and ship the original under `libPvr_UnitySDK_orig.so`. The shim has two parts:

1. **Trampolines for every imported function**: generated arm64 stubs that jump to the original function pointer. This lets sensor, controller, audio, and boundary logic keep working untouched.
2. **Hooks for display-critical functions**: hand-written C++ wrappers that call the original and then adjust behavior for Neo 2.

The hook list is intentionally small:

- `Pvr_Init` — detect or force the correct HMD type / device model.
- `Pvr_ChangeScreenParameters` — supply Neo 2 display/lens parameters if the original call does not match.
- `Pvr_SetInitActivity` — ensure the Android `Activity` / surface are compatible with Neo 2.
- `Pvr_SetCurrentHMDType` — reject unsupported HMD types and fall back to a known-good Neo 2 profile.
- `Pvr_GetSupportHMDTypes` — optionally lie about supported devices so the game picks the Neo 2 profile.
- Display-frequency helpers (`Pvr_GetRefreshRate`, `Pvr_SetRefreshRate`, `Pvr_GetDisplayFrequenciesAvailable`, `Pvr_SetDisplayFrequency`) — clamp to Neo 2 refresh rates.
- Single-pass / foveation helpers — disable or clamp features the Neo 2 GPU/runtime may not support.

## Why assembly trampolines instead of C++ wrappers for everything

A game may import 50-100 `Pvr_*` functions. Writing typed C++ wrappers for every one would require exact signatures from every SDK version. A tiny `adrp`/`ldr`/`br` trampoline does not care about the signature, so it is generated automatically and is robust against SDK differences.

## Hook implementation

Each hook is declared `extern "C"` with the same name the game imports. The generated trampoline generator skips any function that has a hook definition, so the dynamic linker resolves the game's reference to the hook instead of the trampoline.

Hooks call the original via a function pointer stored in `loader.cpp`. The loader opens `libPvr_UnitySDK_orig.so` with `dlopen` and fills a resolver table in a constructor.

## Manifest fixes

The patcher also applies two small manifest changes that are known to cause black-screen issues on Neo 2:

- Change `com.pvr.instructionset` from `32` to `64` if the APK only ships `arm64-v8a` libraries.
- Add `android.hardware.vr.headtracking` if missing.

These are not part of the native shim but are required for the Neo 2 runtime to load the VR plugin correctly.

## Security / legal note

The patcher only works on APKs you already own. It does not remove store entitlement checks, DRM, or licensing. It is a compatibility layer for running your own games on your own hardware.
