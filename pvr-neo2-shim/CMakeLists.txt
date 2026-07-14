cmake_minimum_required(VERSION 3.10.2)
project(pvr_neo2_shim LANGUAGES C CXX ASM)

# --- Old PVR SDK shim (replaces libPvr_UnitySDK.so in old games) ---
add_library(Pvr_UnitySDK SHARED
    src/loader.cpp
    src/hooks.cpp
    src/generated/forward_stubs.S
    src/generated/forward_vars.cpp
)

target_include_directories(Pvr_UnitySDK PRIVATE src)

find_library(log-lib log)
find_library(dl-lib dl)
find_library(egl-lib EGL)
find_library(gles-lib GLESv2)

target_link_libraries(Pvr_UnitySDK ${log-lib} ${dl-lib})

set_target_properties(Pvr_UnitySDK PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# --- PXR Platform shim (replaces libpxr_api.so in OpenXR-based games) ---
add_library(pxr_api SHARED
    src/pxr_api_shim.c
)

target_include_directories(pxr_api PRIVATE src)

target_link_libraries(pxr_api ${log-lib} ${dl-lib} ${egl-lib} ${gles-lib})

set_target_properties(pxr_api PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)
