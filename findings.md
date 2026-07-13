# Pico Neo 3 Games on Pico Neo 2 — No Display Issue

## Summary

When installing Neo 3 games on a Pico Neo 2, the app launches and audio/controller input works, but the headset display stays black. Investigation of the provided SDKs and game APKs points to the VR display initialization path failing on Neo 2, while audio and controller services continue to function through separate, older services.

## Hardware / SDK Context

| Headset | SoC | Runtime Era | Relevant SDK in this repo |
|---------|-----|-------------|---------------------------|
| Pico Neo 2 | Snapdragon 845 / Adreno 630 | Legacy PicoVR / WaveVR / early OpenXR | `sdks/neo2/PicoVR_Unity_SDK_2.8.12_B583.zip`, `PicoXR_Platform_SDK-1.2.5_B81.zip`, `Pico_OpenXR_Mobile_SDK_2.0.1.zip` |
| Pico Neo 3 | Snapdragon XR2 | PicoXR OpenXR runtime | `sdks/neo3 sdk/Pico Unity Integration SDK-2.0.5` |

## Key Findings

### 1. The provided "Neo 3" SDK targets a runtime Neo 2 does not have

`sdks/neo3 sdk/Pico Unity Integration SDK-2.0.5/Runtime/Scripts/PXR_Loader.cs` initializes the XR display subsystem by calling:

- `PXR_Plugin.System.UPxr_Construct(...)`
- `PXR_Plugin.System.UPxr_SetUserDefinedSettings(...)`
- `CreateSubsystem<XRDisplaySubsystemDescriptor, XRDisplaySubsystem>(..., "PicoXR Display")`

The native plugin `libPxrPlatform.so` (extracted from `Runtime/Android/PxrPlatform.aar`) contains log strings showing it binds to:

- `com.pico.xr.openxr_runtime`
- `com.pico.xr.openxr_runtime.DriverLoader`
- `com/pico/xr/openxr_runtime/VrDriver`

On Neo 2 this newer PicoXR OpenXR runtime is either absent or exposes a different API version, so the display subsystem fails to initialize. The loader explicitly logs:

```
PXR Plugin Failed to initialize Pxr plugin
PXR Failed to initialize PicoVR system.
PXR Unable to retrieve XR Display Interface.
```

Controllers and audio still work because input is registered through `PicoXR Input` / `PicoXR HMD` / `Pico Neo` / `Pico G` layouts and the legacy `HummingBirdControllerService`, and Android audio is independent of the VR compositor.

### 2. The actual Neo 3 game APK is built with an older SDK

`games/neo3/Super_Hot_1.90_FNAL.apk` decodes to a manifest with:

```xml
<meta-data android:name="pvr.sdk.version" android:value="Unity_2.8.9.12"/>
<activity android:name="com.unity3d.player.UnityPlayerNativeActivityPico" ... />
<meta-data android:name="com.pvr.instructionset" android:value="32"/>
```

This is the legacy PicoVR Unity SDK, **not** the new Pico Integration SDK 2.0.5. Its native library `libPvr_UnitySDK.so` contains the build path `PicoSDK_Unity-dev_phoenix` and a hardcoded supported-device string:

```
Pico1,Pico1S,Pico2
```

It does **not** list a Neo 3 device name, and it uses `ScreenParameters::findScreenByModel(...)` to pick lens/display parameters from the VR service. If the Neo 2 model string is not matched, display parameters may not be set, leaving the compositor with no valid output.

Additionally, the manifest declares `com.pvr.instructionset` = `32` while the APK only contains `arm64-v8a` native libraries. That mismatch can cause the VR runtime to fail to load the display plugin even though the package installs.

### 3. The working Neo 2 game uses a different manifest / activity

`games/neo2/Warplanes_Battle_over_Pacific_1.1.2.1.apk` uses:

```xml
<activity android:name="com.unity3d.player.UnityPlayerActivity" ... />
<uses-feature android:name="android.hardware.vr.headtracking" android:required="true" android:version="1"/>
```

It does **not** set `com.pvr.instructionset`, and it includes WaveVR native libraries (`libwvr*.so`). This aligns with the Neo 2 runtime and is why it renders correctly.

## Why Audio and Controllers Work

- **Controllers**: Both SDKs bind to `com.picovr.picovrlib.hummingbird.HummingBirdControllerService` and register generic Pico controller layouts. This path is still alive on Neo 2.
- **Audio**: Unity audio runs through standard Android AudioTrack/OpenSL ES and does not depend on the VR display subsystem.
- **Display**: Rendering goes through the Pico VR compositor / TimeWarp path, which is the part that fails when the runtime or device model is not recognized.

## Recommended Fixes

### If you have the Unity source

Rebuild the game for Neo 2 using one of the Neo 2 SDKs:

- `sdks/neo2/PicoNeo2-SDKs-EXEs/Unity+Plugin/PicoVR_Unity_SDK_2.8.12_B583.zip`
- `sdks/neo2/PicoNeo2-SDKs-EXEs/Unity+Plugin/PicoXR_Platform_SDK-1.2.5_B81.zip`

Use the standard `UnityPlayerActivity`, include WaveVR libraries if required, and target OpenGL ES 3.0. Do **not** use `UnityPlayerNativeActivityPico` or the new Pico Integration SDK 2.0.5.

### If you only have the APK

Patching is hit-or-miss, but the most likely blockers are:

1. Remove or change `<meta-data android:name="com.pvr.instructionset" android:value="32"/>` to `64` so it matches the shipped arm64 libraries.
2. Add `<uses-feature android:name="android.hardware.vr.headtracking" android:required="true" android:version="1"/>` if the Neo 2 runtime requires it.
3. Repack, re-sign, and test on device.

If the native activity `UnityPlayerNativeActivityPico` is hard-coded to assume a Neo 3-era runtime, manifest changes alone may not be enough.

## Files Referenced

- `sdks/neo3 sdk/Pico Unity Integration SDK-2.0.5/Runtime/Scripts/PXR_Loader.cs`
- `sdks/neo3 sdk/Pico Unity Integration SDK-2.0.5/Runtime/Scripts/PXR_Manager.cs`
- `sdks/neo3 sdk/Pico Unity Integration SDK-2.0.5/Runtime/Android/PxrPlatform.aar`
- `sdks/neo3 sdk/Pico Unity Integration SDK-2.0.5/Runtime/Android/pxr_api-release.aar`
- `sdks/neo2/PicoNeo2-SDKs-EXEs/Unity+Plugin/PicoVR_Unity_SDK_2.8.12_B583.zip`
- `games/neo3/Super_Hot_1.90_FNAL.apk`
- `games/neo2/Warplanes_Battle_over_Pacific_1.1.2.1.apk`
