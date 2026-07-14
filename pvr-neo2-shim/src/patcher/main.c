#include "apk_check.h"
#include "elf_symbols.h"
#include "stub_gen.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *HOOK_NAMES[] = {
    "Pvr_Init",
    "Pvr_ChangeScreenParameters",
    "Pvr_SetInitActivity",
    "Pvr_SetCurrentHMDType",
    "Pvr_GetSupportHMDTypes",
    "JNI_OnLoad",
    NULL,
};

static int run_cmd(const char *argv[]) {
    printf("+");
    for (int i = 0; argv[i]; i++) printf(" %s", argv[i]);
    printf("\n");

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], (char *const *)argv);
        perror("execvp");
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "command failed with exit %d\n", WEXITSTATUS(status));
        return -1;
    }
    return 0;
}

static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return 0;
    if (mkdir(path, 0755) < 0 && errno != EEXIST) {
        perror("mkdir");
        return -1;
    }
    return 0;
}

static int ensure_keystore(const char *ks_path) {
    struct stat st;
    if (stat(ks_path, &st) == 0) return 0;

    char dir[512];
    snprintf(dir, sizeof(dir), "%s", ks_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        ensure_dir(dir);
    }

    const char *cmd[] = {
        "keytool", "-genkey", "-v",
        "-keystore", ks_path,
        "-alias", "androiddebugkey",
        "-keyalg", "RSA", "-keysize", "2048",
        "-validity", "10000",
        "-dname", "CN=Android Debug,O=Android,C=US",
        "-storepass", "android",
        "-keypass", "android",
        NULL,
    };
    return run_cmd(cmd);
}

static int build_shim(const char *shim_src_dir, const char *ndk_path,
                      const char *build_dir, const char *target,
                      char *out_shim_path) {
    char cmake_toolchain[1024];
    snprintf(cmake_toolchain, sizeof(cmake_toolchain),
             "-DCMAKE_TOOLCHAIN_FILE=%s/build/cmake/android.toolchain.cmake", ndk_path);

    const char *cmake_cfg[] = {
        "cmake",
        cmake_toolchain,
        "-DANDROID_ABI=arm64-v8a",
        "-DANDROID_PLATFORM=android-27",
        "-S", shim_src_dir,
        "-B", build_dir,
        NULL,
    };
    if (run_cmd(cmake_cfg) < 0) return -1;

    const char *cmake_build[] = {
        "cmake", "--build", build_dir, "--parallel", "--target", target, NULL,
    };
    if (run_cmd(cmake_build) < 0) return -1;

    char so_path[1024];
    snprintf(so_path, sizeof(so_path), "%s/lib%s.so", build_dir, target);
    struct stat st;
    if (stat(so_path, &st) < 0) {
        fprintf(stderr, "shim .so not found at %s\n", so_path);
        return -1;
    }

    snprintf(out_shim_path, 1024, "%s", so_path);
    return 0;
}

static int patch_manifest(const char *manifest_path) {
    FILE *f = fopen(manifest_path, "r");
    if (!f) {
        perror("fopen manifest");
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(sz + 4096);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    int changed = 0;

    if (!strstr(buf, "android.hardware.vr.headtracking")) {
        char *insert = strstr(buf, "<application");
        if (insert) {
            char feature[512];
            snprintf(feature, sizeof(feature),
                "    <uses-feature android:name=\"android.hardware.vr.headtracking\" android:required=\"true\" android:version=\"1\" />\n    ");
            size_t flen = strlen(feature);
            size_t blen = strlen(buf);
            memmove(insert + flen, insert, blen - (insert - buf) + 1);
            memcpy(insert, feature, flen);
            changed = 1;
            printf("[manifest] added android.hardware.vr.headtracking\n");
        }
    }

    char *is = strstr(buf, "com.pvr.instructionset");
    if (is) {
        char *val = strstr(is, "android:value=\"32\"");
        if (val) {
            val[strlen("android:value=\"")] = '6';
            val[strlen("android:value=\"6")] = '4';
            changed = 1;
            printf("[manifest] changed com.pvr.instructionset from 32 to 64\n");
        }
    }

    if (changed) {
        f = fopen(manifest_path, "w");
        if (!f) { perror("fopen manifest w"); free(buf); return -1; }
        fputs(buf, f);
        fclose(f);
    }

    free(buf);
    return 0;
}

static int inject_shim_pvr(const char *decoded_dir, const char *shim_path) {
    char orig_path[1024], renamed_path[1024];

    snprintf(orig_path, sizeof(orig_path), "%s/lib/arm64-v8a/libPvr_UnitySDK.so", decoded_dir);
    snprintf(renamed_path, sizeof(renamed_path), "%s/lib/arm64-v8a/libPvr_UnitySDK_orig.so", decoded_dir);

    struct stat st;
    if (stat(orig_path, &st) < 0) {
        fprintf(stderr, "libPvr_UnitySDK.so not found in decoded apk\n");
        return -1;
    }

    if (rename(orig_path, renamed_path) < 0) {
        perror("rename original");
        return -1;
    }

    const char *cp_cmd[] = { "cp", shim_path, orig_path, NULL };
    if (run_cmd(cp_cmd) < 0) return -1;

    printf("[patch] renamed original to libPvr_UnitySDK_orig.so and injected shim\n");
    return 0;
}

static int inject_shim_pxr(const char *decoded_dir, const char *shim_path) {
    char target_path[1024];

    snprintf(target_path, sizeof(target_path), "%s/lib/arm64-v8a/libpxr_api.so", decoded_dir);

    struct stat st;
    if (stat(target_path, &st) < 0) {
        fprintf(stderr, "libpxr_api.so not found in decoded apk\n");
        return -1;
    }

    /* Replace libpxr_api.so with our shim (no need to keep original) */
    const char *cp_cmd[] = { "cp", shim_path, target_path, NULL };
    if (run_cmd(cp_cmd) < 0) return -1;

    printf("[patch] replaced libpxr_api.so with PXR->PVR translation shim\n");
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s <input.apk> [output.apk]\n"
        "\n"
        "  patches a Pico Neo 3 game APK to run on Pico Neo 2.\n"
        "  supports both old PVR SDK games and OpenXR-based PXR Platform games.\n"
        "  checks compatibility, builds the shim, swaps the .so,\n"
        "  fixes the manifest, and signs the output.\n"
        "\n"
        "  options:\n"
        "    --ndk <path>     android ndk path (default: /opt/android-sdk/ndk/27.0.12077973)\n"
        "    --shim-src <dir>  shim source dir (default: auto-detect)\n"
        "    --work <dir>     temp work dir (default: ~/.pvr-neo2-shim/work)\n"
        "    --keystore <ks>  debug keystore (default: ~/.pvr-neo2-shim/debug.keystore)\n",
        prog);
}

int main(int argc, char **argv) {
    const char *apk_path = NULL;
    const char *out_path = NULL;
    const char *ndk_path = "/opt/android-sdk/ndk/27.0.12077973";
    const char *shim_src = NULL;
    const char *work_dir = NULL;
    const char *keystore = NULL;

    char home_work[512], home_ks[512], auto_src[512];
    const char *home = getenv("HOME");
    if (home) {
        snprintf(home_work, sizeof(home_work), "%s/.pvr-neo2-shim/work", home);
        snprintf(home_ks, sizeof(home_ks), "%s/.pvr-neo2-shim/debug.keystore", home);
        work_dir = home_work;
        keystore = home_ks;
    }

    int i = 1;
    while (i < argc) {
        if (!strcmp(argv[i], "--ndk") && i + 1 < argc) {
            ndk_path = argv[++i];
        } else if (!strcmp(argv[i], "--shim-src") && i + 1 < argc) {
            shim_src = argv[++i];
        } else if (!strcmp(argv[i], "--work") && i + 1 < argc) {
            work_dir = argv[++i];
        } else if (!strcmp(argv[i], "--keystore") && i + 1 < argc) {
            keystore = argv[++i];
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-' && !apk_path) {
            apk_path = argv[i];
        } else if (argv[i][0] != '-' && !out_path) {
            out_path = argv[i];
        }
        i++;
    }

    if (!apk_path) {
        usage(argv[0]);
        return 1;
    }

    if (!out_path) {
        char auto_out[1024];
        snprintf(auto_out, sizeof(auto_out), "%s", apk_path);
        char *dot = strrchr(auto_out, '.');
        if (dot) *dot = '\0';
        strncat(auto_out, "_neo2.apk", sizeof(auto_out) - strlen(auto_out) - 1);
        out_path = strdup(auto_out);
    }

    if (!shim_src) {
        char self[1024];
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0) {
            self[n] = '\0';
            for (int up = 0; up < 4; up++) {
                char *slash = strrchr(self, '/');
                if (!slash) break;
                *slash = '\0';
            }
            if (self[0]) {
                snprintf(auto_src, sizeof(auto_src), "%s", self);
                shim_src = auto_src;
            }
        }
        if (!shim_src) {
            fprintf(stderr, "cannot auto-detect shim source dir, use --shim-src\n");
            return 1;
        }
    }

    printf("=== pvr-neo2-shim patcher ===\n");
    printf("  input:     %s\n", apk_path);
    printf("  output:    %s\n", out_path);
    printf("  ndk:       %s\n", ndk_path);
    printf("  shim src:  %s\n", shim_src);
    printf("  work dir:  %s\n", work_dir);
    printf("  keystore:  %s\n", keystore);
    printf("\n");

    /* step 1: compatibility check */
    printf("--- step 1: compatibility check ---\n");
    char tmp_so[512];
    enum apk_type apk_type;
    if (apk_check_compatible(apk_path, tmp_so, sizeof(tmp_so), &apk_type) < 0) {
        fprintf(stderr, "APK is not compatible with the shim\n");
        return 1;
    }

    const char *shim_target = NULL;
    const char *shim_libname = NULL;

    if (apk_type == APK_TYPE_PVR_SDK) {
        shim_target = "Pvr_UnitySDK";
        shim_libname = "libPvr_UnitySDK.so";
        printf("[type] PVR SDK game (old PVR runtime)\n");
    } else {
        shim_target = "pxr_api";
        shim_libname = "libpxr_api.so";
        printf("[type] PXR Platform game (OpenXR-based)\n");
    }

    /* step 2: for PVR SDK, extract symbols; for PXR Platform, skip */
    elf_symbol_list_t symbols = {0};

    if (apk_type == APK_TYPE_PVR_SDK) {
        printf("\n--- step 2: extract exported symbols ---\n");
        if (elf_extract_pvr_symbols(tmp_so, &symbols) < 0) {
            fprintf(stderr, "failed to extract symbols\n");
            unlink(tmp_so);
            return 1;
        }
        printf("[symbols] found %d exported functions\n", symbols.count);
        unlink(tmp_so);

        if (symbols.count == 0) {
            fprintf(stderr, "no compatible symbols found\n");
            return 1;
        }

        printf("\n--- step 3: generate forward stubs ---\n");
        char stubs_path[1024], vars_path[1024];
        snprintf(stubs_path, sizeof(stubs_path), "%s/src/generated/forward_stubs.S", shim_src);
        snprintf(vars_path, sizeof(vars_path), "%s/src/generated/forward_vars.cpp", shim_src);

        if (stub_gen_generate(&symbols, HOOK_NAMES, stubs_path, vars_path) < 0) {
            fprintf(stderr, "failed to generate stubs\n");
            return 1;
        }
        printf("[stubs] generated %s and %s\n", stubs_path, vars_path);
    } else {
        printf("\n--- step 2/3: skipped (PXR Platform uses static shim) ---\n");
    }

    /* step 4: build the shim */
    printf("\n--- step 4: build shim ---\n");
    char build_dir[1024];
    snprintf(build_dir, sizeof(build_dir), "%s/build", shim_src);
    char shim_out[1024];
    if (build_shim(shim_src, ndk_path, build_dir, (char *)shim_target, shim_out) < 0) {
        fprintf(stderr, "failed to build shim\n");
        return 1;
    }
    printf("[build] shim at %s\n", shim_out);

    /* step 5: decode APK with apktool */
    printf("\n--- step 5: decode APK ---\n");
    ensure_dir(work_dir);
    char decoded_dir[1024];
    snprintf(decoded_dir, sizeof(decoded_dir), "%s/decoded_%d", work_dir, (int)getpid());

    const char *decode_cmd[] = {
        "apktool", "decode", "-f", "-o", decoded_dir, apk_path, NULL,
    };
    if (run_cmd(decode_cmd) < 0) {
        fprintf(stderr, "apktool decode failed\n");
        return 1;
    }

    /* step 6: patch manifest + inject shim */
    printf("\n--- step 6: patch manifest + inject shim ---\n");
    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/AndroidManifest.xml", decoded_dir);
    patch_manifest(manifest_path);

    if (apk_type == APK_TYPE_PVR_SDK) {
        inject_shim_pvr(decoded_dir, shim_out);
    } else {
        inject_shim_pxr(decoded_dir, shim_out);
    }

    /* step 7: rebuild APK */
    printf("\n--- step 7: rebuild APK ---\n");
    char unsigned_apk[1024], aligned_apk[1024];
    snprintf(unsigned_apk, sizeof(unsigned_apk), "%s/unsigned.apk", work_dir);
    snprintf(aligned_apk, sizeof(aligned_apk), "%s/aligned.apk", work_dir);

    const char *build_cmd[] = {
        "apktool", "build", "-o", unsigned_apk, decoded_dir, NULL,
    };
    if (run_cmd(build_cmd) < 0) {
        fprintf(stderr, "apktool build failed\n");
        return 1;
    }

    /* step 8: zipalign */
    printf("\n--- step 8: zipalign ---\n");
    const char *align_cmd[] = {
        "zipalign", "-p", "-f", "4", unsigned_apk, aligned_apk, NULL,
    };
    if (run_cmd(align_cmd) < 0) {
        fprintf(stderr, "zipalign failed\n");
        return 1;
    }

    /* step 9: sign */
    printf("\n--- step 9: sign ---\n");
    ensure_keystore(keystore);
    const char *sign_cmd[] = {
        "apksigner", "sign",
        "--ks", keystore,
        "--ks-pass", "pass:android",
        "--key-pass", "pass:android",
        "--out", out_path,
        aligned_apk,
        NULL,
    };
    if (run_cmd(sign_cmd) < 0) {
        fprintf(stderr, "apksigner failed\n");
        return 1;
    }

    /* cleanup */
    const char *rm_cmd[] = { "rm", "-rf", decoded_dir, NULL };
    run_cmd(rm_cmd);
    unlink(unsigned_apk);
    unlink(aligned_apk);

    printf("\n=== done ===\n");
    printf("patched APK: %s\n", out_path);
    return 0;
}
