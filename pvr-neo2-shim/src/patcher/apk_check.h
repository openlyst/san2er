#ifndef APK_CHECK_H
#define APK_CHECK_H

#include <zip.h>

/* Check if an APK is compatible with the shim.
   Returns 0 if compatible, -1 otherwise.
   On success, extracts libPvr_UnitySDK.so to tmp_so_path. */
int apk_check_compatible(const char *apk_path, char *tmp_so_path, size_t tmp_so_len);

/* Extract a file from an APK/ZIP to a temp path.
   Returns 0 on success. */
int apk_extract_file(const char *apk_path, const char *entry_name,
                     const char *out_path);

/* Check if an APK contains a specific entry. */
int apk_has_entry(const char *apk_path, const char *entry_name);

#endif
