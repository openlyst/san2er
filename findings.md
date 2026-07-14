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

### 2. The actual Neo 3 game uses the newer XR Platform SDK

`games/neo3/Warplanes_Battle_over_Pacific_1.1.2.1.apk` decodes to a manifest with:

```xml
<meta-data android:name="pvr.sdk.version" android:value="XR Platform_1.2.4.7"/>
<activity android:name="com.unity3d.player.UnityPlayerActivity" ... />
<uses-feature android:name="android.hardware.vr.headtracking" android:required="true" android:version="1"/>
```

This is the PicoXR Platform SDK (the 1.2.x line), not the legacy PicoVR Unity SDK and not the newer Pico Integration SDK 2.0.5. Its native library `libPvr_UnitySDK.so` was built from the `PicoXR_Platform_Unity_SDK` branch (visible in the embedded build paths), and the APK ships WaveVR native libraries alongside it:

```
lib/arm64-v8a/libwvr_api.so
lib/arm64-v8a/libwvrunity.so
lib/arm64-v8a/libwvrunityxr.so
lib/arm64-v8a/libwvrugl.so
lib/arm64-v8a/libwvr_client_bootstrap.so
lib/arm64-v8a/libUnityPicoVR.so
lib/arm64-v8a/libGfxWXRUnity.so
```

The `libPvr_UnitySDK.so` still carries the same hardcoded supported-device string as the legacy SDK:

```
Pico1,Pico1S,Pico2
```

and uses `ScreenParameters::findScreenByModel(...)` to pick lens/display parameters from the VR service. On Neo 2 the XR Platform runtime path is not available, so the display subsystem never comes up. The manifest does not declare `com.pvr.instructionset`, and the activity is the standard `UnityPlayerActivity` rather than `UnityPlayerNativeActivityPico`.

### 3. The native Neo 2 game uses the legacy PicoVR SDK

`games/neo2/Super_Hot_1.90_FNAL.apk` is a native Neo 2 title (Chinese market release). Its manifest:

```xml
<meta-data android:name="pvr.sdk.version" android:value="Unity_2.8.9.12"/>
<activity android:name="com.unity3d.player.UnityPlayerNativeActivityPico" ... />
<meta-data android:name="com.pvr.instructionset" android:value="32"/>
<meta-data android:name="com.pvr.hmd.trackingmode" android:value="6dof"/>
```

This is the legacy PicoVR Unity SDK (the deprecated pre-XR-Platform line). Its `libPvr_UnitySDK.so` was built from the `PicoSDK_Unity-dev_phoenix` branch. The APK includes the Pico Chinese-market login/pay SDK (`com.pico.loginpaysdk`), confirming it was a domestic Neo 2 release. It renders correctly on Neo 2 because it targets the legacy runtime that Neo 2 still provides.

The manifest declares `com.pvr.instructionset` = `32` while the APK only contains `arm64-v8a` native libraries. This does not cause a problem on Neo 2 because the legacy runtime tolerates it, but it is worth noting for comparison with the Neo 3 game.

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

Manifest changes alone are not enough. Empirical testing on a black-screen + audio Neo 3 game showed that manifest patching did not fix the display. A working approach requires:

1. Swap `.so` files — replace the Neo 3 `libPvr_UnitySDK.so` (and related PicoXR Platform libs) with Neo 2-compatible equivalents, or intercept calls via a shim.
2. Patch the Vulkan/rendering path if the game targets a graphics API version the Neo 2 runtime does not expose.
3. Patch `libil2cpp.so` to fix any hardcoded XR Platform calls that bypass the native library indirection.
4. Fix manifest metadata as a secondary step (instructionset, headtracking feature, etc.).
5. Repack, re-sign, and test on device.

The `pvr-neo2-shim` in this repo implements step 1 by replacing `libPvr_UnitySDK.so` with a forwarding shim that intercepts display-critical calls and applies Neo 2 parameters, while passing everything else through to the renamed original.

## Files Referenced

- `sdks/neo3 sdk/Pico Unity Integration SDK-2.0.5/Runtime/Scripts/PXR_Loader.cs`
- `sdks/neo3 sdk/Pico Unity Integration SDK-2.0.5/Runtime/Scripts/PXR_Manager.cs`
- `sdks/neo3 sdk/Pico Unity Integration SDK-2.0.5/Runtime/Android/PxrPlatform.aar`
- `sdks/neo3 sdk/Pico Unity Integration SDK-2.0.5/Runtime/Android/pxr_api-release.aar`
- `sdks/neo2/PicoNeo2-SDKs-EXEs/Unity+Plugin/PicoVR_Unity_SDK_2.8.12_B583.zip`
- `games/neo3/Warplanes_Battle_over_Pacific_1.1.2.1.apk`
- `games/neo2/Super_Hot_1.90_FNAL.apk`
