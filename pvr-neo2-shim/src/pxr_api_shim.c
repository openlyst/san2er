/*
 * pxr_api_shim.c - Translation layer from PXR Platform SDK (OpenXR-based)
 * to Pico Neo 2's native PVR runtime.
 *
 * This replaces libpxr_api.so in OpenXR-based Pico Neo 3 games so they
 * can run on the Neo 2 which lacks the OpenXR runtime but has the
 * legacy PVR runtime (libPvr_UnitySDK.so + com.pvr.vrdisplay service).
 */

#include <jni.h>
#include <dlfcn.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <unwind.h>
#include <link.h>
#include <sys/mman.h>
#include <elf.h>

#define TAG "pxr_api_shim"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* File-based logging to bypass chatty - use raw write() for reliability */
#include <fcntl.h>
#include <unistd.h>
static void flog(const char* msg) {
    int fd = open("/sdcard/Android/data/com.ivanovichgames.touringkartsPRO/files/pxr_shim.log",
                  O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) {
        fd = open("/sdcard/pxr_shim.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd < 0) return;
    }
    write(fd, msg, strlen(msg));
    write(fd, "\n", 1);
    close(fd);
}
static void flog_int(const char* prefix, int val) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s%d", prefix, val);
    if (n > 0) flog(buf);
}
#define FLOG(msg) flog(msg)
#define FLOGI(prefix, val) flog_int(prefix, val)

/* ---- PVR runtime function pointers ---- */

typedef int  (*Pvr_Init_t)(int);
typedef int  (*Pvr_StartSensor_t)(int);
typedef int  (*Pvr_StopSensor_t)(int);
typedef int  (*Pvr_ResetSensor_t)(int);
typedef void (*Pvr_SetInitActivity_t)(void*, void*);
typedef int  (*Pvr_GetIntConfig_t)(int, int*);
typedef int  (*Pvr_GetFloatConfig_t)(int, float*);
typedef int  (*Pvr_GetFOV_t)(float*, float*);
typedef int  (*Pvr_GetMainSensorState_t)(float*, float*, float*, float*,
                                         float*, float*, float*,
                                         float*, float*, int*);
typedef void (*Pvr_SetupLayerData_t)(int, int, int, int, int, float*); /* layerIndex, sideMask, textureId, textureType, layerFlags, colorScaleAndOffset */
typedef void (*Pvr_SetCurrentRenderTexture_t)(int); /* textureId */
typedef void (*Pvr_EnableSinglePass_t)(int); /* enable */
typedef void (*Pvr_CameraEndFrame_t)(int, int); /* eye, textureId */
typedef void (*Pvr_BothEyeEndFrameFU_t)(int, int, int); /* tex, depthTex, boundaryTex */
typedef void (*Pvr_TimeWarpEvent_t)(int); /* textureId/frameParam */
typedef void (*UnityRenderEvent_t)(int);
typedef int  (*Pvr_GetPsensorState_t)(void);
typedef int  (*Pvr_GetSensorState_t)(int, float*, float*, float*, float*,
                                     float*, float*, float*);
typedef void (*Pvr_SetCurrentHMDType_t)(const char*);
typedef void* (*Pvr_GetSupportHMDTypes_t)(void);
typedef void (*Pvr_ChangeScreenParameters_t)(const char*, int, int, double, double, double);

static struct {
    void* handle;
    void* ext2_handle;
    Pvr_Init_t Init;
    Pvr_StartSensor_t StartSensor;
    Pvr_StopSensor_t StopSensor;
    Pvr_ResetSensor_t ResetSensor;
    Pvr_SetInitActivity_t SetInitActivity;
    Pvr_GetIntConfig_t GetIntConfig;
    Pvr_GetFloatConfig_t GetFloatConfig;
    Pvr_GetFOV_t GetFOV;
    Pvr_GetMainSensorState_t GetMainSensorState;
    Pvr_SetupLayerData_t SetupLayerData;
    Pvr_SetCurrentRenderTexture_t SetCurrentRenderTexture;
    Pvr_EnableSinglePass_t EnableSinglePass;
    Pvr_CameraEndFrame_t CameraEndFrame;
    Pvr_BothEyeEndFrameFU_t BothEyeEndFrameFU;
    Pvr_TimeWarpEvent_t TimeWarpEvent;
    UnityRenderEvent_t RenderEvent;
    Pvr_GetPsensorState_t GetPsensorState;
    Pvr_GetSensorState_t GetSensorState;
    Pvr_SetCurrentHMDType_t SetCurrentHMDType;
    Pvr_GetSupportHMDTypes_t GetSupportHMDTypes;
    Pvr_ChangeScreenParameters_t ChangeScreenParameters;
} pvr;

/* ---- State ---- */

#define MAX_LAYERS 8
#define SWAPCHAIN_LEN 2

struct layer_info {
    int in_use;
    int width;
    int height;
    int format;
    GLuint textures[SWAPCHAIN_LEN];
    int current_index;
    int is_multiview;
};

static struct layer_info g_layers[MAX_LAYERS];

static JavaVM* g_jvm = NULL;
static int g_initialized = 0;
static int g_runtime_initialized = 0;
static int g_init_requested = 0;
static int g_xr_running = 0;
static int g_render_thread_inited = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- eglSwapBuffers hook via GOT patching ---- */
static EGLBoolean (*real_eglSwapBuffers)(EGLDisplay, EGLSurface) = NULL;
static unsigned char* g_swap_pixels = NULL;
static int g_swap_pix_w = 0, g_swap_pix_h = 0;
static int g_swap_hook_installed = 0;

/* FBO redirect state - declared early for hook functions */
static GLuint g_eye_fbo = 0;
static int g_fbo_hook_installed = 0;
static int g_redirect_fbo = 0; /* when 1, redirect FBO 0 to eye_fbo */

/* ---- eglGetProcAddress hook to intercept GL function lookups ---- */
static void* (*real_eglGetProcAddress)(const char*) = NULL;
static void (*real_glBindFramebuffer_ptr)(GLenum, GLuint) = NULL;

static void hook_glBindFramebuffer_ptr(GLenum target, GLuint fbo) {
    static int ptr_bind_log = 0;
    if (ptr_bind_log < 30) {
        LOGI("hook_glBindFramebuffer_ptr: target=0x%x fbo=%d redirect=%d eye_fbo=%d",
             target, fbo, g_redirect_fbo, g_eye_fbo);
        ptr_bind_log++;
    }
    if (g_redirect_fbo && fbo == 0 && g_eye_fbo) {
        fbo = g_eye_fbo;
    }
    if (real_glBindFramebuffer_ptr) {
        real_glBindFramebuffer_ptr(target, fbo);
    } else {
        glBindFramebuffer(target, fbo);
    }
}

static void* hook_eglGetProcAddress(const char* procname) {
    static int call_count = 0;
    if (call_count < 30) {
        LOGI("hook_eglGetProcAddress: %s", procname ? procname : "null");
        call_count++;
    }
    void* result = NULL;
    if (real_eglGetProcAddress) {
        result = real_eglGetProcAddress(procname);
    } else {
        result = (void*)eglGetProcAddress(procname);
    }
    if (procname && strcmp(procname, "glBindFramebuffer") == 0 && result) {
        real_glBindFramebuffer_ptr = (void(*)(GLenum, GLuint))result;
        LOGI("hook_eglGetProcAddress: intercepted glBindFramebuffer=%p -> %p",
             result, hook_glBindFramebuffer_ptr);
        return (void*)hook_glBindFramebuffer_ptr;
    }
    return result;
}

static void wrapped_RenderEvent(int event);
static UnityRenderEvent_t pvr_real_RenderEvent = NULL;

/* ---- glBindFramebuffer hook via inline patching of libGLESv2.so ---- */
static void (*real_glBindFramebuffer)(GLenum, GLuint) = NULL;
static GLuint g_eye_fbo_2 = 0; /* unused, g_eye_fbo declared above */

/* Original instructions of glBindFramebuffer stub (24 bytes max) */
static unsigned char g_orig_glBindFB[32];
static void* g_glBindFB_addr = NULL;
static int g_inline_hook_installed = 0;

/* Trampoline: executes original instructions then jumps back */
static unsigned char g_trampoline[64];

static void hook_glBindFramebuffer(GLenum target, GLuint fbo) {
    static int bind_log_count = 0;
    if (bind_log_count < 30) {
        LOGI("hook_glBindFramebuffer: target=0x%x fbo=%d redirect=%d eye_fbo=%d",
             target, fbo, g_redirect_fbo, g_eye_fbo);
        {
            char buf[128];
            snprintf(buf, sizeof(buf), "hook_glBindFramebuffer: target=0x%x fbo=%d redirect=%d eye_fbo=%d",
                     target, fbo, g_redirect_fbo, g_eye_fbo);
            FLOG(buf);
        }
        bind_log_count++;
    }
    if (g_redirect_fbo && fbo == 0 && target == GL_FRAMEBUFFER && g_eye_fbo) {
        real_glBindFramebuffer(target, g_eye_fbo);
    } else {
        real_glBindFramebuffer(target, fbo);
    }
}

/* Install inline hook on glBindFramebuffer in libGLESv2.so.
   The stub is:
     mrs x16, tpidr_el0       ; d53bd050
     ldr x16, [x16, #24]      ; f9400e10
     cbz x16, +12             ; b4000070
     ldr x16, [x16, #168]     ; f9405610
     br  x16                  ; d61f0200
     ret                      ; d65f03c0
   We overwrite first 16 bytes with:
     ldr x16, [pc, #8]        ; 58000050
     br  x16                  ; d61f0200
     .quad hook_addr
   The trampoline has the original 24 bytes + a branch back to addr+16. */
static void install_inline_hook() {
    void* handle = dlopen("libGLESv2.so", RTLD_NOW);
    if (!handle) {
        FLOG("install_inline_hook: dlopen libGLESv2.so failed");
        return;
    }
    void* sym = dlsym(handle, "glBindFramebuffer");
    dlclose(handle);
    if (!sym) {
        FLOG("install_inline_hook: glBindFramebuffer not found");
        return;
    }
    g_glBindFB_addr = sym;
    real_glBindFramebuffer = (void(*)(GLenum, GLuint))sym;

    /* Save original bytes */
    memcpy(g_orig_glBindFB, sym, 24);

    /* Build trampoline: copy original 24 bytes, then add branch back */
    memcpy(g_trampoline, g_orig_glBindFB, 24);
    /* After the original stub code, branch back to original+24 */
    /* But the stub is self-contained (it branches to the real impl),
       so we don't need to branch back. The trampoline IS the original stub. */
    /* Make trampoline executable */
    uintptr_t tpage = (uintptr_t)g_trampoline & ~0xFFF;
    mprotect((void*)tpage, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
    /* Flush instruction cache for trampoline */
    __builtin___clear_cache((char*)g_trampoline, (char*)g_trampoline + 64);

    /* Set real_glBindFramebuffer to the trampoline */
    real_glBindFramebuffer = (void(*)(GLenum, GLuint))g_trampoline;

    /* Overwrite original function with jump to our hook */
    uintptr_t page = (uintptr_t)sym & ~0xFFF;
    mprotect((void*)page, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);

    unsigned int* code = (unsigned int*)sym;
    /* ldr x16, [pc, #8]  ->  0x58000050
       br x16             ->  0xd61f0200 */
    code[0] = 0x58000050;  /* ldr x16, [pc, #8] */
    code[1] = 0xd61f0200;  /* br x16 */
    /* Write hook function address at offset +8 */
    void** addr_slot = (void**)((char*)sym + 8);
    addr_slot[0] = (void*)hook_glBindFramebuffer;

    /* Flush instruction cache */
    __builtin___clear_cache((char*)sym, (char*)sym + 16);

    /* Verify the overwrite succeeded */
    unsigned int* verify = (unsigned int*)sym;
    if (verify[0] == 0x58000050 && verify[1] == 0xd61f0200) {
        g_inline_hook_installed = 1;
        char buf[256];
        snprintf(buf, sizeof(buf), "install_inline_hook: hooked glBindFramebuffer at %p -> %p (verified)",
                 sym, (void*)hook_glBindFramebuffer);
        FLOG(buf);
    } else {
        char buf[256];
        snprintf(buf, sizeof(buf), "install_inline_hook: OVERWRITE FAILED! code=%08x %08x",
                 verify[0], verify[1]);
        FLOG(buf);
        /* Restore original */
        memcpy(sym, g_orig_glBindFB, 24);
        __builtin___clear_cache((char*)sym, (char*)sym + 24);
    }
}

/* Generic GOT patching state */
static const char* g_patch_sym_name = NULL;
static void** g_patch_real_fn = NULL;
static void* g_patch_hook_fn = NULL;
static int g_patch_found = 0;
static void* g_scan_target = NULL; /* pointer to scan for in writable memory */

/* Callback to scan a library's segments for a cached function pointer */
static int scan_ptr_callback(struct dl_phdr_info* info, size_t size, void* data) {
    const char* name = info->dlpi_name;
    if (!name || !*name) return 0;
    if (!strstr(name, "libunity.so") && !strstr(name, "libGLESv2") && !strstr(name, "libEGL")) return 0;
    if (!g_scan_target) return 0;

    for (int i = 0; i < info->dlpi_phnum; i++) {
        const Elf64_Phdr* phdr = &info->dlpi_phdr[i];
        if (phdr->p_type != PT_LOAD) continue;

        uintptr_t start = info->dlpi_addr + phdr->p_vaddr;
        size_t sz = phdr->p_memsz;
        void** ptr = (void**)start;
        int count = sz / sizeof(void*);

        for (int j = 0; j < count; j++) {
            if (ptr[j] == g_scan_target) {
                uintptr_t page = (uintptr_t)&ptr[j] & ~0xFFF;
                mprotect((void*)page, 0x1000, PROT_READ | PROT_WRITE);
                *g_patch_real_fn = ptr[j];
                ptr[j] = g_patch_hook_fn;
                LOGI("Patched cached %p -> %p at offset %d in %s segment %d (flags=0x%x)",
                     g_scan_target, g_patch_hook_fn, j, name, i, phdr->p_flags);
                g_patch_found = 1;
                /* Keep scanning for more occurrences */
            }
        }
    }
    return 0;
}

static int patch_got_callback(struct dl_phdr_info* info, size_t size, void* data) {
    const char* name = info->dlpi_name;
    if (!name || !*name) return 0;
    if (!strstr(name, ".so")) return 0;
    /* For glBindFramebuffer, try all libs (Unity may load it via eglGetProcAddress) */

    Elf64_Sym* symtab = NULL;
    const char* strtab = NULL;
    /* On ARM64, both DT_JMPREL (PLT) and DT_RELA use RELA entries.
       Scan both tables. */
    Elf64_Rela* jmprela = NULL;
    size_t jmprelasz = 0;
    Elf64_Rela* rela = NULL;
    size_t relasz = 0;
    size_t relaent = 0;

    for (int i = 0; i < info->dlpi_phnum; i++) {
        const Elf64_Phdr* phdr = &info->dlpi_phdr[i];
        if (phdr->p_type == PT_DYNAMIC) {
            Elf64_Dyn* dyn = (Elf64_Dyn*)(info->dlpi_addr + phdr->p_vaddr);
            for (Elf64_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
                switch (d->d_tag) {
                case DT_SYMTAB: symtab = (Elf64_Sym*)(info->dlpi_addr + d->d_un.d_ptr); break;
                case DT_STRTAB: strtab = (const char*)(info->dlpi_addr + d->d_un.d_ptr); break;
                case DT_JMPREL: jmprela = (Elf64_Rela*)(info->dlpi_addr + d->d_un.d_ptr); break;
                case DT_PLTRELSZ: jmprelasz = d->d_un.d_val; break;
                case DT_RELA: rela = (Elf64_Rela*)(info->dlpi_addr + d->d_un.d_ptr); break;
                case DT_RELASZ: relasz = d->d_un.d_val; break;
                case DT_RELAENT: relaent = d->d_un.d_val; break;
                }
            }
            break;
        }
    }
    if (!symtab || !strtab) return 0;

    size_t ent = relaent ? relaent : sizeof(Elf64_Rela);
    /* Scan PLT relocations first (where eglGetProcAddress, eglSwapBuffers live) */
    if (jmprela && jmprelasz > 0) {
        int count = jmprelasz / ent;
        for (int j = 0; j < count; j++) {
            Elf64_Rela* r = (Elf64_Rela*)((char*)jmprela + j * ent);
            int symidx = ELF64_R_SYM(r->r_info);
            Elf64_Sym* sym = &symtab[symidx];
            const char* symname = strtab + sym->st_name;
            if (symname && strcmp(symname, g_patch_sym_name) == 0) {
                void** got_entry = (void**)(info->dlpi_addr + r->r_offset);
                uintptr_t page = (uintptr_t)got_entry & ~0xFFF;
                mprotect((void*)page, 0x1000, PROT_READ | PROT_WRITE);
                if (*got_entry != g_patch_hook_fn) {
                    *g_patch_real_fn = *got_entry;
                    *got_entry = g_patch_hook_fn;
                    LOGI("Hooked %s in %s (PLT GOT %p -> %p)", g_patch_sym_name, name, got_entry, g_patch_hook_fn);
                    {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "patch_got: Hooked %s in %s GOT=%p",
                                 g_patch_sym_name, name, got_entry);
                        FLOG(buf);
                    }
                    g_patch_found = 1;
                }
                return 0;
            }
        }
    }
    /* Then scan non-PLT relocations */
    if (rela && relasz > 0) {
        int count = relasz / ent;
        for (int j = 0; j < count; j++) {
            Elf64_Rela* r = (Elf64_Rela*)((char*)rela + j * ent);
            int symidx = ELF64_R_SYM(r->r_info);
            Elf64_Sym* sym = &symtab[symidx];
            const char* symname = strtab + sym->st_name;
            if (symname && strcmp(symname, g_patch_sym_name) == 0) {
                void** got_entry = (void**)(info->dlpi_addr + r->r_offset);
                uintptr_t page = (uintptr_t)got_entry & ~0xFFF;
                mprotect((void*)page, 0x1000, PROT_READ | PROT_WRITE);
                if (*got_entry != g_patch_hook_fn) {
                    *g_patch_real_fn = *got_entry;
                    *got_entry = g_patch_hook_fn;
                    LOGI("Hooked %s in %s (RELA GOT %p)", g_patch_sym_name, name, got_entry);
                    {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "patch_got: Hooked(RELA) %s in %s GOT=%p",
                                 g_patch_sym_name, name, got_entry);
                        FLOG(buf);
                    }
                    g_patch_found = 1;
                }
                return 0;
            }
        }
    }
    return 0;
}

static int patch_got(const char* sym_name, void** real_fn, void* hook_fn) {
    g_patch_sym_name = sym_name;
    g_patch_real_fn = real_fn;
    g_patch_hook_fn = hook_fn;
    g_patch_found = 0;
    dl_iterate_phdr(patch_got_callback, NULL);
    return g_patch_found;
}

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    static int hook_call_count = 0;
    if (hook_call_count < 3) {
        LOGI("hook_eglSwapBuffers called! render_thread=%d", g_render_thread_inited);
        hook_call_count++;
    }
    if (g_render_thread_inited && real_eglSwapBuffers) {
        for (int i = 0; i < MAX_LAYERS; i++) {
            if (g_layers[i].in_use) {
                int w = g_layers[i].width;
                int h = g_layers[i].height;
                GLuint tex = g_layers[i].textures[0];
                if (!g_swap_pixels || g_swap_pix_w != w || g_swap_pix_h != h) {
                    free(g_swap_pixels);
                    g_swap_pixels = (unsigned char*)malloc(w * h * 4);
                    g_swap_pix_w = w;
                    g_swap_pix_h = h;
                }
                GLint prevFbo = 0;
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
                /* Read from the default framebuffer (window surface back buffer) */
                glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, g_swap_pixels);
                /* Log first pixel for debugging */
                static int swap_log_count = 0;
                if (swap_log_count < 5) {
                    LOGI("hook_eglSwapBuffers: prevFbo=%d pixel[0]=%d,%d,%d,%d size=%dx%d",
                         prevFbo, g_swap_pixels[0], g_swap_pixels[1],
                         g_swap_pixels[2], g_swap_pixels[3], w, h);
                    swap_log_count++;
                }
                glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
                glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, w, h, 1,
                                GL_RGBA, GL_UNSIGNED_BYTE, g_swap_pixels);
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, w, h, 1,
                                GL_RGBA, GL_UNSIGNED_BYTE, g_swap_pixels);
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
                break;
            }
        }
    }
    return real_eglSwapBuffers(dpy, surface);
}

static void install_swap_hook() {
    if (g_swap_hook_installed) return;
    FLOG("install_swap_hook: installing hooks");
    /* Hook eglGetProcAddress in libunity.so to intercept GL function lookups.
       Unity loads glBindFramebuffer via eglGetProcAddress, not through PLT,
       so GOT patching of glBindFramebuffer directly won't work for libunity.so. */
    if (!g_fbo_hook_installed) {
        int found = patch_got("eglGetProcAddress", (void**)&real_eglGetProcAddress, (void*)hook_eglGetProcAddress);
        FLOGI("install_swap_hook: eglGetProcAddress patch_got result=", found);
        if (found) {
            g_fbo_hook_installed = 1;
            LOGI("eglGetProcAddress hook installed");
            FLOG("install_swap_hook: eglGetProcAddress hook installed");
        } else {
            FLOG("install_swap_hook: eglGetProcAddress NOT found in PLT");
        }
    }
    /* Also hook glBindFramebuffer in libs that do use PLT (e.g. libPvr_UnitySDK.so) */
    if (!g_fbo_hook_installed) {
        int found = patch_got("glBindFramebuffer", (void**)&real_glBindFramebuffer, (void*)hook_glBindFramebuffer);
        FLOGI("install_swap_hook: glBindFramebuffer patch_got result=", found);
        if (found) {
            g_fbo_hook_installed = 1;
            LOGI("glBindFramebuffer hook installed");
            FLOG("install_swap_hook: glBindFramebuffer hook installed");
        }
    }
    /* Also try to find and patch Unity's cached glBindFramebuffer pointer.
       Unity loads GL funcs via eglGetProcAddress and caches them in .data/.bss.
       The eglGetProcAddress hook above only intercepts FUTURE calls, but Unity
       already cached the pointer. Scan libunity.so's writable segments for the
       real pointer and replace it with our wrapper. */
    {
        void* real_fn = (void*)eglGetProcAddress("glBindFramebuffer");
        FLOGI("install_swap_hook: real glBindFramebuffer=", (int)(uintptr_t)real_fn);
        if (real_fn) {
            g_patch_real_fn = (void**)&real_glBindFramebuffer_ptr;
            g_patch_hook_fn = (void*)hook_glBindFramebuffer_ptr;
            g_patch_sym_name = NULL;
            g_patch_found = 0;
            g_scan_target = real_fn;
            dl_iterate_phdr(scan_ptr_callback, NULL);
            if (g_patch_found) {
                LOGI("glBindFramebuffer pointer patched in libunity.so");
                FLOG("install_swap_hook: glBindFramebuffer pointer patched");
            } else {
                FLOG("install_swap_hook: glBindFramebuffer pointer NOT found in libunity.so");
            }
        }
    }
    /* Also hook eglSwapBuffers in libunity.so to capture Unity's output */
    {
        int found = patch_got("eglSwapBuffers", (void**)&real_eglSwapBuffers, (void*)hook_eglSwapBuffers);
        FLOGI("install_swap_hook: eglSwapBuffers patch_got result=", found);
        if (found) {
            LOGI("eglSwapBuffers hook installed");
            FLOG("install_swap_hook: eglSwapBuffers hook installed");
        }
    }
    /* Inline-hook glBindFramebuffer in libGLESv2.so. This is the most reliable
       way to intercept GL calls because Unity loads GL funcs via eglGetProcAddress
       and caches the pointers, bypassing GOT/PLT. Inline patching overwrites the
       actual function code so ALL callers go through our hook. */
    install_inline_hook();
    g_swap_hook_installed = 1;
}

/* ---- IL2CPP runtime API for calling C# methods ---- */
typedef struct Il2CppDomain Il2CppDomain;
typedef struct Il2CppAssembly Il2CppAssembly;
typedef struct Il2CppImage Il2CppImage;
typedef struct Il2CppClass Il2CppClass;
typedef struct MethodInfo MethodInfo;
typedef struct Il2CppException Il2CppException;
typedef struct Il2CppArray Il2CppArray;

static Il2CppDomain* (*p_il2cpp_domain_get)(void);
static const Il2CppAssembly* const* (*p_il2cpp_domain_get_assemblies)(Il2CppDomain*, size_t*);
static const Il2CppImage* (*p_il2cpp_assembly_get_image)(const Il2CppAssembly*);
static Il2CppClass* (*p_il2cpp_class_from_name)(const Il2CppImage*, const char*, const char*);
static const MethodInfo* (*p_il2cpp_class_get_method_from_name)(Il2CppClass*, const char*, int);
static void (*p_il2cpp_runtime_invoke)(const MethodInfo*, void*, void**, Il2CppException**);
static Il2CppClass* (*p_il2cpp_image_get_class)(const Il2CppImage*, size_t);
static size_t (*p_il2cpp_image_get_class_count)(const Il2CppImage*);
static void* (*p_il2cpp_thread_attach)(Il2CppDomain*);

static int g_il2cpp_loaded = 0;
static int g_xr_init_called = 0;

static void load_il2cpp_api() {
    if (g_il2cpp_loaded) return;
    void* h = dlopen("libil2cpp.so", RTLD_NOW);
    if (!h) {
        FLOG("load_il2cpp_api: dlopen libil2cpp.so failed");
        return;
    }
    p_il2cpp_domain_get = dlsym(h, "il2cpp_domain_get");
    p_il2cpp_domain_get_assemblies = dlsym(h, "il2cpp_domain_get_assemblies");
    p_il2cpp_assembly_get_image = dlsym(h, "il2cpp_assembly_get_image");
    p_il2cpp_class_from_name = dlsym(h, "il2cpp_class_from_name");
    p_il2cpp_class_get_method_from_name = dlsym(h, "il2cpp_class_get_method_from_name");
    p_il2cpp_runtime_invoke = dlsym(h, "il2cpp_runtime_invoke");
    p_il2cpp_image_get_class = dlsym(h, "il2cpp_image_get_class");
    p_il2cpp_image_get_class_count = dlsym(h, "il2cpp_image_get_class_count");
    p_il2cpp_thread_attach = dlsym(h, "il2cpp_thread_attach");
    if (p_il2cpp_domain_get && p_il2cpp_class_from_name && p_il2cpp_class_get_method_from_name && p_il2cpp_runtime_invoke) {
        g_il2cpp_loaded = 1;
        FLOG("load_il2cpp_api: IL2CPP runtime API loaded");
    } else {
        FLOG("load_il2cpp_api: missing functions");
    }
}

/* Call a static C# method with no parameters.
   Returns 1 on success, 0 on failure. */
static int call_csharp_static(const char* assembly_name, const char* ns, const char* class_name, const char* method_name) {
    if (!g_il2cpp_loaded) return 0;
    Il2CppDomain* domain = p_il2cpp_domain_get();
    if (!domain) {
        FLOG("call_csharp: domain is null");
        return 0;
    }
    size_t count = 0;
    const Il2CppAssembly* const* assemblies = p_il2cpp_domain_get_assemblies(domain, &count);
    if (!assemblies) {
        FLOG("call_csharp: assemblies is null");
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        const Il2CppImage* img = p_il2cpp_assembly_get_image(assemblies[i]);
        if (!img) continue;
        Il2CppClass* klass = p_il2cpp_class_from_name(img, ns, class_name);
        if (klass) {
            const MethodInfo* method = p_il2cpp_class_get_method_from_name(klass, method_name, 0);
            if (method) {
                Il2CppException* exc = NULL;
                p_il2cpp_runtime_invoke(method, NULL, NULL, &exc);
                if (exc) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "call_csharp: %s.%s.%s threw exception", ns, class_name, method_name);
                    FLOG(buf);
                } else {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "call_csharp: called %s.%s.%s (assembly %zu)", ns, class_name, method_name, i);
                    FLOG(buf);
                    return 1;
                }
            } else {
                char buf[256];
                snprintf(buf, sizeof(buf), "call_csharp: method %s not found in %s.%s (assembly %zu)", method_name, ns, class_name, i);
                FLOG(buf);
            }
        }
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "call_csharp: class %s.%s not found in any assembly", ns, class_name);
    FLOG(buf);
    return 0;
}

/* Call the XR initialization methods that are normally triggered by
   RuntimeInitializeOnLoadMethod(BeforeSplashScreen). In VR mode, the
   splash screen is skipped so these never fire. We call them manually. */
static void trigger_xr_init() {
    if (g_xr_init_called) return;
    g_xr_init_called = 1;
    load_il2cpp_api();
    if (!g_il2cpp_loaded) {
        FLOG("trigger_xr_init: IL2CPP API not loaded, skipping");
        return;
    }
    /* Attach current thread to IL2CPP runtime (we're on the render thread) */
    Il2CppDomain* domain = p_il2cpp_domain_get();
    if (!domain) {
        FLOG("trigger_xr_init: domain is null");
        return;
    }
    if (p_il2cpp_thread_attach) {
        p_il2cpp_thread_attach(domain);
        FLOG("trigger_xr_init: thread attached");
    }
    /* Call XRGeneralSettings.AttemptInitializeXRSDKOnLoad first.
       This initializes the XR loader. Without it, StartSDK does nothing. */
    call_csharp_static("Unity.XR.Management", "UnityEngine.XR.Management", "XRGeneralSettings", "AttemptInitializeXRSDKOnLoad");
    /* Call XRGeneralSettings.AttemptStartXRSDKOnBeforeSplashScreen
       This starts the XR SDK (display subsystem, input subsystem) */
    call_csharp_static("Unity.XR.Management", "UnityEngine.XR.Management", "XRGeneralSettings", "AttemptStartXRSDKOnBeforeSplashScreen");
    /* Call XRSystem.XRSystemInit
       This initializes URP's XR rendering system */
    call_csharp_static("Unity.RenderPipelines.Universal.Runtime", "UnityEngine.Rendering.Universal", "XRSystem", "XRSystemInit");
    FLOG("trigger_xr_init: done");
}

/* GL multiview extension function pointers */
typedef void (*PFNGLFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC)(GLenum, GLenum, GLuint, GLint, GLint, GLsizei);
typedef void (*PFNGLFRAMEBUFFERTEXTURESAMPLEMULTIVIEWOVRPROC)(GLenum, GLenum, GLuint, GLint, GLint, GLsizei);
static PFNGLFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC g_glFramebufferTextureMultiview = NULL;
static PFNGLFRAMEBUFFERTEXTURESAMPLEMULTIVIEWOVRPROC g_glFramebufferTextureSampleMultiview = NULL;

/* Layer/swapchain state (definitions moved to top of file) */

static int g_keep_submitting = 0;
static pthread_t g_submit_thread;

/* FOV cache */
static float g_fov_left = 50.0f;
static float g_fov_right = 50.0f;
static float g_fov_up = 50.0f;
static float g_fov_down = 50.0f;
static int g_fov_cached = 0;

/* Render event IDs matching PVR SDK RenderEventType enum */
#define RENDER_EVENT_INIT_RENDER_THREAD  1024
#define RENDER_EVENT_PAUSE               1025
#define RENDER_EVENT_RESUME              1026
#define RENDER_EVENT_LEFT_EYE_END_FRAME  1027
#define RENDER_EVENT_RIGHT_EYE_END_FRAME 1028
#define RENDER_EVENT_TIMEWARP            1029
#define RENDER_EVENT_RESET_VRMODE_PARMS  1030
#define RENDER_EVENT_SHUTDOWN            1031
#define RENDER_EVENT_BEGIN_EYE           1032
#define RENDER_EVENT_END_EYE             1033
#define RENDER_EVENT_BOUNDARY_LEFT       1034
#define RENDER_EVENT_BOUNDARY_RIGHT      1035
#define RENDER_EVENT_BOTH_EYE_END_FRAME  1036

/* ---- PVR config enum values (from Pvr_UnitySDKAPI.cs GlobalIntConfigs/GlobalFloatConfigs) ---- */
enum pvr_int_config {
    PVR_ICFG_EYE_TEX_RES0     = 0,  /* EYE_TEXTURE_RESOLUTION0 */
    PVR_ICFG_EYE_TEX_RES1     = 1,  /* EYE_TEXTURE_RESOLUTION1 */
    PVR_ICFG_SENSOR_COUNT     = 2,
    PVR_ICFG_6DOF             = 3,
    PVR_ICFG_PLATFORM_TYPE    = 4,
    PVR_ICFG_TRACKING_MODE    = 5,
    PVR_ICFG_TARGET_FPS       = 9,
    PVR_ICFG_SHOW_FPS         = 10,
    PVR_ICFG_AA_LEVEL         = 25,
};

enum pvr_float_config {
    PVR_FCFG_IPD              = 0,
    PVR_FCFG_VFOV             = 1,
    PVR_FCFG_HFOV             = 2,
    PVR_FCFG_NECK_X           = 3,
    PVR_FCFG_NECK_Y           = 4,
    PVR_FCFG_NECK_Z           = 5,
    PVR_FCFG_DISPLAY_RATE     = 6,
};

/* ---- PXR ConfigType enum (from PXR_Plugin.cs) ---- */
enum pxr_config {
    PXR_CFG_RENDER_TEX_W       = 0,
    PXR_CFG_RENDER_TEX_H       = 1,
    PXR_CFG_SHOW_FPS           = 2,
    PXR_CFG_TARGET_FRAME_RATE  = 7,
    PXR_CFG_PHYSICAL_IPD       = 10,
    PXR_CFG_SYSTEM_DISPLAY_RATE= 12,
    PXR_CFG_TRACKING_ORIGIN_H  = 13,
};

/* ---- Initialization ---- */

static int load_pvr_runtime() {
    if (pvr.handle) return 0;
    FLOG("load_pvr_runtime: starting");

    /* The APK bundles a PVR SDK compat library (libPvr_UnitySDK_compat.so)
       that provides Pvr_SetupLayerData, UnityRenderEvent, etc. This is a
       known-good PVR SDK library from a Neo 2 game. We use a distinct name
       to avoid conflict with the system libPvr_UnitySDK.so. */
    pvr.handle = dlopen("libPvr_UnitySDK_compat.so", RTLD_NOW | RTLD_LOCAL);
    if (!pvr.handle) {
        LOGE("failed to load libPvr_UnitySDK_compat.so: %s", dlerror());
        FLOG("load_pvr_runtime: FAILED to load compat lib");
        return -1;
    }
    FLOG("load_pvr_runtime: loaded compat lib");
    LOGI("loaded libPvr_UnitySDK_compat.so (PVR SDK compat)");

    /* Call pvr_OnLoad to initialize the compat library. This sets
       VrLibJavaVM, calls GetEnv/AttachCurrentThread, and caches class
       references (VrActivity, VrLib, etc.). We must NOT set VrLibJavaVM
       before calling pvr_OnLoad, because pvr_OnLoad checks if it's already
       set and returns false immediately, skipping class caching. */
    if (g_jvm) {
        /* Preload lib6DofReset.so from the system path so the compat
           library's pvr_OnLoad can find it. The compat library's dlopen
           with just the filename fails due to Android namespace rules. */
        void* dof = dlopen("/system/lib64/lib6DofReset.so", RTLD_NOW | RTLD_GLOBAL);
        if (!dof) dof = dlopen("lib6DofReset.so", RTLD_NOW | RTLD_GLOBAL);
        if (dof) {
            LOGI("preloaded lib6DofReset.so");
        } else {
            LOGW("failed to preload lib6DofReset.so: %s", dlerror());
        }

        typedef int (*pvr_OnLoad_t)(JavaVM*);
        pvr_OnLoad_t on_load = (pvr_OnLoad_t)dlsym(pvr.handle, "pvr_OnLoad");
        if (on_load) {
            int ret = on_load(g_jvm);
            LOGI("pvr_OnLoad returned %d", ret);
        } else {
            LOGW("pvr_OnLoad not found, setting VrLibJavaVM manually");
            JavaVM** vm_ptr = (JavaVM**)dlsym(pvr.handle, "VrLibJavaVM");
            if (vm_ptr) *vm_ptr = g_jvm;
        }

        /* Disable VR casting (MiraCast) - it crashes because it needs Java
           classes we don't have, and we don't need casting on Neo 2. */
        char* gEnableVRCasting = (char*)dlsym(pvr.handle, "gEnableVRCasting");
        if (gEnableVRCasting) {
            *gEnableVRCasting = 0;
            LOGI("disabled gEnableVRCasting");
        }
        int* iMiracastMode = (int*)dlsym(pvr.handle, "iMiracastMode");
        if (iMiracastMode) {
            *iMiracastMode = 0;
            LOGI("disabled iMiracastMode");
        }
    }

    #define LOAD(name, field) \
        pvr.field = (typeof(pvr.field))dlsym(pvr.handle, name); \
        if (!pvr.field) LOGW("symbol %s not found: %s", name, dlerror());

    LOAD("Pvr_Init", Init);
    LOAD("Pvr_StartSensor", StartSensor);
    LOAD("Pvr_StopSensor", StopSensor);
    LOAD("Pvr_ResetSensor", ResetSensor);
    LOAD("Pvr_SetInitActivity", SetInitActivity);
    LOAD("Pvr_GetIntConfig", GetIntConfig);
    LOAD("Pvr_GetFloatConfig", GetFloatConfig);
    LOAD("Pvr_GetFOV", GetFOV);
    LOAD("Pvr_GetMainSensorState", GetMainSensorState);
    LOAD("Pvr_SetupLayerData", SetupLayerData);
    LOAD("Pvr_SetCurrentRenderTexture", SetCurrentRenderTexture);
    LOAD("Pvr_EnableSinglePass", EnableSinglePass);
    LOAD("PVR_CameraEndFrame_", CameraEndFrame);
    LOAD("PVR_BothEyeEndFrameFU", BothEyeEndFrameFU);
    LOAD("PVR_TimeWarpEvent_", TimeWarpEvent);
    LOAD("UnityRenderEvent", RenderEvent);
    /* Wrap RenderEvent so we can blit from Unity's FBO to the eye
       texture before the PVR SDK processes the frame. */
    if (pvr.RenderEvent) {
        pvr_real_RenderEvent = pvr.RenderEvent;
        pvr.RenderEvent = wrapped_RenderEvent;
        LOGI("wrapped RenderEvent: real=%p wrapper=%p",
             pvr_real_RenderEvent, pvr.RenderEvent);
    }
    LOAD("Pvr_GetPsensorState", GetPsensorState);
    LOAD("Pvr_GetSensorState", GetSensorState);
    LOAD("Pvr_SetCurrentHMDType", SetCurrentHMDType);
    LOAD("Pvr_GetSupportHMDTypes", GetSupportHMDTypes);
    LOAD("Pvr_ChangeScreenParameters", ChangeScreenParameters);
    #undef LOAD

    /* The compat lib's interface table (PVR_CameraEndFrame_interface, etc.)
       is never populated because the PICO system's init code doesn't run.
       Load libPvr_UESDKExt2.so and wire up the interface pointers so the
       compat lib's thunk functions dispatch to the real implementations. */
    void* ext2 = dlopen("libPvr_UESDKExt2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!ext2) ext2 = dlopen("/system/lib64/libPvr_UESDKExt2.so", RTLD_NOW | RTLD_GLOBAL);
    if (ext2) {
        LOGI("loaded libPvr_UESDKExt2.so for interface wiring");
        FLOG("load_pvr_runtime: loaded ext2 for interface wiring");
        /* The ext2 lib needs VrLibJavaVM set via pvr_OnLoad, otherwise
           PVR_InitRenderThread_ aborts with "pvr_OnLoad() not called yet". */
        if (g_jvm) {
            typedef int (*pvr_OnLoad_t)(JavaVM*);
            pvr_OnLoad_t ext2_onload = (pvr_OnLoad_t)dlsym(ext2, "pvr_OnLoad");
            if (ext2_onload) {
                int ret = ext2_onload(g_jvm);
                LOGI("ext2 pvr_OnLoad returned %d", ret);
            } else {
                JavaVM** vm_ptr = (JavaVM**)dlsym(ext2, "VrLibJavaVM");
                if (vm_ptr) *vm_ptr = g_jvm;
                LOGI("set ext2 VrLibJavaVM manually");
            }
        }
        /* Both libs have a global "up" pointer to the main PVR struct.
           The compat lib's PVR_InitRenderThread_ allocates and sets it.
           The ext2 lib's functions (CameraEndFrame, TimeWarpEvent) read
           from ext2's own "up" which is still null. We need to sync them
           after the compat lib's init runs. This is done in Pxr_Initialize
           after calling pvr.Init(). For now, just save the ext2 handle. */
        pvr.ext2_handle = ext2;
        /* For each interface symbol in the compat lib, resolve the
           matching function in ext2 and store it.
           Only wire up functions that don't need Java objects - the
           render thread init and event handlers stay on the compat lib. */
        const char* iface_names[] = {
            "PVR_CameraEndFrame",
            "PVR_BoundaryRender",
            "PVR_CameraEndFrameItf",
            "PvrBeginEyeEvent", "PvrEndEyeEvent",
            "PVR_GetCpuLevel", "PVR_SetCpuLevel",
            "PVR_GetGpuLevel", "PVR_SetGpuLevel",
            "PVR_GetGpuUtilization",
            "PVR_GetHmdBatteryLevel", "PVR_GetHmdBatteryStatus",
            "PVR_GetHmdBatteryTemperature", "PVR_SetHmdAudioStatus",
            "PVR_SetUnrealParam",
            NULL
        };
        int wired = 0;
        for (int i = 0; iface_names[i]; i++) {
            char sym[128];
            snprintf(sym, sizeof(sym), "%s_interface", iface_names[i]);
            void** iface_ptr = (void**)dlsym(pvr.handle, sym);
            if (!iface_ptr) continue;
            void* func = dlsym(ext2, iface_names[i]);
            if (func) {
                *iface_ptr = func;
                wired++;
                if (wired <= 5)
                    LOGI("wired %s -> %p", sym, func);
            }
        }
        LOGI("wired %d PVR interfaces to libPvr_UESDKExt2.so", wired);
        FLOGI("load_pvr_runtime: wired ", wired);
    } else {
        LOGW("failed to load libPvr_UESDKExt2.so: %s", dlerror());
        FLOG("load_pvr_runtime: FAILED to load ext2");
    }

    return 0;
}

static jobject get_unity_activity(JNIEnv* env) {
    jclass cls = (*env)->FindClass(env, "com/unity3d/player/UnityPlayer");
    if (!cls) {
        LOGE("UnityPlayer class not found");
        return NULL;
    }
    jfieldID fid = (*env)->GetStaticFieldID(env, cls, "currentActivity", "Landroid/app/Activity;");
    if (!fid) {
        (*env)->DeleteLocalRef(env, cls);
        LOGE("currentActivity field not found");
        return NULL;
    }
    jobject activity = (*env)->GetStaticObjectField(env, cls, fid);
    (*env)->DeleteLocalRef(env, cls);
    return activity;
}

static void init_pvr_runtime(JNIEnv* env) {
    if (g_runtime_initialized) return;
    pthread_mutex_lock(&g_lock);
    if (g_runtime_initialized) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    if (load_pvr_runtime() < 0) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    /* Set the activity context for PVR runtime */
    jobject activity = get_unity_activity(env);
    if (activity && pvr.SetInitActivity) {
        jclass act_cls = (*env)->GetObjectClass(env, activity);
        pvr.SetInitActivity((*env)->NewGlobalRef(env, activity),
                           (*env)->NewGlobalRef(env, act_cls));
        LOGI("Pvr_SetInitActivity called");
        (*env)->DeleteLocalRef(env, act_cls);
    }
    if (activity) (*env)->DeleteLocalRef(env, activity);

    /* Set HMD type */
    if (pvr.SetCurrentHMDType) {
        pvr.SetCurrentHMDType("Pico Neo 2");
    }

    /* Initialize PVR */
    if (pvr.Init) {
        int ret = pvr.Init(0);
        LOGI("Pvr_Init(0) -> %d", ret);
    }

    /* Start sensor */
    if (pvr.StartSensor) {
        int ret = pvr.StartSensor(0);
        LOGI("Pvr_StartSensor(0) -> %d", ret);
    }

    /* Cache FOV from Pvr_GetMainSensorState which returns vfov and hfov */
    if (pvr.GetMainSensorState) {
        float x, y, z, w, px, py, pz, vfov, hfov;
        int viewNum;
        if (pvr.GetMainSensorState(&x, &y, &z, &w, &px, &py, &pz,
                                    &vfov, &hfov, &viewNum) == 0) {
            g_fov_up = g_fov_down = vfov * 0.5f;
            g_fov_left = g_fov_right = hfov * 0.5f;
            g_fov_cached = 1;
            LOGI("FOV: v=%f h=%f (L=%f R=%f U=%f D=%f)", vfov, hfov,
                 g_fov_left, g_fov_right, g_fov_up, g_fov_down);
        }
    } else if (pvr.GetFOV) {
        float vfov, hfov;
        pvr.GetFOV(&vfov, &hfov);
        g_fov_up = g_fov_down = vfov * 0.5f;
        g_fov_left = g_fov_right = hfov * 0.5f;
        g_fov_cached = 1;
        LOGI("FOV (GetFOV): v=%f h=%f (L=%f R=%f U=%f D=%f)", vfov, hfov,
             g_fov_left, g_fov_right, g_fov_up, g_fov_down);
    }

    /* Don't init render thread here - UnityRenderEvent(INIT_RENDER_THREAD)
       needs an EGL context and must be called from the render thread.
       We'll call it on the first Pxr_EndFrame instead. */

    g_initialized = 1;
    g_runtime_initialized = 1;
    pthread_mutex_unlock(&g_lock);
}

/* ---- JNI_OnLoad ---- */

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_jvm = vm;
    LOGI("JNI_OnLoad pxr_api_shim");
    FLOG("JNI_OnLoad");

    /* Load GL multiview extension functions */
    g_glFramebufferTextureMultiview = (PFNGLFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC)eglGetProcAddress("glFramebufferTextureMultiviewOVR");
    if (!g_glFramebufferTextureMultiview)
        g_glFramebufferTextureMultiview = (PFNGLFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC)eglGetProcAddress("glFramebufferTextureMultiview");
    g_glFramebufferTextureSampleMultiview = (PFNGLFRAMEBUFFERTEXTURESAMPLEMULTIVIEWOVRPROC)eglGetProcAddress("glFramebufferTextureSampleMultiviewOVR");
    if (!g_glFramebufferTextureSampleMultiview)
        g_glFramebufferTextureSampleMultiview = (PFNGLFRAMEBUFFERTEXTURESAMPLEMULTIVIEWOVRPROC)eglGetProcAddress("glFramebufferTextureSampleMultiview");
    LOGI("GL multiview: %p %p", g_glFramebufferTextureMultiview, g_glFramebufferTextureSampleMultiview);

    /* If Pxr_Initialize was called before JNI_OnLoad (it is, since
       libPxrPlatform.so loads us as a dependency), do the runtime init now.
       g_initialized may already be 1 (set by Pxr_Initialize to let the
       display subsystem proceed), but g_runtime_initialized is still 0
       because the PVR runtime needs JNI. */
    if (g_init_requested && !g_runtime_initialized) {
        FLOG("JNI_OnLoad: init_requested, calling init_pvr_runtime");
        JNIEnv* env = NULL;
        if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) == JNI_OK) {
            init_pvr_runtime(env);
            FLOG("JNI_OnLoad: init_pvr_runtime done");
        } else {
            FLOG("JNI_OnLoad: GetEnv failed");
        }
    }
    FLOG("JNI_OnLoad done");

    /* Install eglGetProcAddress hook EARLY, before Unity loads GL functions.
       Unity calls eglGetProcAddress to get GL function pointers. By hooking
       it now, we can intercept glBindFramebuffer and redirect FBO 0 to the
       eye texture. */
    install_swap_hook();
    FLOG("JNI_OnLoad: install_swap_hook called early");

    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {
    LOGI("JNI_OnUnload pxr_api_shim");
}

/* ---- Helper: get JNIEnv ---- */

static JNIEnv* get_env() {
    JavaVM* vm = g_jvm;
    if (!vm) {
        /* libpxr_api.so may be loaded as a dependency before JNI_OnLoad.
           Use JNI_GetCreatedJavaVMs to find the VM. */
        void* h = dlopen("libart.so", RTLD_NOW);
        if (h) {
            typedef jint (*GetVMs_t)(JavaVM**, jsize, jsize*);
            GetVMs_t fn = (GetVMs_t)dlsym(h, "JNI_GetCreatedJavaVMs");
            if (fn) {
                jsize count = 0;
                fn(NULL, 0, &count);
                if (count > 0) {
                    fn(&vm, 1, NULL);
                }
            }
            dlclose(h);
        }
    }
    if (!vm) return NULL;
    g_jvm = vm;

    JNIEnv* env = NULL;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) == JNI_OK)
        return env;
    (*vm)->AttachCurrentThread(vm, &env, NULL);
    return env;
}

/* ---- PXR API Implementation ---- */

/* PxrLayerParam struct layout (from PXR_Plugin.cs):
 * int layerId, int layerShape, int layerType, int layerLayout,
 * uint64 format, uint32 width, uint32 height, uint32 sampleCount,
 * uint32 faceCount, uint32 arraySize, uint32 mipmapCount,
 * uint32 layerFlags, uint32 externalImageCount,
 * void* leftExternalImages, void* rightExternalImages
 */
struct PxrLayerParam {
    int layerId;
    int layerShape;
    int layerType;
    int layerLayout;
    unsigned long long format;
    unsigned int width;
    unsigned int height;
    unsigned int sampleCount;
    unsigned int faceCount;
    unsigned int arraySize;
    unsigned int mipmapCount;
    unsigned int layerFlags;
    unsigned int externalImageCount;
    void* leftExternalImages;
    void* rightExternalImages;
};

struct PxrVec3 { float x, y, z; };
struct PxrVec4 { float x, y, z, w; };

struct PxrPosef {
    struct PxrVec4 orientation;
    struct PxrVec3 position;
};

struct PxrSensorState {
    int status;
    struct PxrPosef pose;
    struct PxrVec3 angVel, linVel, angAccel, linAccel;
    unsigned long long timestamp;
};

struct PxrSensorState2 {
    int status;
    struct PxrPosef pose;
    struct PxrPosef globalPose;
    struct PxrVec3 angVel, linVel, angAccel, linAccel;
    unsigned long long timestamp;
};

struct PxrLayerHeader {
    int layerId;
    unsigned int layerFlags;
    float colorScale[4];
    float colorBias[4];
    int compositionDepth;
    int sensorFrameIndex;
    int imageIndex;
    struct PxrPosef headPose;
};

JNIEXPORT int Pxr_Initialize() {
    LOGI("Pxr_Initialize");
    FLOG("Pxr_Initialize");
    g_init_requested = 1;

    JNIEnv* env = get_env();
    if (env) {
        init_pvr_runtime(env);
        FLOG("Pxr_Initialize: init_pvr_runtime done");
    } else {
        /* JNI_OnLoad hasn't been called yet. The init will be deferred
           to JNI_OnLoad. This is expected when libPxrPlatform.so loads
           us as a dependency before the Java runtime registers us.
           Mark as initialized anyway so the display subsystem proceeds
           with rendering. The PVR runtime will be fully initialized
           in JNI_OnLoad. */
        g_initialized = 1;
        FLOG("Pxr_Initialize: no JNIEnv yet, marked initialized, deferring runtime init to JNI_OnLoad");
        LOGW("Pxr_Initialize: no JNIEnv yet, deferring to JNI_OnLoad");
    }
    return 0;
}

JNIEXPORT int Pxr_Shutdown() {
    LOGI("Pxr_Shutdown");
    if (pvr.RenderEvent) pvr.RenderEvent(RENDER_EVENT_SHUTDOWN);
    if (pvr.StopSensor) pvr.StopSensor(0);
    g_initialized = 0;
    g_xr_running = 0;
    return 0;
}

JNIEXPORT int Pxr_IsInitialized() {
    LOGI("Pxr_IsInitialized -> %d", g_initialized);
    FLOG("Pxr_IsInitialized called");
    return g_initialized;
}

JNIEXPORT int Pxr_IsRunning() {
    LOGI("Pxr_IsRunning -> %d", g_xr_running);
    FLOG("Pxr_IsRunning called");
    return g_xr_running;
}

JNIEXPORT int Pxr_BeginXr() {
    LOGI("Pxr_BeginXr");
    FLOG("Pxr_BeginXr");
    g_xr_running = 1;
    return 0;
}

JNIEXPORT int Pxr_EndXr() {
    LOGI("Pxr_EndXr");
    FLOG("Pxr_EndXr");
    if (pvr.RenderEvent && g_render_thread_inited)
        pvr.RenderEvent(RENDER_EVENT_PAUSE);
    g_xr_running = 0;
    return 0;
}

JNIEXPORT int Pxr_SetInitializeData(void* data) {
    LOGI("Pxr_SetInitializeData(%p)", data);
    FLOG("Pxr_SetInitializeData");
    return 0;
}

JNIEXPORT int Pxr_SetPlatformOption(int type, int value) {
    LOGI("Pxr_SetPlatformOption(%d, %d)", type, value);
    FLOG("Pxr_SetPlatformOption");
    return 0;
}

JNIEXPORT int Pxr_SetGraphicOption(int option) {
    LOGI("Pxr_SetGraphicOption(%d)", option);
    FLOG("Pxr_SetGraphicOption");
    return 0;
}

JNIEXPORT int Pxr_SetTrackingMode(int mode) {
    LOGI("Pxr_SetTrackingMode(%d)", mode);
    FLOG("Pxr_SetTrackingMode");
    return 0;
}

JNIEXPORT int Pxr_GetTrackingMode(int* mode) {
    LOGI("Pxr_GetTrackingMode");
    if (mode) *mode = 0;
    return 0;
}

JNIEXPORT int Pxr_SetTrackingOrigin(int origin) {
    LOGI("Pxr_SetTrackingOrigin(%d)", origin);
    FLOG("Pxr_SetTrackingOrigin");
    return 0;
}

JNIEXPORT int Pxr_GetTrackingOrigin(int* origin) {
    LOGI("Pxr_GetTrackingOrigin");
    if (origin) *origin = 0;
    return 0;
}

JNIEXPORT int Pxr_ResetSensor(int option) {
    LOGI("Pxr_ResetSensor(%d)", option);
    if (pvr.ResetSensor) return pvr.ResetSensor(0);
    return 0;
}

JNIEXPORT void Pxr_ResetSensorHard() {
    LOGI("Pxr_ResetSensorHard");
    if (pvr.ResetSensor) pvr.ResetSensor(0);
}

/* ---- Backtrace helper ---- */
struct BacktraceState {
    void** current;
    void** end;
};

static _Unwind_Reason_Code unwind_cb(struct _Unwind_Context* ctx, void* arg) {
    struct BacktraceState* state = (struct BacktraceState*)arg;
    if (state->current == state->end) return _URC_END_OF_STACK;
    *state->current = (void*)_Unwind_GetIP(ctx);
    state->current++;
    return _URC_NO_REASON;
}

static void print_backtrace(const char* label) {
    void* buffer[20];
    struct BacktraceState state = { buffer, buffer + 20 };
    _Unwind_Backtrace(unwind_cb, &state);
    int n = state.current - buffer;
    LOGI("%s backtrace (%d frames):", label, n);
    for (int i = 0; i < n; i++) {
        LOGI("  #%d: %p", i, buffer[i]);
    }
}

/* ---- Layer / Swapchain ---- */

/* Background thread that continuously submits frames to PVR TimeWarp.
   Needed because the game's C# display subsystem never enters "running"
   state (StartSubsystems is never called), so Unity never calls the
   render loop callbacks. Without continuous frame submission, TimeWarp
   shows "No valid Eye Buffers" and the headset stays black. */
static void* submit_thread_fn(void* arg) {
    (void)arg;
    FLOG("submit_thread: started");
    int frame_count = 0;
    while (g_keep_submitting) {
        for (int i = 0; i < MAX_LAYERS; i++) {
            if (g_layers[i].in_use && pvr.SetupLayerData && pvr.RenderEvent) {
                float colorScaleOffset[8] = {1, 1, 1, 1, 0, 0, 0, 0};
                GLuint tex = g_layers[i].textures[0];
                int texType = g_layers[i].is_multiview ? 1 : 0;
                if (g_layers[i].is_multiview) {
                    pvr.SetupLayerData(0, 3, tex, 0x910A, 0, colorScaleOffset);
                } else {
                    pvr.SetupLayerData(0, 1, tex, 0x0DE1, 0, colorScaleOffset);
                    pvr.SetupLayerData(0, 2, g_layers[i].textures[1], 0x0DE1, 0, colorScaleOffset);
                }
                pvr.RenderEvent(RENDER_EVENT_BOTH_EYE_END_FRAME);
                if (frame_count == 0) FLOG("submit_thread: first frame submitted");
                frame_count++;
                break;
            }
        }
        usleep(13000); /* ~72fps */
    }
    FLOG("submit_thread: stopped");
    return NULL;
}

JNIEXPORT int Pxr_CreateLayer(void* layerParamPtr) {
    FLOG("Pxr_CreateLayer called");
    FLOG("Pxr_CreateLayer step 1: dump raw bytes");
    /* Dump raw bytes to figure out the struct layout */
    unsigned int* raw = (unsigned int*)layerParamPtr;
    if (raw) {
        LOGI("Pxr_CreateLayer raw: %u %u %u %u %u %u %u %u %u %u %u %u",
             raw[0], raw[1], raw[2], raw[3], raw[4], raw[5],
             raw[6], raw[7], raw[8], raw[9], raw[10], raw[11]);
        unsigned long long* raw64 = (unsigned long long*)layerParamPtr;
        LOGI("Pxr_CreateLayer raw64: %llu %llu %llu %llu %llu %llu",
             raw64[0], raw64[1], raw64[2], raw64[3], raw64[4], raw64[5]);
    }

    FLOG("Pxr_CreateLayer step 2: backtrace");
    /* Print backtrace to see who's calling */
    print_backtrace("Pxr_CreateLayer");

    FLOG("Pxr_CreateLayer step 3: parse params");
    struct PxrLayerParam* p = (struct PxrLayerParam*)layerParamPtr;
    int width = p ? (int)p->width : 1440;
    int height = p ? (int)p->height : 1584;

    LOGI("Pxr_CreateLayer w=%d h=%d format=%llu", width, height, p ? p->format : 0);
    FLOG("Pxr_CreateLayer parsed params");

    FLOG("Pxr_CreateLayer step 4: find slot");
    int slot = -1;
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (!g_layers[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        LOGE("no free layer slots");
        FLOG("Pxr_CreateLayer ERROR: no free slots");
        return -1;
    }

    FLOG("Pxr_CreateLayer step 5: setup layer");
    g_layers[slot].in_use = 1;
    g_layers[slot].width = width;
    g_layers[slot].height = height;
    g_layers[slot].format = p ? (int)p->format : GL_RGBA8;
    g_layers[slot].current_index = 0;

    int arraySize = p ? (int)p->arraySize : 1;
    int is_multiview = (arraySize >= 2);
    g_layers[slot].is_multiview = is_multiview;
    FLOGI("Pxr_CreateLayer: arraySize=", arraySize);
    FLOGI("Pxr_CreateLayer: is_multiview=", is_multiview);
    FLOGI("Pxr_CreateLayer: width=", width);
    FLOGI("Pxr_CreateLayer: height=", height);
    FLOG("Pxr_CreateLayer step 5b: arraySize checked");

    FLOG("Pxr_CreateLayer step 6: glGenTextures");
    /* Create swapchain textures. For multiview layers (arraySize>=2),
       use GL_TEXTURE_2D_ARRAY with 2 layers - Unity's PXR plugin uses
       glFramebufferTextureMultiviewOVR to attach them. */
    glGenTextures(SWAPCHAIN_LEN, g_layers[slot].textures);
    FLOG("Pxr_CreateLayer step 7: textures created");
    for (int i = 0; i < SWAPCHAIN_LEN; i++) {
        if (is_multiview) {
            glBindTexture(GL_TEXTURE_2D_ARRAY, g_layers[slot].textures[i]);
            glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, width, height, arraySize, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            glBindTexture(GL_TEXTURE_2D, g_layers[slot].textures[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }
    glBindTexture(is_multiview ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D, 0);

    FLOG("Pxr_CreateLayer step 8: textures configured");
    LOGI("Pxr_CreateLayer -> slot %d, textures [%u, %u]",
         slot, g_layers[slot].textures[0], g_layers[slot].textures[1]);

    if (p) {
        p->layerId = slot;
        /* Update arraySize to match what we actually created.
           Unity's PXR plugin reads this to decide whether to use
           glFramebufferTextureMultiviewOVR (arraySize>=2) or
           glFramebufferTexture2D (arraySize=1). */
        p->arraySize = arraySize;
    }

    FLOG("Pxr_CreateLayer step 9: auto-start XR");
    /* Auto-start XR if not already running - the game creates a layer
       when it's ready to render, so this is the right time to start XR */
    if (!g_xr_running) {
        LOGI("auto-starting XR (Pxr_BeginXr)");
        g_xr_running = 1;
    }

    FLOG("Pxr_CreateLayer step 10: init render thread");
    EGLContext ctx = eglGetCurrentContext();
    EGLDisplay dpy = eglGetCurrentDisplay();
    FLOGI("Pxr_CreateLayer: EGL ctx=", (int)(uintptr_t)ctx);
    FLOGI("Pxr_CreateLayer: EGL dpy=", (int)(uintptr_t)dpy);
    if (!g_render_thread_inited && pvr.RenderEvent) {
        LOGI("initializing PVR render thread from Pxr_CreateLayer");
        pvr.RenderEvent(RENDER_EVENT_INIT_RENDER_THREAD);
        g_render_thread_inited = 1;
        LOGI("PVR render thread init returned");
        install_swap_hook();
        /* Sync the "up" struct pointer from compat lib to ext2 lib.
           Both libs have a global "up" that points to the main PVR struct.
           The compat lib's InitRenderThread allocated and set it.
           The ext2 lib's functions need the same pointer. */
        if (pvr.ext2_handle) {
            /* Both libs have a global "up" struct (the main PVR state).
               They're in separate BSS sections. We need ext2's functions
               to access the same struct as the compat lib.
               Patch ext2's GOT entry for "up" to point to compat's "up". */
            void* compat_up = dlsym(pvr.handle, "up");
            void* ext2_up = dlsym(pvr.ext2_handle, "up");
            if (compat_up && ext2_up) {
                /* Calculate ext2's base address from the "up" symbol offset */
                const uintptr_t ext2_up_offset = 0x1ddde0;
                uintptr_t ext2_base = (uintptr_t)ext2_up - ext2_up_offset;
                /* The GOT entry for "up" is at offset 0x1c9178 in ext2 */
                void** got_entry = (void**)(ext2_base + 0x1c9178);
                /* Make the GOT page writable */
                uintptr_t page = (uintptr_t)got_entry & ~0xFFF;
                mprotect((void*)page, 0x1000, PROT_READ | PROT_WRITE);
                /* Overwrite the GOT entry to point to compat's "up" */
                *got_entry = compat_up;
                LOGI("patched ext2 GOT: up entry %p -> compat up %p", got_entry, compat_up);
                FLOG("Pxr_CreateLayer: patched ext2 GOT for up");
            }
        }
    }
    FLOG("Pxr_CreateLayer step 11: render thread done");

    /* Set up the display surface for TimeWarp. The PVR SDK needs an
       EGL window surface to present warped frames to the headset.
       Get the ANativeWindow from the Activity's ViewRootImpl.mSurface
       and call initialize() to create the EGL surface. */
    if (g_render_thread_inited) {
        /* Skip initialize() - it takes over Unity's window surface and
           causes DequeueBuffer errors. Test if Unity can render without it. */
        FLOG("Pxr_CreateLayer: skipping initialize() to preserve Unity surface");
    }

    if (pvr.SetupLayerData && pvr.RenderEvent) {
        float colorScaleOffset[8] = {1, 1, 1, 1, 0, 0, 0, 0};
        GLuint tex = g_layers[slot].textures[0];
        int texType = is_multiview ? 0x910A : 0x0DE1;
        LOGI("setting up PVR layer data with tex %u (type 0x%x, mv=%d)", tex, texType, is_multiview);

        /* Save GL state before FBO clear */
        GLint savedFbo = 0, savedViewport[4] = {0};
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &savedFbo);
        glGetIntegerv(GL_VIEWPORT, savedViewport);

        /* Clear textures to a solid color via FBO so TimeWarp has valid
           eye buffers. */
        GLuint fbo;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        if (is_multiview && g_glFramebufferTextureMultiview) {
            g_glFramebufferTextureMultiview(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                tex, 0, 0, arraySize);
        } else {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                GL_TEXTURE_2D, tex, 0);
        }
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        FLOGI("Pxr_CreateLayer: FBO status=", (int)status);
        if (status == GL_FRAMEBUFFER_COMPLETE) {
            glViewport(0, 0, width, height);
            glClearColor(0.2f, 0.3f, 0.5f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            FLOG("Pxr_CreateLayer: cleared eye texture");
        } else {
            FLOG("Pxr_CreateLayer: FBO incomplete, skipping clear");
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);

        /* Restore GL state */
        glBindFramebuffer(GL_FRAMEBUFFER, savedFbo);
        glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);

        /* Set current render texture before SetupLayerData - PVR SDK
           requires this to know which texture to use for each eye.
           Pvr_SetCurrentRenderTexture takes just one arg (textureId). */
        if (pvr.SetCurrentRenderTexture) {
            pvr.SetCurrentRenderTexture(tex);
            FLOG("Pxr_CreateLayer: SetCurrentRenderTexture called");
        }

        /* For multiview, enable single-pass mode and set the single-pass
           texture ID directly. The PVR SDK uses g_iSinglePassTexID for
           multiview rendering. */
        if (is_multiview) {
            if (pvr.EnableSinglePass) {
                pvr.EnableSinglePass(1);
                FLOG("Pxr_CreateLayer: EnableSinglePass(1) called");
            }
            int* g_iSinglePassTexID = (int*)dlsym(pvr.handle, "g_iSinglePassTexID");
            if (g_iSinglePassTexID) {
                FLOGI("Pxr_CreateLayer: PVR SDK g_iSinglePassTexID before set=", *g_iSinglePassTexID);
                *g_iSinglePassTexID = tex;
                FLOGI("Pxr_CreateLayer: g_iSinglePassTexID=", (int)tex);
            }
        }

        /* Save GL state before SetupLayerData (it changes GL state) */
        GLint savedFbo2 = 0, savedViewport2[4] = {0};
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &savedFbo2);
        glGetIntegerv(GL_VIEWPORT, savedViewport2);

        if (is_multiview) {
            /* Multiview: single texture array, store for both eyes */
            pvr.SetupLayerData(0, 1, tex, texType, 0, colorScaleOffset);
            pvr.SetupLayerData(0, 2, tex, texType, 0, colorScaleOffset);
        } else {
            /* Non-multiview: separate textures for each eye */
            pvr.SetupLayerData(0, 1, tex, texType, 0, colorScaleOffset);
            pvr.SetupLayerData(0, 2, g_layers[slot].textures[1], texType, 0, colorScaleOffset);
        }
        /* Set eye textures directly via PVR_CameraEndFrame_ */
        if (pvr.CameraEndFrame) {
            pvr.CameraEndFrame(0, tex);
            pvr.CameraEndFrame(1, is_multiview ? tex : g_layers[slot].textures[1]);
            FLOG("Pxr_CreateLayer: CameraEndFrame called");
        }

        /* Restore GL state after SetupLayerData/CameraEndFrame */
        glBindFramebuffer(GL_FRAMEBUFFER, savedFbo2);
        glViewport(savedViewport2[0], savedViewport2[1], savedViewport2[2], savedViewport2[3]);
        /* Set texture IDs in the up struct (from Ghidra decompilation). */
        {
            char* s = (char*)dlsym(pvr.handle, "up");
            FLOGI("Pxr_CreateLayer: up=", (int)(uintptr_t)s);
            if (s) {
                *(unsigned char*)(s + 0x68) = 1;
                *(int*)(s + 0xf960) = tex;
                *(int*)(s + 0xf964) = 0;
                *(int*)(s + 0xf928) = 0;
                *(int*)(s + 0xf92c) = 0;
                *(int*)(s + 0xf918) = tex;
                *(int*)(s + 0xf91c) = 0;
                *(int*)(s + 0xf920) = tex;
                *(int*)(s + 0xf924) = 0;
                *(unsigned char*)(s + 0xd810) = 1;
                *(int*)(s + 0xdabc) = 0;
                *(int*)(s + 0xda98) = 0;
                LOGI("Pxr_CreateLayer: set up+0xf960=%d up+0x68=%d",
                     *(int*)(s + 0xf960), *(unsigned char*)(s + 0x68));
                FLOG("Pxr_CreateLayer: set struct flags+tex");
            }
        }
        /* Submit frame via UnityRenderEvent_ (bypasses message queue).
           Call BOTH_EYE_END_FRAME to set eye textures, then TIMEWARP. */
        if (pvr.RenderEvent) {
            pvr.RenderEvent(RENDER_EVENT_BOTH_EYE_END_FRAME);
            pvr.RenderEvent(RENDER_EVENT_TIMEWARP);
            FLOG("Pxr_CreateLayer: RenderEvent BOTH_EYE+TIMEWARP called");
        }
        LOGI("submitted first frame to PVR TimeWarp");
        LOGI("submitted first frame to PVR TimeWarp");
    }
    FLOG("Pxr_CreateLayer step 12: layer data submitted");

    /* Frame submission is handled by Pxr_PollEvent (called every frame
       from the same thread as Pxr_CreateLayer, ensuring GL context
       compatibility). No background thread needed. */

    /* Check for pending GL errors before returning */
    {
        GLenum err = glGetError();
        while (err != GL_NO_ERROR) {
            LOGE("Pxr_CreateLayer: pending GL error 0x%x before return", err);
            FLOGI("Pxr_CreateLayer: pending GL error=", (int)err);
            err = glGetError();
        }
    }
    LOGI("Pxr_CreateLayer returning slot %d", slot);
    FLOG("Pxr_CreateLayer returning");
    return slot;
}

JNIEXPORT int Pxr_DestroyLayer(int layerId) {
    LOGI("Pxr_DestroyLayer(%d)", layerId);
    FLOG("Pxr_DestroyLayer");
    if (layerId < 0 || layerId >= MAX_LAYERS || !g_layers[layerId].in_use)
        return -1;
    g_layers[layerId].in_use = 0;
    glDeleteTextures(SWAPCHAIN_LEN, g_layers[layerId].textures);
    memset(&g_layers[layerId], 0, sizeof(g_layers[layerId]));
    /* Stop submit thread if no layers remain */
    int any = 0;
    for (int i = 0; i < MAX_LAYERS; i++)
        if (g_layers[i].in_use) { any = 1; break; }
    if (!any && g_keep_submitting) {
        g_keep_submitting = 0;
        pthread_join(g_submit_thread, NULL);
    }
    return 0;
}

JNIEXPORT int Pxr_GetLayerImageCount(int layerId, int eye, unsigned int* count) {
    LOGI("Pxr_GetLayerImageCount(%d, %d)", layerId, eye);
    FLOG("Pxr_GetLayerImageCount");
    if (count) *count = SWAPCHAIN_LEN;
    return 0;
}

JNIEXPORT int Pxr_GetLayerNextImageIndex(int layerId, int* imageIndex) {
    if (layerId < 0 || layerId >= MAX_LAYERS || !g_layers[layerId].in_use)
        return -1;
    int idx = g_layers[layerId].current_index;
    g_layers[layerId].current_index = (idx + 1) % SWAPCHAIN_LEN;
    if (imageIndex) *imageIndex = idx;
    LOGI("Pxr_GetLayerNextImageIndex(%d) -> %d", layerId, idx);
    FLOG("Pxr_GetLayerNextImageIndex");
    return 0;
}

JNIEXPORT int Pxr_GetLayerImage(int layerId, int eye, int imageIndex, unsigned long long* image) {
    LOGI("Pxr_GetLayerImage(%d, eye=%d, idx=%d)", layerId, eye, imageIndex);
    FLOGI("Pxr_GetLayerImage eye=", eye);
    FLOGI("Pxr_GetLayerImage idx=", imageIndex);
    if (layerId < 0 || layerId >= MAX_LAYERS || !g_layers[layerId].in_use)
        return -1;
    if (imageIndex < 0 || imageIndex >= SWAPCHAIN_LEN)
        return -1;
    if (image) *image = (unsigned long long)g_layers[layerId].textures[imageIndex];
    LOGI("Pxr_GetLayerImage(%d, %d, %d) -> %llu", layerId, eye, imageIndex, image ? *image : 0);
    FLOGI("Pxr_GetLayerImage -> tex=", (int)g_layers[layerId].textures[imageIndex]);
    return 0;
}

JNIEXPORT int Pxr_GetLayerFoveationImage(int layerId, int eye, int imageIndex, unsigned long long* image) {
    return Pxr_GetLayerImage(layerId, eye, imageIndex, image);
}

JNIEXPORT void Pxr_SetCreateLayerParam(void* param) {
    LOGI("Pxr_SetCreateLayerParam");
    FLOG("Pxr_SetCreateLayerParam");
    /* called by PxrPlatform before Pxr_CreateLayer, we handle it in CreateLayer */
}

JNIEXPORT void Pxr_DestroyLayerByRender(int layerId) {
    LOGI("Pxr_DestroyLayerByRender(%d)", layerId);
    FLOG("Pxr_DestroyLayer");
    Pxr_DestroyLayer(layerId);
}

/* ---- Frame ---- */

JNIEXPORT int Pxr_WaitFrame() {
    FLOG("Pxr_WaitFrame");
    LOGI("Pxr_WaitFrame");
    FLOG("Pxr_WaitFrame");
    return 0;
}

JNIEXPORT int Pxr_BeginFrame() {
    FLOG("Pxr_BeginFrame");
    LOGI("Pxr_BeginFrame");
    FLOG("Pxr_BeginFrame");
    return 0;
}

JNIEXPORT int Pxr_EndFrame() {
    FLOG("Pxr_EndFrame");
    /* Init render thread on first EndFrame (needs EGL context) */
    if (!g_render_thread_inited && pvr.RenderEvent) {
        LOGI("initializing PVR render thread");
        FLOG("initializing PVR render thread");
        pvr.RenderEvent(RENDER_EVENT_INIT_RENDER_THREAD);
        g_render_thread_inited = 1;
    }

    /* Trigger TimeWarp via PVR render event */
    if (pvr.RenderEvent) {
        pvr.RenderEvent(RENDER_EVENT_BOTH_EYE_END_FRAME);
    }
    return 0;
}

JNIEXPORT int Pxr_CanBeginVR() {
    LOGI("Pxr_CanBeginVR -> 1");
    FLOG("Pxr_CanBeginVR");
    return 1;
}

/* ---- Layer submission ---- */

JNIEXPORT int Pxr_SubmitLayer(void* layerPtr) {
    struct PxrLayerHeader* h = (struct PxrLayerHeader*)layerPtr;
    if (!h || h->layerId < 0 || h->layerId >= MAX_LAYERS || !g_layers[h->layerId].in_use)
        return -1;

    int texId = (int)g_layers[h->layerId].textures[h->imageIndex % SWAPCHAIN_LEN];
    LOGI("Pxr_SubmitLayer layer=%d imageIdx=%d tex=%u", h->layerId, h->imageIndex, texId);
    FLOG("Pxr_SubmitLayer");

    /* Submit to PVR TimeWarp: layer 0, both eyes, texture, type=0, flags=0 */
    if (pvr.SetupLayerData) {
        float colorScaleOffset[8] = {1,1,1,1, 0,0,0,0};
        pvr.SetupLayerData(0, 3, texId, 0, 0, colorScaleOffset);
        pvr.SetupLayerData(0, 1, texId, 0, 0, colorScaleOffset);
    }
    return 0;
}

JNIEXPORT int Pxr_SubmitLayer2(void* layerPtr) {
    LOGI("Pxr_SubmitLayer2");
    FLOG("Pxr_SubmitLayer");
    return Pxr_SubmitLayer(layerPtr);
}

JNIEXPORT int Pxr_SubmitLayerProjection(void* layer) {
    LOGI("Pxr_SubmitLayerProjection");
    FLOG("Pxr_SubmitLayer");
    return Pxr_SubmitLayer(layer);
}
JNIEXPORT int Pxr_SubmitLayerProjection2(void* layer) {
    LOGI("Pxr_SubmitLayerProjection2");
    FLOG("Pxr_SubmitLayer");
    return Pxr_SubmitLayer(layer);
}
JNIEXPORT int Pxr_SubmitLayerQuad(void* layer) { LOGI("Pxr_SubmitLayerQuad"); FLOG("Pxr_SubmitLayerQuad"); return 0; }
JNIEXPORT int Pxr_SubmitLayerQuad2(void* layer) { LOGI("Pxr_SubmitLayerQuad2"); FLOG("Pxr_SubmitLayerQuad2"); return 0; }
JNIEXPORT int Pxr_SubmitLayerCylinder(void* layer) { LOGI("Pxr_SubmitLayerCylinder"); FLOG("Pxr_SubmitLayerCylinder"); return 0; }
JNIEXPORT int Pxr_SubmitLayerCylinder2(void* layer) { LOGI("Pxr_SubmitLayerCylinder2"); FLOG("Pxr_SubmitLayerCylinder2"); return 0; }
JNIEXPORT int Pxr_SubmitLayerEquirect(void* layer) { LOGI("Pxr_SubmitLayerEquirect"); FLOG("Pxr_SubmitLayerEquirect"); return 0; }
JNIEXPORT int Pxr_SubmitLayerEquirect2(void* layer) { LOGI("Pxr_SubmitLayerEquirect2"); FLOG("Pxr_SubmitLayerEquirect2"); return 0; }
JNIEXPORT int Pxr_SubmitLayerCube2(void* layer) { LOGI("Pxr_SubmitLayerCube2"); FLOG("Pxr_SubmitLayerCube2"); return 0; }

/* ---- Sensor / Pose ---- */

JNIEXPORT int Pxr_GetPredictedMainSensorState2(double predictTimeMs,
        struct PxrSensorState2* sensorState, int* sensorFrameIndex) {
    LOGI("Pxr_GetPredictedMainSensorState2(%f)", predictTimeMs);
    FLOG("Pxr_GetPredictedMainSensorState2");
    if (!sensorState) return -1;
    memset(sensorState, 0, sizeof(*sensorState));

    if (pvr.GetMainSensorState) {
        float x, y, z, w, px, py, pz, vfov, hfov;
        int viewNum;
        int ret = pvr.GetMainSensorState(&x, &y, &z, &w, &px, &py, &pz,
                                          &vfov, &hfov, &viewNum);
        if (ret == 0) {
            sensorState->status = 1;
            sensorState->pose.orientation.x = x;
            sensorState->pose.orientation.y = y;
            sensorState->pose.orientation.z = z;
            sensorState->pose.orientation.w = w;
            sensorState->pose.position.x = px;
            sensorState->pose.position.y = py;
            sensorState->pose.position.z = pz;
            /* copy to global pose too */
            sensorState->globalPose = sensorState->pose;
            if (sensorFrameIndex) *sensorFrameIndex = 0;
            return 0;
        }
    }

    /* fallback: try GetSensorState */
    if (pvr.GetSensorState) {
        float x, y, z, w, px, py, pz;
        if (pvr.GetSensorState(0, &x, &y, &z, &w, &px, &py, &pz) == 0) {
            sensorState->status = 1;
            sensorState->pose.orientation.x = x;
            sensorState->pose.orientation.y = y;
            sensorState->pose.orientation.z = z;
            sensorState->pose.orientation.w = w;
            sensorState->pose.position.x = px;
            sensorState->pose.position.y = py;
            sensorState->pose.position.z = pz;
            sensorState->globalPose = sensorState->pose;
            if (sensorFrameIndex) *sensorFrameIndex = 0;
            return 0;
        }
    }
    return -1;
}

JNIEXPORT int Pxr_GetPredictedMainSensorState(double predictTimeMs,
        struct PxrSensorState* sensorState) {
    LOGI("Pxr_GetPredictedMainSensorState(%f)", predictTimeMs);
    FLOG("Pxr_GetPredictedMainSensorState");
    if (!sensorState) return -1;
    memset(sensorState, 0, sizeof(*sensorState));

    if (pvr.GetMainSensorState) {
        float x, y, z, w, px, py, pz, vfov, hfov;
        int viewNum;
        if (pvr.GetMainSensorState(&x, &y, &z, &w, &px, &py, &pz,
                                    &vfov, &hfov, &viewNum) == 0) {
            sensorState->status = 1;
            sensorState->pose.orientation.x = x;
            sensorState->pose.orientation.y = y;
            sensorState->pose.orientation.z = z;
            sensorState->pose.orientation.w = w;
            sensorState->pose.position.x = px;
            sensorState->pose.position.y = py;
            sensorState->pose.position.z = pz;
            return 0;
        }
    }
    return -1;
}

JNIEXPORT int Pxr_GetPredictedMainSensorStateWithEyePose(double t, void* s) {
    LOGI("Pxr_GetPredictedMainSensorStateWithEyePose");
    FLOG("Pxr_GetPredictedMainSensorState");
    return Pxr_GetPredictedMainSensorState(t, (struct PxrSensorState*)s);
}

JNIEXPORT int Pxr_GetHeadSensorData(double t, void* s) {
    LOGI("Pxr_GetHeadSensorData");
    FLOG("Pxr_GetHeadSensorData");
    return Pxr_GetPredictedMainSensorState(t, (struct PxrSensorState*)s);
}

JNIEXPORT int Pxr_GetPredictedDisplayTime(double* t) {
    LOGI("Pxr_GetPredictedDisplayTime");
    FLOG("Pxr_GetPredictedDisplayTime");
    if (t) *t = 0.0;
    return 0;
}

/* ---- FOV / Frustum ---- */

JNIEXPORT int Pxr_GetFov(int eye, float* fovLeft, float* fovRight,
                         float* fovUp, float* fovDown) {
    LOGI("Pxr_GetFov(%d)", eye);
    FLOG("Pxr_GetFov");
    if (!g_fov_cached) {
        if (pvr.GetMainSensorState) {
            float x, y, z, w, px, py, pz, vfov, hfov;
            int viewNum;
            if (pvr.GetMainSensorState(&x, &y, &z, &w, &px, &py, &pz,
                                        &vfov, &hfov, &viewNum) == 0) {
                g_fov_up = g_fov_down = vfov * 0.5f;
                g_fov_left = g_fov_right = hfov * 0.5f;
                g_fov_cached = 1;
            }
        } else if (pvr.GetFOV) {
            float vfov, hfov;
            pvr.GetFOV(&vfov, &hfov);
            g_fov_up = g_fov_down = vfov * 0.5f;
            g_fov_left = g_fov_right = hfov * 0.5f;
            g_fov_cached = 1;
        }
    }
    if (fovLeft) *fovLeft = g_fov_left;
    if (fovRight) *fovRight = g_fov_right;
    if (fovUp) *fovUp = g_fov_up;
    if (fovDown) *fovDown = g_fov_down;
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Pxr_GetFov -> L=%f R=%f U=%f D=%f",
            g_fov_left, g_fov_right, g_fov_up, g_fov_down);
        FLOG(buf);
    }
    return 0;
}

JNIEXPORT int Pxr_SetFrustum(int eye, float fl, float fr, float fu,
                             float fd, float n, float f) {
    LOGI("Pxr_SetFrustum(%d)", eye);
    return 0;
}

JNIEXPORT int Pxr_GetFrustum(int eye, float* fl, float* fr, float* fu,
                             float* fd, float* n, float* f) {
    LOGI("Pxr_GetFrustum(%d)", eye);
    FLOG("Pxr_GetFrustum");
    Pxr_GetFov(eye, fl, fr, fu, fd);
    if (n) *n = 0.1f;
    if (f) *f = 100.0f;
    return 0;
}

/* ---- Config ---- */

JNIEXPORT int Pxr_GetConfigInt(int configIndex, int* value) {
    if (!value) return -1;
    FLOGI("Pxr_GetConfigInt idx=", configIndex);
    int ret;
    switch (configIndex) {
    case PXR_CFG_RENDER_TEX_W:
        if (pvr.GetIntConfig) {
            ret = pvr.GetIntConfig(PVR_ICFG_EYE_TEX_RES0, value);
            LOGI("Pxr_GetConfigInt(%d) [RENDER_TEX_W] -> %d (val=%d)", configIndex, ret, *value);
            FLOG("Pxr_GetConfigInt");
            if (ret != 0 || *value <= 0) { *value = 1440; ret = 0; }
            return ret;
        }
        *value = 1440;
        LOGI("Pxr_GetConfigInt(%d) [RENDER_TEX_W] -> fallback 1440", configIndex);
        FLOG("Pxr_GetConfigInt");
        return 0;
    case PXR_CFG_RENDER_TEX_H:
        if (pvr.GetIntConfig) {
            ret = pvr.GetIntConfig(PVR_ICFG_EYE_TEX_RES1, value);
            LOGI("Pxr_GetConfigInt(%d) [RENDER_TEX_H] -> %d (val=%d)", configIndex, ret, *value);
            FLOG("Pxr_GetConfigInt");
            if (ret != 0 || *value <= 0) { *value = 1584; ret = 0; }
            return ret;
        }
        *value = 1584;
        LOGI("Pxr_GetConfigInt(%d) [RENDER_TEX_H] -> fallback 1584", configIndex);
        FLOG("Pxr_GetConfigInt");
        return 0;
    case PXR_CFG_TARGET_FRAME_RATE:
        *value = 72;
        LOGI("Pxr_GetConfigInt(%d) [TARGET_FRAME_RATE] -> 72", configIndex);
        FLOG("Pxr_GetConfigInt");
        return 0;
    case PXR_CFG_SYSTEM_DISPLAY_RATE:
        *value = 72;
        LOGI("Pxr_GetConfigInt(%d) [SYSTEM_DISPLAY_RATE] -> 72", configIndex);
        FLOG("Pxr_GetConfigInt");
        return 0;
    case 28:
        /* Entitlement check - return 1 to indicate passed */
        *value = 1;
        LOGI("Pxr_GetConfigInt(%d) [ENTITLEMENT] -> 1", configIndex);
        FLOG("Pxr_GetConfigInt(28)->1");
        return 0;
    case 5:
        /* Unknown config - return 1 */
        *value = 1;
        LOGI("Pxr_GetConfigInt(%d) -> 1", configIndex);
        FLOG("Pxr_GetConfigInt(5)->1");
        return 0;
    case 8:
        /* UnityLogLevel - return 2 (Debug) to enable all PXR logs */
        *value = 2;
        LOGI("Pxr_GetConfigInt(%d) [UnityLogLevel] -> 2", configIndex);
        FLOG("Pxr_GetConfigInt(8)->2");
        return 0;
    case 13:
        /* Unknown config - might be a feature flag for display subsystem.
           Return 1 to enable. */
        *value = 1;
        LOGI("Pxr_GetConfigInt(%d) [13] -> 1", configIndex);
        FLOG("Pxr_GetConfigInt(13)->1");
        return 0;
    case 25:
        /* Unknown config - might be a feature flag for display subsystem.
           Return 1 to enable. */
        *value = 1;
        LOGI("Pxr_GetConfigInt(%d) [25] -> 1", configIndex);
        FLOG("Pxr_GetConfigInt(25)->1");
        return 0;
    default:
        *value = 0;
        LOGI("Pxr_GetConfigInt(%d) -> default 0", configIndex);
        FLOG("Pxr_GetConfigInt");
        return 0;
    }
}

JNIEXPORT int Pxr_GetConfigFloat(int configIndex, float* value) {
    if (!value) return -1;
    switch (configIndex) {
    case PXR_CFG_PHYSICAL_IPD:
        *value = 0.064f;
        return 0;
    case PXR_CFG_SYSTEM_DISPLAY_RATE:
        *value = 72.0f;
        return 0;
    default:
        *value = 0.0f;
        return 0;
    }
}

JNIEXPORT int Pxr_SetConfigInt(int configIndex, int value) {
    LOGI("Pxr_SetConfigInt(%d, %d)", configIndex, value);
    FLOG("Pxr_SetConfigInt");
    return 0;
}

JNIEXPORT int Pxr_SetConfigString(int configIndex, const char* value) {
    LOGI("Pxr_SetConfigString(%d, %s)", configIndex, value ? value : "null");
    return 0;
}

JNIEXPORT int Pxr_SetConfigUint64(int configIndex, unsigned long long value) {
    LOGI("Pxr_SetConfigUint64(%d, %llu)", configIndex, value);
    return 0;
}

JNIEXPORT int Pxr_SetConfigIntArray(int configIndex, int* data, int count) {
    LOGI("Pxr_SetConfigIntArray(%d, %d)", configIndex, count);
    FLOG("Pxr_SetConfigInt");
    return 0;
}

JNIEXPORT void Pxr_SetConfigFloatArray(int a, float* b, int c) {}
JNIEXPORT int Pxr_SetConfig(void* a) { return 0; }
JNIEXPORT int Pxr_SetConfigs(void* a) { return 0; }
JNIEXPORT int Pxr_GetConfig(void* a) { return 0; }
JNIEXPORT int Pxr_GetConfigs(void* a) { return 0; }
JNIEXPORT int Pxr_GetConfigString(int a, char* b, int c) { return 0; }
JNIEXPORT int Pxr_GetConfigViewsInfos(void* a, void* b) { return 0; }

/* ---- Display refresh rate ---- */

JNIEXPORT int Pxr_SetDisplayRefreshRate(float rate) {
    LOGI("Pxr_SetDisplayRefreshRate(%f)", rate);
    FLOG("Pxr_SetDisplayRefreshRate");
    return 0;
}

JNIEXPORT int Pxr_GetDisplayRefreshRate(float* rate) {
    if (rate) *rate = 72.0f;
    return 0;
}

JNIEXPORT int Pxr_GetDisplayRefreshRatesAvailable(int* count, void** array) {
    if (count) *count = 1;
    return 0;
}

JNIEXPORT float Pxr_RefreshRateChanged() {
    return 0.0f;
}

JNIEXPORT int Pxr_SetExtraLatencyMode(int mode) { return 0; }

/* ---- Focus / App state ---- */

JNIEXPORT int Pxr_GetAppHasFocus() { LOGI("Pxr_GetAppHasFocus -> 1"); FLOG("Pxr_GetAppHasFocus"); return 1; }
JNIEXPORT int Pxr_GetTrackingState() { LOGI("Pxr_GetTrackingState -> 1"); FLOG("Pxr_GetTrackingState"); return 1; }
JNIEXPORT int Pxr_PollEvent(void* event) {
    static int poll_count = 0;
    /* Trigger XR initialization after a few frames to ensure IL2CPP
       assemblies are loaded. BeforeSplashScreen never fires in VR mode
       so AttemptStartXRSDKOnBeforeSplashScreen and XRSystemInit are
       never called by Unity. We call them manually. */
    if (poll_count == 3 && !g_xr_init_called) {
        trigger_xr_init();
    }
    if (g_render_thread_inited && pvr.RenderEvent) {
        /* Blit Unity's rendered output to the eye texture at the START
           of Pxr_PollEvent. Unity renders between Pxr_PollEvent calls,
           so the previous frame's output is now in Unity's FBO.
           We copy it to the eye texture before submitting to TimeWarp. */
        if (poll_count > 0) {
            int any_in_use = 0;
            for (int i = 0; i < MAX_LAYERS; i++) any_in_use += g_layers[i].in_use;
            static int reach_log = 0;
            if (reach_log < 3) {
                char buf[128];
                snprintf(buf, sizeof(buf), "EYECHECK reach: poll_count=%d any_in_use=%d", poll_count, any_in_use);
                FLOG(buf);
                reach_log++;
            }

            /* Check what FBO Unity rendered to. Unity uses FBO 2 for rendering
               (not FBO 0), so we need to blit from FBO 2 to the eye texture. */
            static int s_unity_fbo = -1;
            {
                GLint curFbo = 0;
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &curFbo);
                static int fb_log = 0;
                if (fb_log < 5) {
                    /* Check FBO 2 content */
                    unsigned char pix[4] = {0};
                    GLint prevFbo = 0;
                    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevFbo);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, 2);
                    glReadPixels(512, 512, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pix);
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                        "Pxr_PollEvent: FBO 2 pixel=%d,%d,%d,%d (curFbo=%d)",
                        pix[0], pix[1], pix[2], pix[3], curFbo);
                    FLOG(buf);
                    /* Also check FBO 0 */
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                    glReadPixels(512, 512, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pix);
                    snprintf(buf, sizeof(buf),
                        "Pxr_PollEvent: FBO 0 pixel=%d,%d,%d,%d",
                        pix[0], pix[1], pix[2], pix[3]);
                    FLOG(buf);
                    /* Check FBO 1 */
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, 1);
                    glReadPixels(512, 512, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pix);
                    snprintf(buf, sizeof(buf),
                        "Pxr_PollEvent: FBO 1 pixel=%d,%d,%d,%d",
                        pix[0], pix[1], pix[2], pix[3]);
                    FLOG(buf);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevFbo);
                    fb_log++;
                }
            }

            for (int i = 0; i < MAX_LAYERS; i++) {
                if (g_layers[i].in_use) {
                    GLuint tex = g_layers[i].textures[0];
                    int w = g_layers[i].width;
                    int h = g_layers[i].height;
                    GLint prevFbo = 0, prevReadFbo = 0, prevDrawFbo = 0;
                    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
                    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
                    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);

                    /* Check what Unity rendered to the eye texture */
                    static int check_log = 0;
                    if (check_log < 10 || (check_log % 300) == 0) {
                        GLuint checkFbo = 0;
                        glGenFramebuffers(1, &checkFbo);
                        glBindFramebuffer(GL_READ_FRAMEBUFFER, checkFbo);
                        glFramebufferTextureLayer(GL_READ_FRAMEBUFFER,
                            GL_COLOR_ATTACHMENT0, tex, 0, 0);
                        GLenum st = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
                        if (st == GL_FRAMEBUFFER_COMPLETE) {
                            unsigned char pix[4] = {0};
                            glReadPixels(w/2, h/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pix);
                            unsigned char pix2[4] = {0};
                            glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pix2);
                            char buf[256];
                            snprintf(buf, sizeof(buf),
                                "EYECHECK[%d]: center=%d,%d,%d,%d corner=%d,%d,%d,%d",
                                 check_log, pix[0], pix[1], pix[2], pix[3],
                                 pix2[0], pix2[1], pix2[2], pix2[3]);
                            FLOG(buf);
                        }
                        glDeleteFramebuffers(1, &checkFbo);
                        check_log++;
                    }

                    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFbo);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFbo);
                    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
                    break;
                }
            }
        }
        /* Check GL context */
        void* (*eglGetCurrentContext)(void) = (void*(*)(void))dlsym(RTLD_DEFAULT, "eglGetCurrentContext");
        if (eglGetCurrentContext) {
            void* ctx = eglGetCurrentContext();
            FLOGI("Pxr_PollEvent: GL ctx=", (int)(uintptr_t)ctx);
        }
        for (int i = 0; i < MAX_LAYERS; i++) {
            if (g_layers[i].in_use) {
                float colorScaleOffset[8] = {1, 1, 1, 1, 0, 0, 0, 0};
                GLuint tex = g_layers[i].textures[0];
                if (pvr.SetCurrentRenderTexture) {
                    pvr.SetCurrentRenderTexture(tex);
                }
                if (g_layers[i].is_multiview) {
                    if (pvr.EnableSinglePass) pvr.EnableSinglePass(1);
                    int* sp_tex = (int*)dlsym(pvr.handle, "g_iSinglePassTexID");
                    if (sp_tex) *sp_tex = tex;
                    if (pvr.SetupLayerData) {
                        pvr.SetupLayerData(0, 1, tex, 0x910A, 0, colorScaleOffset);
                        pvr.SetupLayerData(0, 2, tex, 0x910A, 0, colorScaleOffset);
                    }
                } else {
                    if (pvr.SetupLayerData) {
                        pvr.SetupLayerData(0, 1, tex, 0x0DE1, 0, colorScaleOffset);
                        pvr.SetupLayerData(0, 2, g_layers[i].textures[1], 0x0DE1, 0, colorScaleOffset);
                    }
                }
                /* Set texture IDs in the up struct before calling PVR functions.
                   From Ghidra: BothEyeEndFrameFU and CameraEndFrame write to
                   up+0x4c/0x50, TimeWarp reads up+0x4c/0x50 and copies to
                   up+0x90. We set the source fields the render events read. */
                {
                    char* s = (char*)dlsym(pvr.handle, "up");
                    if (s) {
                        *(unsigned char*)(s + 0x68) = 1;
                        *(int*)(s + 0xf960) = tex;
                        *(int*)(s + 0xf964) = 0;
                        /* up+0xf928 is a frame index (must be <= 63), NOT tex */
                        *(int*)(s + 0xf928) = 0;
                        *(int*)(s + 0xf92c) = 0;
                        *(unsigned char*)(s + 0xd810) = 1;
                        *(int*)(s + 0xdabc) = 0;
                        *(int*)(s + 0xda98) = 0;
                        static int poll_struct_log = 0;
                        if (poll_struct_log < 3) {
                            LOGI("Pxr_PollEvent: set up=%p up+0xf960=%d up+0x68=%d up+0x4c=%d",
                                 s, *(int*)(s + 0xf960), *(unsigned char*)(s + 0x68),
                                 *(int*)(s + 0x4c));
                            poll_struct_log++;
                        }
                    } else {
                        static int null_log = 0;
                        if (null_log < 3) {
                            LOGI("Pxr_PollEvent: dlsym(up) returned NULL!");
                            null_log++;
                        }
                    }
                }
                /* Call PVR_BothEyeEndFrameFU to set eye textures.
                   This calls PVR_CameraEndFrame for both eyes + depth + boundary. */
                if (pvr.BothEyeEndFrameFU) {
                    pvr.BothEyeEndFrameFU(tex, 0, 0);
                    LOGI("Pxr_PollEvent: BothEyeEndFrameFU(%d, 0, 0) called", tex);
                } else if (pvr.CameraEndFrame) {
                    pvr.CameraEndFrame(0, tex);
                    pvr.CameraEndFrame(1, g_layers[i].is_multiview ? tex : g_layers[i].textures[1]);
                }
                /* Check if CameraEndFrame wrote up+0x4c */
                {
                    char* s = (char*)dlsym(pvr.handle, "up");
                    if (s) {
                        static int check_log = 0;
                        if (check_log < 5) {
                            LOGI("Pxr_PollEvent: after BEF up+0x4c=%d up+0x50=%d up+0x90=%d",
                                 *(int*)(s + 0x4c), *(int*)(s + 0x50), *(int*)(s + 0x90));
                            check_log++;
                        }
                    }
                }
                /* Read eye texture content and Unity's FBO for debugging */
                {
                    GLint prevFbo = 0;
                    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
                    static int eye_read_log = 0;
                    if (eye_read_log < 10) {
                        unsigned char pix[4] = {0};
                        void (*realBind)(GLenum, GLuint) = real_glBindFramebuffer ? real_glBindFramebuffer : glBindFramebuffer;
                        if (g_eye_fbo) {
                            realBind(GL_READ_FRAMEBUFFER, g_eye_fbo);
                            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pix);
                            LOGI("Pxr_PollEvent: eyeFBO=%d pixel=%d,%d,%d,%d",
                                 g_eye_fbo, pix[0], pix[1], pix[2], pix[3]);
                        }
                        if (prevFbo > 0) {
                            realBind(GL_READ_FRAMEBUFFER, prevFbo);
                            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pix);
                            LOGI("Pxr_PollEvent: prevFbo=%d pixel=%d,%d,%d,%d",
                                 prevFbo, pix[0], pix[1], pix[2], pix[3]);
                        }
                        realBind(GL_READ_FRAMEBUFFER, prevFbo);
                        eye_read_log++;
                    }
                }
                /* Submit frame via wrapped render event.
                   Save/restore GL state around the render events because
                   the PVR SDK's event handlers change FBO bindings, viewport,
                   etc. Without restoring, Unity's subsequent rendering fails
                   with GL_INVALID_FRAMEBUFFER_OPERATION. */
                LOGI("Pxr_PollEvent: before RenderEvent check, pvr.RenderEvent=%p", pvr.RenderEvent);
                if (pvr.RenderEvent) {
                    /* Save GL state */
                    GLint savedFbo = 0, savedViewport[4] = {0};
                    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &savedFbo);
                    glGetIntegerv(GL_VIEWPORT, savedViewport);
                    GLint savedReadFbo = 0, savedDrawFbo = 0;
                    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &savedReadFbo);
                    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &savedDrawFbo);

                    wrapped_RenderEvent(RENDER_EVENT_BOTH_EYE_END_FRAME);
                    /* Set LE_TexID before TIMEWARP */
                    {
                        char* s = (char*)dlsym(pvr.handle, "up");
                        if (s) {
                            *(int*)(s + 0x90) = tex;
                            *(int*)(s + 0x2390) = g_layers[i].is_multiview ? tex : g_layers[i].textures[1];
                            *(int*)(s + 0xdabc) = 0;
                            *(int*)(s + 0xda98) = 0;
                            *(unsigned char*)(s + 0xd810) = 1;
                        }
                    }
                    wrapped_RenderEvent(RENDER_EVENT_TIMEWARP);
                    /* Re-set LE_TexID AFTER TIMEWARP (which resets it to 0)
                       so the PVR SDK's ATW thread can read it. */
                    {
                        char* s = (char*)dlsym(pvr.handle, "up");
                        if (s) {
                            *(int*)(s + 0x90) = tex;
                            *(int*)(s + 0x2390) = g_layers[i].is_multiview ? tex : g_layers[i].textures[1];
                            *(int*)(s + 0xdabc) = 0;
                            *(int*)(s + 0xda98) = 0;
                            *(unsigned char*)(s + 0xd810) = 1;
                        }
                    }

                    /* Restore GL state */
                    void (*realBind2)(GLenum, GLuint) = real_glBindFramebuffer ? real_glBindFramebuffer : glBindFramebuffer;
                    realBind2(GL_READ_FRAMEBUFFER, savedReadFbo);
                    realBind2(GL_DRAW_FRAMEBUFFER, savedDrawFbo);
                    realBind2(GL_FRAMEBUFFER, savedFbo);
                    glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
                }
                if (poll_count == 0) FLOG("Pxr_PollEvent: first frame submitted");
                poll_count++;
                /* Set up eye FBO with the eye texture and enable redirect.
                   When Unity calls glBindFramebuffer(0), it will be redirected
                   to the eye FBO. Unity's rendering goes to the eye texture. */
                {
                    GLuint tex = g_layers[i].textures[0];
                    if (!g_eye_fbo) glGenFramebuffers(1, &g_eye_fbo);
                    if (real_glBindFramebuffer) {
                        real_glBindFramebuffer(GL_FRAMEBUFFER, g_eye_fbo);
                    } else {
                        glBindFramebuffer(GL_FRAMEBUFFER, g_eye_fbo);
                    }
                    if (g_layers[i].is_multiview && g_glFramebufferTextureMultiview) {
                        g_glFramebufferTextureMultiview(GL_FRAMEBUFFER,
                            GL_COLOR_ATTACHMENT0, tex, 0, 0, 2);
                    } else {
                        glFramebufferTextureLayer(GL_FRAMEBUFFER,
                            GL_COLOR_ATTACHMENT0, tex, 0, 0);
                    }
                    g_redirect_fbo = 1;
                    static int redirect_log = 0;
                    if (redirect_log < 3) {
                        LOGI("Pxr_PollEvent: eye FBO=%u set up, redirect enabled tex=%u", g_eye_fbo, tex);
                        redirect_log++;
                    }
                }
                break;
            }
        }
    }
    return 0;
}

/* ---- Missing functions needed by libPxrPlatform.so ---- */

JNIEXPORT int Pxr_Construct() {
    LOGI("Pxr_Construct called");
    FLOG("Pxr_Construct called");
    /* Try calling libPxrPlatform.so's Pxr_Construct to initialize the
       display subsystem. This is what Unity's XR Management should call
       but doesn't, so we call it ourselves. */
    static int (*pplatform_construct)(void) = NULL;
    if (!pplatform_construct) {
        void* h = dlopen("libPxrPlatform.so", RTLD_NOW | RTLD_NOLOAD);
        if (!h) h = dlopen("libPxrPlatform.so", RTLD_NOW);
        if (h) {
            pplatform_construct = (int(*)(void))dlsym(h, "Pxr_Construct");
            LOGI("Pxr_Construct: found libPxrPlatform.so Pxr_Construct=%p", pplatform_construct);
        } else {
            LOGI("Pxr_Construct: failed to load libPxrPlatform.so: %s", dlerror());
        }
    }
    if (pplatform_construct) {
        int ret = pplatform_construct();
        LOGI("Pxr_Construct: libPxrPlatform.so returned %d", ret);
        return ret;
    }
    return 0;
}
JNIEXPORT int Pxr_LoadPlugin(const char* name) { LOGI("Pxr_LoadPlugin(%s) -> 0", name ? name : "null"); return 0; }
JNIEXPORT int Pxr_UnloadPlugin() { LOGI("Pxr_UnloadPlugin -> 0"); return 0; }
JNIEXPORT int Pxr_GetFocusState() { return 1; }
JNIEXPORT int Pxr_IsSensorReady() { return 1; }
JNIEXPORT int Pxr_GetSensorStatus() { return 1; }
JNIEXPORT int Pxr_GetSeeThroughState() { return 0; }
JNIEXPORT int Pxr_GetMRCEnable() { return 0; }
JNIEXPORT int Pxr_GetHomeKey() { return 0; }
JNIEXPORT int Pxr_InitHomeKey() { return 0; }
JNIEXPORT int Pxr_SetFoveationLevelEnable(int enable) { return 0; }
JNIEXPORT int Pxr_SetInputDeviceChangedCallBack(void* cb) { return 0; }
JNIEXPORT int Pxr_SetLayerBlend(int layerId, float alpha) { return 0; }
JNIEXPORT int Pxr_SetLogInfoActive(int active, int level) { return 0; }
JNIEXPORT int Pxr_SetSRPState(int state) { return 0; }
JNIEXPORT int Pxr_SetUserDefinedSettings(void* settings) { return 0; }
JNIEXPORT int Pxr_SetVideoSeethroughState(int state) { return 0; }
JNIEXPORT int Pxr_CreateLayerParam(void* param) { return 0; }
JNIEXPORT int Pxr_EnableEyeTracking(int enable) { return 0; }
JNIEXPORT int Pxr_EnableFaceTracking(int enable) { return 0; }
JNIEXPORT int Pxr_EnableLipsync(int enable) { return 0; }
JNIEXPORT void* Pxr_GetLayerImagePtr(int layerId, int eye, int imageIndex) {
    if (layerId < 0 || layerId >= MAX_LAYERS || !g_layers[layerId].in_use)
        return NULL;
    return (void*)(uintptr_t)g_layers[layerId].textures[imageIndex % SWAPCHAIN_LEN];
}

/* ---- Multiview ---- */

JNIEXPORT int Pxr_EnableMultiview(int enable) {
    LOGI("Pxr_EnableMultiview(%d)", enable);
    FLOG("Pxr_EnableMultiview");
    return 0;
}

/* ---- Color space ---- */

JNIEXPORT int Pxr_SetColorSpace(int space) { return 0; }

/* ---- Mono mode ---- */

JNIEXPORT int Pxr_SetMonoMode(int enable) { return 0; }

/* ---- IPD ---- */

JNIEXPORT int Pxr_SetIPD(float ipd) { return 0; }
JNIEXPORT int Pxr_GetIPD(float* ipd) { if (ipd) *ipd = 0.064f; return 0; }
JNIEXPORT int Pxr_GetPupilDistance(float* pd) { if (pd) *pd = 0.064f; return 0; }
JNIEXPORT int Pxr_SetTrackingIPDEnabled(int e) { return 0; }
JNIEXPORT int Pxr_GetTrackingIPDEnabled(int* e) { if (e) *e = 0; return 0; }

/* ---- Psensor ---- */

JNIEXPORT int Pxr_InitPsensor() {
    return 0;
}
JNIEXPORT int Pxr_UnregisterPsensor() {
    return 0;
}
JNIEXPORT int Pxr_getPsensorState() {
    /* Don't call Pvr_GetPsensorState - it crashes on Neo 2 due to
       uninitialized OcFusion. Just return "not near" (0). */
    return 0;
}

/* ---- Controller stubs ---- */

JNIEXPORT int Pxr_GetControllerConnectStatus(unsigned int id, void* status) {
    /* libPxrPlatform.so may pass this as a value or pointer depending on version.
       Just return connected=1 without touching the second arg to avoid crashes. */
    return 1;
}

JNIEXPORT int Pxr_GetControllerTrackingState(unsigned int id, double t,
        float* headData, void* tracking) {
    if (tracking) memset(tracking, 0, 112); /* PxrControllerTracking size */
    return 0;
}

JNIEXPORT int Pxr_GetControllerInputState(unsigned int id, void* state) {
    if (state) memset(state, 0, 64);
    return 0;
}

JNIEXPORT int Pxr_GetControllerCapabilities(unsigned int id, void* cap) {
    if (cap) memset(cap, 0, 24);
    return 0;
}

JNIEXPORT int Pxr_SetControllerVibration(unsigned int id, float strength, int time) {
    return 0;
}

JNIEXPORT int Pxr_GetActiveInputDeviceType(int* type) {
    if (type) *type = 1;
    return 0;
}

JNIEXPORT int Pxr_GetControllerinfo(unsigned int id, void* info) { return 0; }
JNIEXPORT int Pxr_GetControllerInputEvent(void* event) { return 0; }
JNIEXPORT int Pxr_GetControllerKeyEventExt(void* e) { return 0; }
JNIEXPORT int Pxr_GetControllerTouchEvent(void* e) { return 0; }
JNIEXPORT int Pxr_GetControllerMainInputHandle(int* id) { if (id) *id = 0; return 0; }
JNIEXPORT int Pxr_SetControllerMainInputHandle(unsigned int id) { return 0; }
JNIEXPORT int Pxr_SetControllerEnableKey(int e, int key) { return 0; }
JNIEXPORT void Pxr_SetControllerOriginOffset(int id, float x, float y, float z) {}
JNIEXPORT int Pxr_RecenterInputPose(int id) { return 0; }

/* ---- Boundary / Guardian stubs ---- */

JNIEXPORT int Pxr_GetBoundaryEnabled(int* e) { if (e) *e = 0; return 0; }
JNIEXPORT int Pxr_GetBoundaryConfigured(int* e) { if (e) *e = 0; return 0; }
JNIEXPORT int Pxr_GetBoundaryVisible(int* e) { if (e) *e = 0; return 0; }
JNIEXPORT int Pxr_SetBoundaryVisible(int e) { return 0; }
JNIEXPORT int Pxr_DisableBoundary() { return 0; }
JNIEXPORT int Pxr_TestNodeIsInBoundary(int n, int p, void* i) { return 0; }
JNIEXPORT int Pxr_TestPointIsInBoundary(void* p, int a, void* i) { return 0; }
JNIEXPORT int Pxr_GetBoundaryGeometry(int p, unsigned int cin, unsigned int* cout, void* out) { return 0; }
JNIEXPORT int Pxr_GetBoundaryGeometry2(int p, unsigned int cin, unsigned int* cout, void* out) { return 0; }
JNIEXPORT int Pxr_GetBoundaryDimensions(int p, void* dim) { return 0; }
JNIEXPORT int Pxr_StartSdkBoundary() { return 0; }
JNIEXPORT int Pxr_ShutdownSdkGuardianSystem() { return 0; }
JNIEXPORT int Pxr_SetGuardianSystemDisable(int d) { return 0; }
JNIEXPORT int Pxr_PauseGuardianSystemForSTS() { return 0; }
JNIEXPORT int Pxr_ResumeGuardianSystemForSTS() { return 0; }
JNIEXPORT int Pxr_GetRoomModeState(int* s) { if (s) *s = 0; return 0; }

/* ---- See-through / Camera stubs ---- */

JNIEXPORT int Pxr_GetSeeThroughData(void* d) { return 0; }
JNIEXPORT int Pxr_StartCameraPreview() { return 0; }
JNIEXPORT int Pxr_SetSeeThroughBackground(int e) { return 0; }
JNIEXPORT int Pxr_SetSeeThroughVisible(int e) { return 0; }
JNIEXPORT int Pxr_SetSeeThroughImageExtent(int w, int h) { return 0; }
JNIEXPORT int Pxr_GetCameraDataExt(void* d) { return 0; }

/* ---- Eye tracking stubs ---- */

JNIEXPORT int Pxr_GetEyeTrackingData(void* d) { return 0; }
JNIEXPORT int Pxr_GetEyeTrackingAutoIPD(float* ipd) { if (ipd) *ipd = 0.064f; return 0; }
JNIEXPORT int Pxr_StartEyeTracking() { return 0; }
JNIEXPORT int Pxr_StopEyeTracking() { return 0; }
JNIEXPORT int Pxr_GetEyeOrientation(int e, void* o) { return 0; }

/* ---- Hand tracking stubs ---- */

JNIEXPORT int Pxr_GetHandTrackingEnabled(int* e) { if (e) *e = 0; return 0; }
JNIEXPORT int Pxr_GetHandTrackingHandState(int h, void* s) { return 0; }
JNIEXPORT int Pxr_GetHandTrackingSkeleton(int h, void* s) { return 0; }
JNIEXPORT int Pxr_GetHandTrackingMesh(int h, void* m) { return 0; }
JNIEXPORT int Pxr_SetAppHandTrackingEnabled(int e) { return 0; }
JNIEXPORT int Pxr_GetHandTrackerActiveInputType(void* a) { return 0; }
JNIEXPORT int Pxr_GetHandTrackerAimState(void* a) { return 0; }
JNIEXPORT int Pxr_GetHandTrackerJointLocations(void* a) { return 0; }
JNIEXPORT int Pxr_GetHandTrackerJointLocationsWithPredictTime(void* a) { return 0; }
JNIEXPORT int Pxr_GetHandTrackerSettingState(void* a) { return 0; }

/* ---- Face tracking stubs ---- */

JNIEXPORT int Pxr_GetFaceTrackingData(void* d) { return 0; }

/* ---- Body tracking stubs ---- */

JNIEXPORT int Pxr_GetBodyTrackingImuData(void* d) { return 0; }
JNIEXPORT int Pxr_GetBodyTrackingPose(void* d) { return 0; }
JNIEXPORT int Pxr_SetBodyTrackingStaticCalibState(int s) { return 0; }

/* ---- Foveation stubs ---- */

JNIEXPORT int Pxr_SetFoveationLevel(int level) { return 0; }
JNIEXPORT int Pxr_GetFoveationLevel() { return 0; }
JNIEXPORT int Pxr_SetFoveationParams(void* p) { return 0; }
JNIEXPORT int Pxr_GetFeatureSupported(int f) { return 0; }

/* ---- MRC stubs ---- */

JNIEXPORT int Pxr_GetMrcStatus(int* s) { if (s) *s = 0; return 0; }
JNIEXPORT int Pxr_GetMrcPose(void* p) { return 0; }
JNIEXPORT int Pxr_SetMrcPose(void* p) { return 0; }
JNIEXPORT int Pxr_SetIsSupportMovingMrc(int e) { return 0; }

/* ---- Vulkan stubs ---- */

JNIEXPORT int Pxr_CreateVulkanSystem(void* a, void* b, void* c, void* d) { return 0; }
JNIEXPORT int Pxr_GetInstanceExtensionsXRPlatform(void* a, void* b) { return 0; }
JNIEXPORT int Pxr_GetDeviceExtensionsXRPlatform(void* a, void* b) { return 0; }
JNIEXPORT int Pxr_GetInstanceExtensionsVk(void* a, void* b) { return 0; }
JNIEXPORT int Pxr_GetDeviceExtensionsVk(void* a, void* b) { return 0; }
JNIEXPORT void Pxr_SetVulkanBinding(void* a) {}

/* ---- Performance stubs ---- */

JNIEXPORT int Pxr_SetPerformanceLevels(int cpu, int gpu) { return 0; }
JNIEXPORT int Pxr_GetPerformanceLevels(int* cpu, int* gpu) { return 0; }

/* ---- Misc stubs ---- */

JNIEXPORT void Pxr_LogPrint(int level, const char* tag, const char* fmt, ...) {}
JNIEXPORT int Pxr_GetDialogState() { return 0; }
JNIEXPORT int Pxr_GetIntSysProc(const char* prop, int* val) { if (val) *val = 0; return 0; }
JNIEXPORT int Pxr_GetIntMetaFromApplication(const char* key, int* val) { if (val) *val = 0; return 0; }
JNIEXPORT int Pxr_GetStringMetaFromApplication(const char* key, char* buf, int len) { if (buf && len > 0) buf[0] = 0; return 0; }
JNIEXPORT void Pxr_getMainClientInfo(void* a) {}
JNIEXPORT int Pxr_SetEngineVersion(const char* v) { return 0; }
JNIEXPORT int Pxr_SetOverlayApp(int e) { return 0; }
JNIEXPORT int Pxr_SetIsEnableHomeKey(int e) { return 0; }
JNIEXPORT int Pxr_SetSensorLostCustomMode(int e) { return 0; }
JNIEXPORT int Pxr_SetSensorLostCMST(int e) { return 0; }
JNIEXPORT void Pxr_SetInputEventCallback(void* cb) {}
JNIEXPORT void Pxr_RegisteKeyEventCallback(void* cb) {}
JNIEXPORT int Pxr_InvokeFunctions(int a, void* b) { return 0; }
JNIEXPORT int Pxr_GetLayerAndroidSurface(int a, int b, void* c) { return 0; }
JNIEXPORT void Pxr_SetRenderEventData(int a, int b) {}
JNIEXPORT int Pxr_GetStencilmesh(void* a) { return 0; }

/* ---- Controller vibration extended stubs ---- */
JNIEXPORT void Pxr_SetControllerAmp(unsigned int id, float amp) {}
JNIEXPORT void Pxr_SetControllerDelay(unsigned int id, int delay) {}
JNIEXPORT int Pxr_SetControllerVibrationEvent(unsigned int id, int event) { return 0; }
JNIEXPORT int Pxr_StartControllerVCMotor(unsigned int id, int freq, int amp) { return 0; }
JNIEXPORT int Pxr_StopControllerVCMotor(unsigned int id) { return 0; }
JNIEXPORT int Pxr_StartVibrateByCache(unsigned int id) { return 0; }
JNIEXPORT int Pxr_StartVibrateByPHF(unsigned int id, const char* path) { return 0; }
JNIEXPORT int Pxr_StartVibrateBySharemF(unsigned int id, const char* path) { return 0; }
JNIEXPORT int Pxr_StartVibrateBySharemU(unsigned int id, const char* path) { return 0; }
JNIEXPORT int Pxr_ClearVibrateByCache(unsigned int id) { return 0; }
JNIEXPORT int Pxr_PauseVibrate(unsigned int id) { return 0; }
JNIEXPORT int Pxr_ResumeVibrate(unsigned int id) { return 0; }
JNIEXPORT int Pxr_GetVibrateDelayTime(unsigned int id, int* delay) { if (delay) *delay = 0; return 0; }
JNIEXPORT int Pxr_UpdateVibrateParams(unsigned int id, void* params) { return 0; }
JNIEXPORT int Pxr_SetControllerEnterPairing(unsigned int id) { return 0; }
JNIEXPORT int Pxr_SetControllerStopPairing(unsigned int id) { return 0; }
JNIEXPORT int Pxr_SetControllerUnbind(unsigned int id) { return 0; }
JNIEXPORT int Pxr_SetControllerUpgrade(unsigned int id) { return 0; }
JNIEXPORT void Pxr_SetControllerPoseToBoundary(int a, int b) {}
JNIEXPORT int Pxr_StartCVControllerThread() { return 0; }
JNIEXPORT int Pxr_StopCVControllerThread() { return 0; }
JNIEXPORT void Pxr_SetOriginOfLargeSpace(float x, float y, float z) {}

/* ---- Render event func (for GL.IssuePluginEvent) ---- */

typedef void (*RenderEventFunc_t)(int event);

/* GL multiview wrapper functions (exported for display subsystem) */
JNIEXPORT void glFramebufferTextureMultiview(GLenum target, GLenum attachment,
        GLuint texture, GLint level, GLint baseViewIndex, GLsizei numViews) {
    if (g_glFramebufferTextureMultiview) {
        g_glFramebufferTextureMultiview(target, attachment, texture, level, baseViewIndex, numViews);
    }
}

JNIEXPORT void glFramebufferTexturesampleMultiview(GLenum target, GLenum attachment,
        GLuint texture, GLint level, GLint baseViewIndex, GLsizei numViews) {
    if (g_glFramebufferTextureSampleMultiview) {
        g_glFramebufferTextureSampleMultiview(target, attachment, texture, level, baseViewIndex, numViews);
    }
}

/* Wrapped render event function - intercepts GL.IssuePluginEvent calls
   to copy the default framebuffer to the eye texture before TimeWarp. */
static void wrapped_RenderEvent(int event) {
    static int event_log_count = 0;
    FLOGI("wrapped_RenderEvent: event=", event);
    if (event_log_count < 50) {
        LOGI("wrapped_RenderEvent: event=%d g_render_thread_inited=%d", event, g_render_thread_inited);
        event_log_count++;
    }
    /* Set texture IDs in the up struct before calling PVR render events. */
    if (g_render_thread_inited) {
        char* s = (char*)dlsym(pvr.handle, "up");
        if (s) {
            for (int i = 0; i < MAX_LAYERS; i++) {
                if (g_layers[i].in_use) {
                    GLuint tex = g_layers[i].textures[0];
                    *(unsigned char*)(s + 0x68) = 1;
                    *(int*)(s + 0xf960) = tex;
                    *(int*)(s + 0xf964) = 0;
                    *(int*)(s + 0xf928) = 0;
                    *(int*)(s + 0xf92c) = 0;
                    *(unsigned char*)(s + 0xd810) = 1;
                    *(int*)(s + 0xdabc) = 0;
                    break;
                }
            }
        }
    }
    if (pvr_real_RenderEvent) pvr_real_RenderEvent(event);
}

JNIEXPORT void* GetRenderEventFunc() {
    /* Return our wrapper so we can intercept events and copy framebuffer */
    if (pvr.RenderEvent) return (void*)wrapped_RenderEvent;
    return NULL;
}

JNIEXPORT void OnRenderEvent(int event) {
    wrapped_RenderEvent(event);
}

/* ---- JNI functions (called from Java pxr_api classes) ---- */

JNIEXPORT void JNICALL Java_com_pxr_xrlib_PicovrSDK_pluginInit(JNIEnv* env, jobject thiz) {
    LOGI("PicovrSDK_pluginInit");
    init_pvr_runtime(env);
}

JNIEXPORT void JNICALL Java_com_pxr_xrlib_PicovrSDK_setControllerEventCallback(JNIEnv* env, jobject thiz) {}
JNIEXPORT void JNICALL Java_com_pxr_xrlib_PicovrSDK_setEnableCpt(JNIEnv* env, jobject thiz, jint e) {}
JNIEXPORT void JNICALL Java_com_pxr_xrlib_PicovrSDK_setEnableSinglePass(JNIEnv* env, jobject thiz, jint e) {}
JNIEXPORT void JNICALL Java_com_pxr_xrlib_PicovrSDK_setGraphicOption(JNIEnv* env, jobject thiz, jint e) {}
JNIEXPORT void JNICALL Java_com_pxr_xrlib_PicovrSDK_setPlatformOption(JNIEnv* env, jobject thiz, jint a, jint b) {}
JNIEXPORT void JNICALL Java_com_pxr_xrlib_PicovrSDK_setPresentationFlag(JNIEnv* env, jobject thiz, jint e) {}
JNIEXPORT void JNICALL Java_com_pxr_xrlib_PicovrSDK_setSwapchainEXT(JNIEnv* env, jobject thiz, jint e) {
    LOGI("setSwapchainEXT(%d)", e);
    /* Load GL multiview extension functions */
    g_glFramebufferTextureMultiview = (PFNGLFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC)eglGetProcAddress("glFramebufferTextureMultiviewOVR");
    g_glFramebufferTextureSampleMultiview = (PFNGLFRAMEBUFFERTEXTURESAMPLEMULTIVIEWOVRPROC)eglGetProcAddress("glFramebufferTextureSampleMultiviewOVR");
    if (!g_glFramebufferTextureMultiview)
        g_glFramebufferTextureMultiview = (PFNGLFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC)eglGetProcAddress("glFramebufferTextureMultiview");
    if (!g_glFramebufferTextureSampleMultiview)
        g_glFramebufferTextureSampleMultiview = (PFNGLFRAMEBUFFERTEXTURESAMPLEMULTIVIEWOVRPROC)eglGetProcAddress("glFramebufferTextureSampleMultiview");
    LOGI("setSwapchainEXT: multiview=%p sampleMultiview=%p", g_glFramebufferTextureMultiview, g_glFramebufferTextureSampleMultiview);
}
JNIEXPORT void JNICALL Java_com_pxr_xrlib_PicovrSDK_ResetSensor(JNIEnv* env, jobject thiz, jint e) {
    if (pvr.ResetSensor) pvr.ResetSensor(0);
}
JNIEXPORT jint JNICALL Java_com_pxr_xrlib_PicovrSDK_getConfigProperty(JNIEnv* env, jobject thiz, jint key) {
    return 0;
}
JNIEXPORT void JNICALL Java_com_pxr_xrlib_PicovrSDK_setConfigProperty(JNIEnv* env, jobject thiz, jint key, jint val) {}
JNIEXPORT jint JNICALL Java_com_pxr_xrlib_PicovrSDK_getSerialNumber(JNIEnv* env, jobject thiz, jbyteArray arr) {
    return 0;
}
JNIEXPORT void JNICALL Java_com_pxr_xrlib_KeyEventManager_initialize(JNIEnv* env, jobject thiz) {}
