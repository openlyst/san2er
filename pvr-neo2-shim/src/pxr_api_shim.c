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
    Pvr_TimeWarpEvent_t TimeWarpEvent;
    UnityRenderEvent_t RenderEvent;
    Pvr_GetPsensorState_t GetPsensorState;
    Pvr_GetSensorState_t GetSensorState;
    Pvr_SetCurrentHMDType_t SetCurrentHMDType;
    Pvr_GetSupportHMDTypes_t GetSupportHMDTypes;
    Pvr_ChangeScreenParameters_t ChangeScreenParameters;
} pvr;

/* ---- State ---- */

static JavaVM* g_jvm = NULL;
static int g_initialized = 0;
static int g_init_requested = 0;
static int g_xr_running = 0;
static int g_render_thread_inited = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* GL multiview extension function pointers */
typedef void (*PFNGLFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC)(GLenum, GLenum, GLuint, GLint, GLint, GLsizei);
typedef void (*PFNGLFRAMEBUFFERTEXTURESAMPLEMULTIVIEWOVRPROC)(GLenum, GLenum, GLuint, GLint, GLint, GLsizei);
static PFNGLFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC g_glFramebufferTextureMultiview = NULL;
static PFNGLFRAMEBUFFERTEXTURESAMPLEMULTIVIEWOVRPROC g_glFramebufferTextureSampleMultiview = NULL;

/* Layer/swapchain state */
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
    LOAD("PVR_TimeWarpEvent_", TimeWarpEvent);
    LOAD("UnityRenderEvent", RenderEvent);
    LOAD("Pvr_GetPsensorState", GetPsensorState);
    LOAD("Pvr_GetSensorState", GetSensorState);
    LOAD("Pvr_SetCurrentHMDType", SetCurrentHMDType);
    LOAD("Pvr_GetSupportHMDTypes", GetSupportHMDTypes);
    LOAD("Pvr_ChangeScreenParameters", ChangeScreenParameters);
    #undef LOAD

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
    if (g_initialized) return;
    pthread_mutex_lock(&g_lock);
    if (g_initialized) {
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
       libPxrPlatform.so loads us as a dependency), do the init now. */
    if (g_init_requested && !g_initialized) {
        FLOG("JNI_OnLoad: init_requested but not initialized, calling init_pvr_runtime");
        JNIEnv* env = NULL;
        if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) == JNI_OK) {
            init_pvr_runtime(env);
            FLOG("JNI_OnLoad: init_pvr_runtime done, g_initialized=1?");
        } else {
            FLOG("JNI_OnLoad: GetEnv failed");
        }
    } else {
        FLOG("JNI_OnLoad: init_requested but g_initialized check");
    }
    FLOG("JNI_OnLoad done");
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
           us as a dependency before the Java runtime registers us. */
        FLOG("Pxr_Initialize: no JNIEnv yet, deferring to JNI_OnLoad");
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
       use GL_TEXTURE_2D_ARRAY with 2 layers for stereo rendering. */
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

    if (p) p->layerId = slot;

    FLOG("Pxr_CreateLayer step 9: auto-start XR");
    /* Auto-start XR if not already running - the game creates a layer
       when it's ready to render, so this is the right time to start XR */
    if (!g_xr_running) {
        LOGI("auto-starting XR (Pxr_BeginXr)");
        g_xr_running = 1;
    }

    FLOG("Pxr_CreateLayer step 10: init render thread");
    /* Set up the layer textures for PVR TimeWarp and init render thread.
       We're on the render thread so EGL context is available. */
    EGLContext ctx = eglGetCurrentContext();
    EGLDisplay dpy = eglGetCurrentDisplay();
    FLOGI("Pxr_CreateLayer: EGL ctx=", (int)(uintptr_t)ctx);
    FLOGI("Pxr_CreateLayer: EGL dpy=", (int)(uintptr_t)dpy);
    if (!g_render_thread_inited && pvr.RenderEvent) {
        LOGI("initializing PVR render thread from Pxr_CreateLayer");
        pvr.RenderEvent(RENDER_EVENT_INIT_RENDER_THREAD);
        g_render_thread_inited = 1;
        LOGI("PVR render thread init returned");
    }
    FLOG("Pxr_CreateLayer step 11: render thread done");

    /* Set up the display surface for TimeWarp. The PVR SDK needs an
       EGL window surface to present warped frames to the headset.
       Get the ANativeWindow from the Activity's ViewRootImpl.mSurface
       and call initialize() to create the EGL surface. */
    if (g_render_thread_inited) {
        typedef void (*initialize_t)(void*);
        initialize_t init_fn = (initialize_t)dlsym(pvr.handle, "_Z10initializeP13ANativeWindow");
        if (init_fn) {
            JNIEnv* env = get_env();
            if (env) {
                /* Get the current Activity via ActivityThread */
                jclass at_cls = (*env)->FindClass(env, "android/app/ActivityThread");
                if (at_cls) {
                    jmethodID cat = (*env)->GetStaticMethodID(env, at_cls,
                        "currentActivityThread", "()Landroid/app/ActivityThread;");
                    jobject at_obj = (*env)->CallStaticObjectMethod(env, at_cls, cat);
                    if (at_obj) {
                        jfieldID mf = (*env)->GetFieldID(env, at_cls,
                            "mActivities", "Landroid/util/ArrayMap;");
                        if (mf) {
                            jobject map = (*env)->GetObjectField(env, at_obj, mf);
                            if (map) {
                                jclass map_cls = (*env)->GetObjectClass(env, map);
                                jmethodID size_m = (*env)->GetMethodID(env, map_cls, "size", "()I");
                                jmethodID val_m = (*env)->GetMethodID(env, map_cls, "valueAt", "(I)Ljava/lang/Object;");
                                int n = (*env)->CallIntMethod(env, map, size_m);
                                if (n > 0) {
                                    jobject record = (*env)->CallObjectMethod(env, map, val_m, 0);
                                    if (record) {
                                        jclass rec_cls = (*env)->GetObjectClass(env, record);
                                        jfieldID act_f = (*env)->GetFieldID(env, rec_cls,
                                            "activity", "Landroid/app/Activity;");
                                        if (act_f) {
                                            jobject activity = (*env)->GetObjectField(env, record, act_f);
                                            if (activity) {
                                                /* Activity -> getWindow() -> getDecorView()
                                                   -> getViewRootImpl() -> mSurface */
                                                jclass act_cls = (*env)->GetObjectClass(env, activity);
                                                jmethodID gw = (*env)->GetMethodID(env, act_cls,
                                                    "getWindow", "()Landroid/view/Window;");
                                                jobject window = (*env)->CallObjectMethod(env, activity, gw);
                                                if (window) {
                                                    jclass win_cls = (*env)->GetObjectClass(env, window);
                                                    jmethodID gdv = (*env)->GetMethodID(env, win_cls,
                                                        "getDecorView", "()Landroid/view/View;");
                                                    jobject decor = (*env)->CallObjectMethod(env, window, gdv);
                                                    if (decor) {
                                                        jclass view_cls = (*env)->GetObjectClass(env, decor);
                                                        jmethodID gvr = (*env)->GetMethodID(env, view_cls,
                                                            "getViewRootImpl", "()Landroid/view/ViewRootImpl;");
                                                        jobject vroot = (*env)->CallObjectMethod(env, decor, gvr);
                                                        if (vroot) {
                                                            jclass vr_cls = (*env)->GetObjectClass(env, vroot);
                                                            jfieldID sf = (*env)->GetFieldID(env, vr_cls,
                                                                "mSurface", "Landroid/view/Surface;");
                                                            if (sf) {
                                                                jobject surface = (*env)->GetObjectField(env, vroot, sf);
                                                                if (surface) {
                                                                    void* (*ANW_fromSurface)(JNIEnv*, jobject) =
                                                                        (void*(*)(JNIEnv*, jobject))
                                                                        dlsym(RTLD_DEFAULT, "ANativeWindow_fromSurface");
                                                                    if (ANW_fromSurface) {
                                                                        void* anw = ANW_fromSurface(env, surface);
                                                                        FLOGI("Pxr_CreateLayer: ANativeWindow=", (int)(uintptr_t)anw);
                                                                        if (anw) {
                                                                            init_fn(anw);
                                                                            FLOG("Pxr_CreateLayer: initialize() called");
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (pvr.SetupLayerData && pvr.RenderEvent) {
        float colorScaleOffset[8] = {1, 1, 1, 1, 0, 0, 0, 0};
        GLuint tex = g_layers[slot].textures[0];
        /* PVR SDK uses GL enum values for textureType:
           GL_TEXTURE_2D=0x0DE1, GL_TEXTURE_2D_ARRAY=0x910A */
        int texType = is_multiview ? 0x910A : 0x0DE1;
        LOGI("setting up PVR layer data with tex %u (type 0x%x, mv=%d)", tex, texType, is_multiview);

        /* Clear textures to a solid color via FBO so TimeWarp has valid
           eye buffers. Without this, TimeWarp rejects them as "No valid
           Eye Buffers" because they've never been rendered to. */
        GLuint fbo;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        if (is_multiview) {
            if (g_glFramebufferTextureMultiview) {
                g_glFramebufferTextureMultiview(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                    tex, 0, 0, arraySize);
            }
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
                *g_iSinglePassTexID = tex;
                FLOGI("Pxr_CreateLayer: g_iSinglePassTexID=", (int)tex);
            }
        }

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
        /* Also set textures directly in the PVR structure, in case
           CameraEndFrame_ skips due to the flag at offset 104 being 0.
           Read the actual struct pointer from the GOT entry. */
        {
            char* evbuf = (char*)dlsym(pvr.handle, "eventBuffer");
            if (evbuf) {
                /* GOT entry at link-time 0x1fa100 contains the struct pointer.
                   eventBuffer is at link-time 0x210000.
                   GOT offset from eventBuffer = 0x1fa100 - 0x210000 = -0x15F00 */
                char** got_entry = (char**)(evbuf - 0x15F00);
                char* pvr_struct = *got_entry;
                FLOGI("Pxr_CreateLayer: evbuf=", (int)(uintptr_t)evbuf);
                FLOGI("Pxr_CreateLayer: pvr_struct=", (int)(uintptr_t)pvr_struct);
                if (pvr_struct) {
                    pvr_struct[104] = 1;
                    *(int*)(pvr_struct + 76) = tex;
                    *(int*)(pvr_struct + 80) = is_multiview ? tex : g_layers[slot].textures[1];
                    /* Clear overlay model flags. PVR_TimeWarpEvent_ checks
                       struct[0xdabc] (==1 → overlay, ==0 → eyebuffer).
                       Also clear struct[0xda98] and struct[0x5364]. */
                    *(int*)(pvr_struct + 0xdabc) = 0;
                    *(int*)(pvr_struct + 0xda98) = 0;
                    *(int*)(pvr_struct + 0x5364) = 0;
                    /* Set the flag at struct[0xd810] so the eyebuffer path
                       is taken without needing struct[80] to be checked. */
                    *(unsigned char*)(pvr_struct + 0xd810) = 1;
                    /* Store texture IDs at offsets read by UnityRenderEvent_.
                       BOTH_EYE_END_FRAME reads [struct+0xf960]+[struct+0xf964]
                         and passes the sum to PVR_CameraEndFrame as texture ID.
                       TIMEWARP reads [struct+0xf928]+[struct+0xf92c]
                         and passes the sum to PVR_TimeWarpEvent as a parameter.
                       PVR_TimeWarpEvent_ checks parameter <= 63 and exits
                         early if not. This parameter is NOT the texture ID -
                         it's a small frame/sequence value. Store 0 there. */
                    *(int*)(pvr_struct + 0xf960) = tex;
                    *(int*)(pvr_struct + 0xf964) = 0;
                    *(int*)(pvr_struct + 0xf928) = 0;
                    *(int*)(pvr_struct + 0xf92c) = 0;
                    /* Also store right eye texture for non-multiview */
                    if (!is_multiview) {
                        *(int*)(pvr_struct + 0xf918) = g_layers[slot].textures[1];
                        *(int*)(pvr_struct + 0xf91c) = 0;
                    }
                    FLOG("Pxr_CreateLayer: set struct flag+tex via GOT");
                }
            }
        }
        /* Submit frame via UnityRenderEvent_ (bypasses message queue).
           Call BOTH_EYE_END_FRAME to set eye textures, then TIMEWARP. */
        if (pvr.RenderEvent) {
            /* Check overlay flag before rendering */
            {
                char* evbuf = (char*)dlsym(pvr.handle, "eventBuffer");
                if (evbuf) {
                    char** got_entry = (char**)(evbuf - 0x15F00);
                    char* s = *got_entry;
                    if (s) {
                        FLOGI("Pxr_CreateLayer: pre-render dabc=", *(int*)(s + 0xdabc));
                    }
                }
            }
            pvr.RenderEvent(RENDER_EVENT_BOTH_EYE_END_FRAME);
            /* Check overlay flag after BOTH_EYE */
            {
                char* evbuf = (char*)dlsym(pvr.handle, "eventBuffer");
                if (evbuf) {
                    char** got_entry = (char**)(evbuf - 0x15F00);
                    char* s = *got_entry;
                    if (s) {
                        FLOGI("Pxr_CreateLayer: post-both-eye dabc=", *(int*)(s + 0xdabc));
                        /* Force-clear overlay flag right before TIMEWARP */
                        *(int*)(s + 0xdabc) = 0;
                        *(unsigned char*)(s + 0xd810) = 1;
                        FLOGI("Pxr_CreateLayer: post-clear dabc=", *(int*)(s + 0xdabc));
                    }
                }
            }
            pvr.RenderEvent(RENDER_EVENT_TIMEWARP);
            FLOG("Pxr_CreateLayer: RenderEvent BOTH_EYE+TIMEWARP called");
        }
        LOGI("submitted first frame to PVR TimeWarp");
    }
    FLOG("Pxr_CreateLayer step 12: layer data submitted");

    /* Frame submission is handled by Pxr_PollEvent (called every frame
       from the same thread as Pxr_CreateLayer, ensuring GL context
       compatibility). No background thread needed. */

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
    if (layerId < 0 || layerId >= MAX_LAYERS || !g_layers[layerId].in_use)
        return -1;
    if (imageIndex < 0 || imageIndex >= SWAPCHAIN_LEN)
        return -1;
    if (image) *image = (unsigned long long)g_layers[layerId].textures[imageIndex];
    LOGI("Pxr_GetLayerImage(%d, %d, %d) -> %llu", layerId, eye, imageIndex, image ? *image : 0);
    FLOG("Pxr_GetLayerImage");
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
    /* Submit frames to PVR TimeWarp from here - this is called every
       frame from the same thread that called Pxr_CreateLayer, ensuring
       GL context compatibility. The game's render loop never starts
       (display subsystem Start() is never called), so we feed TimeWarp
       directly. */
    static int poll_count = 0;
    if (g_render_thread_inited && pvr.RenderEvent) {
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
                /* Call PVR_CameraEndFrame_ directly to set eye textures.
                   The event queue is empty because nothing pushes to it,
                   so UnityRenderEvent(BOTH_EYE_END_FRAME) does nothing.
                   Bypass the queue by setting textures directly. */
                if (pvr.CameraEndFrame) {
                    pvr.CameraEndFrame(0, tex);
                    pvr.CameraEndFrame(1, g_layers[i].is_multiview ? tex : g_layers[i].textures[1]);
                }
                /* Also set textures directly in the PVR structure via GOT */
                {
                    char* evbuf = (char*)dlsym(pvr.handle, "eventBuffer");
                    if (evbuf) {
                        char** got_entry = (char**)(evbuf - 0x15F00);
                        char* pvr_struct = *got_entry;
                        if (pvr_struct) {
                            pvr_struct[104] = 1;
                            *(int*)(pvr_struct + 76) = tex;
                            *(int*)(pvr_struct + 80) = g_layers[i].is_multiview ? tex : g_layers[i].textures[1];
                            *(int*)(pvr_struct + 0xdabc) = 0;
                            *(int*)(pvr_struct + 0xda98) = 0;
                            *(int*)(pvr_struct + 0x5364) = 0;
                            *(unsigned char*)(pvr_struct + 0xd810) = 1;
                            *(int*)(pvr_struct + 0xf960) = tex;
                            *(int*)(pvr_struct + 0xf964) = 0;
                            *(int*)(pvr_struct + 0xf928) = 0;
                            *(int*)(pvr_struct + 0xf92c) = 0;
                            if (!g_layers[i].is_multiview) {
                                *(int*)(pvr_struct + 0xf918) = g_layers[i].textures[1];
                                *(int*)(pvr_struct + 0xf91c) = 0;
                            }
                        }
                    }
                }
                /* Submit frame via UnityRenderEvent_ */
                if (pvr.RenderEvent) {
                    pvr.RenderEvent(RENDER_EVENT_BOTH_EYE_END_FRAME);
                    /* Clear overlay flag between BOTH_EYE and TIMEWARP */
                    {
                        char* evbuf = (char*)dlsym(pvr.handle, "eventBuffer");
                        if (evbuf) {
                            char** got_entry = (char**)(evbuf - 0x15F00);
                            char* s = *got_entry;
                            if (s) {
                                *(int*)(s + 0xdabc) = 0;
                                *(int*)(s + 0xda98) = 0;
                                *(unsigned char*)(s + 0xd810) = 1;
                            }
                        }
                    }
                    pvr.RenderEvent(RENDER_EVENT_TIMEWARP);
                }
                /* Bind the eye texture as the current framebuffer so Unity
                   renders to it directly. The game's XR display subsystem
                   never starts, so Unity would otherwise render to the
                   screen. By binding the eye texture FBO here (after frame
                   submission, before returning to Unity), Unity's cameras
                   will render to the eye texture for this frame. */
                {
                    static GLuint eye_fbo = 0;
                    if (!eye_fbo) glGenFramebuffers(1, &eye_fbo);
                    glBindFramebuffer(GL_FRAMEBUFFER, eye_fbo);
                    if (g_layers[i].is_multiview) {
                        if (g_glFramebufferTextureMultiview) {
                            g_glFramebufferTextureMultiview(GL_FRAMEBUFFER,
                                GL_COLOR_ATTACHMENT0, tex, 0, 0, 2);
                        }
                    } else {
                        glFramebufferTextureLayer(GL_FRAMEBUFFER,
                            GL_COLOR_ATTACHMENT0, tex, 0, 0);
                    }
                    glViewport(0, 0, g_layers[i].width, g_layers[i].height);
                }
                if (poll_count == 0) FLOG("Pxr_PollEvent: first frame submitted");
                poll_count++;
                break;
            }
        }
    }
    return 0;
}

/* ---- Missing functions needed by libPxrPlatform.so ---- */

JNIEXPORT int Pxr_Construct() { LOGI("Pxr_Construct -> 0"); return 0; }
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

JNIEXPORT void* GetRenderEventFunc() {
    /* Return PVR's UnityRenderEvent so Unity can call it for TimeWarp */
    if (pvr.RenderEvent) return (void*)pvr.RenderEvent;
    return NULL;
}

JNIEXPORT void OnRenderEvent(int event) {
    if (pvr.RenderEvent) pvr.RenderEvent(event);
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
