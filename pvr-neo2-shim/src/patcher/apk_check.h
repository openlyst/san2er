#ifndef APK_CHECK_H
#define APK_CHECK_H

#include <zip.h>

/* APK type detected during compatibility check */
enum apk_type {
    APK_TYPE_UNKNOWN = 0,
    APK_TYPE_PVR_SDK,      /* old PVR SDK game: ships libPvr_UnitySDK.so */
    APK_TYPE_PXR_PLATFORM, /* OpenXR-based game: ships libPxrPlatform.so + libpxr_api.so */
};

/* Check if an APK is compatible with the shim.
   Returns 0 if compatible, -1 otherwise.
   Sets *type to the detected APK type.
   For PVR_SDK: extracts libPvr_UnitySDK.so to tmp_so_path.
   For PXR_PLATFORM: extracts libpxr_api.so to tmp_so_path. */
int apk_check_compatible(const char *apk_path, char *tmp_so_path,
                         size_t tmp_so_len, enum apk_type *type);

/* Extract a file from an APK/ZIP to a temp path.
   Returns 0 on success. */
int apk_extract_file(const char *apk_path, const char *entry_name,
                     const char *out_path);

/* Check if an APK contains a specific entry. */
int apk_has_entry(const char *apk_path, const char *entry_name);

#endif
