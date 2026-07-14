cmake_minimum_required(VERSION 3.10.2)
project(pvr_neo2_shim LANGUAGES C CXX ASM)

add_library(Pvr_UnitySDK SHARED
    src/loader.cpp
    src/hooks.cpp
    src/generated/forward_stubs.S
    src/generated/forward_vars.cpp
)

target_include_directories(Pvr_UnitySDK PRIVATE src)

find_library(log-lib log)
find_library(dl-lib dl)
target_link_libraries(Pvr_UnitySDK ${log-lib} ${dl-lib})

set_target_properties(Pvr_UnitySDK PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
