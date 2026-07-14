#include "apk_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int apk_has_entry(const char *apk_path, const char *entry_name) {
    int err = 0;
    zip_t *z = zip_open(apk_path, ZIP_RDONLY, &err);
    if (!z) return -1;

    int idx = zip_name_locate(z, entry_name, 0);
    zip_close(z);
    return idx >= 0 ? 0 : -1;
}

int apk_extract_file(const char *apk_path, const char *entry_name,
                     const char *out_path) {
    int err = 0;
    zip_t *z = zip_open(apk_path, ZIP_RDONLY, &err);
    if (!z) {
        fprintf(stderr, "cannot open apk: %s\n", apk_path);
        return -1;
    }

    zip_file_t *zf = zip_fopen(z, entry_name, 0);
    if (!zf) {
        fprintf(stderr, "cannot find %s in apk\n", entry_name);
        zip_close(z);
        return -1;
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        perror("fopen out");
        zip_fclose(zf);
        zip_close(z);
        return -1;
    }

    char buf[65536];
    zip_int64_t n;
    while ((n = zip_fread(zf, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, (size_t)n, out);
    }

    fclose(out);
    zip_fclose(zf);
    zip_close(z);
    return 0;
}

int apk_check_compatible(const char *apk_path, char *tmp_so_path,
                         size_t tmp_so_len, enum apk_type *type) {
    *type = APK_TYPE_UNKNOWN;

    /* Try PVR SDK first: libPvr_UnitySDK.so */
    const char *pvr_so = "lib/arm64-v8a/libPvr_UnitySDK.so";
    if (apk_has_entry(apk_path, pvr_so) == 0) {
        printf("[check] found libPvr_UnitySDK.so (PVR SDK game)\n");
        *type = APK_TYPE_PVR_SDK;

        snprintf(tmp_so_path, tmp_so_len, "/tmp/pvr_shim_orig_XXXXXX.so");
        int fd = mkstemps(tmp_so_path, 3);
        if (fd < 0) { perror("mkstemps"); return -1; }
        close(fd);

        if (apk_extract_file(apk_path, pvr_so, tmp_so_path) < 0) {
            unlink(tmp_so_path);
            return -1;
        }
        printf("[check] extracted %s\n", pvr_so);
        return 0;
    }

    /* Try PXR Platform: libPxrPlatform.so + libpxr_api.so */
    const char *pxr_api_so = "lib/arm64-v8a/libpxr_api.so";
    const char *pxr_platform_so = "lib/arm64-v8a/libPxrPlatform.so";
    if (apk_has_entry(apk_path, pxr_api_so) == 0 &&
        apk_has_entry(apk_path, pxr_platform_so) == 0) {
        printf("[check] found libPxrPlatform.so + libpxr_api.so (PXR Platform game)\n");
        *type = APK_TYPE_PXR_PLATFORM;

        /* For PXR Platform, we don't need to extract symbols from libpxr_api.so
           since our shim implements all functions directly. */
        return 0;
    }

    fprintf(stderr, "incompatible: no PVR SDK or PXR Platform libraries found\n");
    return -1;
}
