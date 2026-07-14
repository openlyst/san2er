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

int apk_check_compatible(const char *apk_path, char *tmp_so_path, size_t tmp_so_len) {
    /* check 1: must have arm64-v8a libPvr_UnitySDK.so */
    const char *so_entry = "lib/arm64-v8a/libPvr_UnitySDK.so";
    if (apk_has_entry(apk_path, so_entry) < 0) {
        fprintf(stderr, "incompatible: no lib/arm64-v8a/libPvr_UnitySDK.so found\n");
        return -1;
    }
    printf("[check] found libPvr_UnitySDK.so (arm64-v8a)\n");

    /* check 2: must have libil2cpp.so (IL2CPP backend) */
    if (apk_has_entry(apk_path, "lib/arm64-v8a/libil2cpp.so") < 0) {
        fprintf(stderr, "warning: no libil2cpp.so found, game may not be IL2CPP\n");
    } else {
        printf("[check] found libil2cpp.so\n");
    }

    /* extract the .so to a temp file for symbol extraction */
    snprintf(tmp_so_path, tmp_so_len, "/tmp/pvr_shim_orig_XXXXXX.so");
    int fd = mkstemps(tmp_so_path, 3);
    if (fd < 0) {
        perror("mkstemps");
        return -1;
    }
    close(fd);

    if (apk_extract_file(apk_path, so_entry, tmp_so_path) < 0) {
        unlink(tmp_so_path);
        return -1;
    }

    printf("[check] extracted libPvr_UnitySDK.so to %s\n", tmp_so_path);
    return 0;
}
