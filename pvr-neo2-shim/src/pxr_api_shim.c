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
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#define TAG "pxr_api_shim"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

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
typedef void (*Pvr_SetupLayerData_t)(int, int, int, int, int, float*);
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
static int g_xr_running = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

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
};

static struct layer_info g_layers[MAX_LAYERS];

/* FOV cache */
static float g_fov_left = 50.0f;
static float g_fov_right = 50.0f;
static float g_fov_up = 50.0f;
static float g_fov_down = 50.0f;
static int g_fov_cached = 0;

/* Render event IDs matching PVR SDK enum */
#define RENDER_EVENT_INIT_RENDER_THREAD  1024
#define RENDER_EVENT_PAUSE               1025
#define RENDER_EVENT_RESUME              1026
#define RENDER_EVENT_LEFT_EYE_END_FRAME  1027
#define RENDER_EVENT_RIGHT_EYE_END_FRAME 1028
#define RENDER_EVENT_TIMEWARP            1029
#define RENDER_EVENT_SHUTDOWN            1030
#define RENDER_EVENT_BOTH_EYE_END_FRAME  1033

/* ---- PVR config enum values (from Pvr_UnitySDKAPI.cs) ---- */
enum pvr_config {
    PVR_CFG_FOV              = 0,
    PVR_CFG_HFOV             = 1,
    PVR_CFG_EYE_BUFFER_W     = 2,
    PVR_CFG_EYE_BUFFER_H     = 3,
    PVR_CFG_TARGET_FPS       = 4,
    PVR_CFG_IPD              = 5,
    PVR_CFG_NECK_X           = 6,
    PVR_CFG_NECK_Y           = 7,
    PVR_CFG_NECK_Z           = 8,
    PVR_CFG_PLATFORM_TYPE    = 9,
    PVR_CFG_DISPLAY_RATE     = 10,
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

    pvr.handle = dlopen("libPvr_UnitySDK.so", RTLD_NOW | RTLD_LOCAL);
    if (!pvr.handle) {
        LOGE("failed to load libPvr_UnitySDK.so: %s", dlerror());
        return -1;
    }
    LOGI("loaded system libPvr_UnitySDK.so");

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

    /* Cache FOV */
    if (pvr.GetFOV) {
        float vfov, hfov;
        pvr.GetFOV(&vfov, &hfov);
        g_fov_up = g_fov_down = vfov * 0.5f;
        g_fov_left = g_fov_right = hfov * 0.5f;
        g_fov_cached = 1;
        LOGI("FOV: v=%f h=%f (L=%f R=%f U=%f D=%f)", vfov, hfov,
             g_fov_left, g_fov_right, g_fov_up, g_fov_down);
    }

    /* Init render thread */
    if (pvr.RenderEvent) {
        pvr.RenderEvent(RENDER_EVENT_INIT_RENDER_THREAD);
        LOGI("render thread initialized");
    }

    g_initialized = 1;
    pthread_mutex_unlock(&g_lock);
}

/* ---- JNI_OnLoad ---- */

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_jvm = vm;
    LOGI("JNI_OnLoad pxr_api_shim");
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {
    LOGI("JNI_OnUnload pxr_api_shim");
}

/* ---- Helper: get JNIEnv ---- */

static JNIEnv* get_env() {
    if (!g_jvm) return NULL;
    JNIEnv* env = NULL;
    if ((*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6) == JNI_OK)
        return env;
    (*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL);
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
    JNIEnv* env = get_env();
    if (env) {
        init_pvr_runtime(env);
    } else {
        LOGE("no JNIEnv for Pxr_Initialize");
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
    return g_initialized;
}

JNIEXPORT int Pxr_IsRunning() {
    return g_xr_running;
}

JNIEXPORT int Pxr_BeginXr() {
    LOGI("Pxr_BeginXr");
    if (pvr.RenderEvent) pvr.RenderEvent(RENDER_EVENT_RESUME);
    g_xr_running = 1;
    return 0;
}

JNIEXPORT int Pxr_EndXr() {
    LOGI("Pxr_EndXr");
    if (pvr.RenderEvent) pvr.RenderEvent(RENDER_EVENT_PAUSE);
    g_xr_running = 0;
    return 0;
}

JNIEXPORT int Pxr_SetInitializeData(void* data) {
    LOGI("Pxr_SetInitializeData(%p)", data);
    return 0;
}

JNIEXPORT int Pxr_SetPlatformOption(int type, int value) {
    LOGI("Pxr_SetPlatformOption(%d, %d)", type, value);
    return 0;
}

JNIEXPORT int Pxr_SetGraphicOption(int option) {
    LOGI("Pxr_SetGraphicOption(%d)", option);
    return 0;
}

JNIEXPORT int Pxr_SetTrackingMode(int mode) {
    LOGI("Pxr_SetTrackingMode(%d)", mode);
    return 0;
}

JNIEXPORT int Pxr_GetTrackingMode(int* mode) {
    if (mode) *mode = 0;
    return 0;
}

JNIEXPORT int Pxr_SetTrackingOrigin(int origin) {
    LOGI("Pxr_SetTrackingOrigin(%d)", origin);
    return 0;
}

JNIEXPORT int Pxr_GetTrackingOrigin(int* origin) {
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

/* ---- Layer / Swapchain ---- */

JNIEXPORT int Pxr_CreateLayer(void* layerParamPtr) {
    struct PxrLayerParam* p = (struct PxrLayerParam*)layerParamPtr;
    int width = p ? (int)p->width : 1440;
    int height = p ? (int)p->height : 1584;

    LOGI("Pxr_CreateLayer w=%d h=%d format=%llu", width, height, p ? p->format : 0);

    int slot = -1;
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (!g_layers[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        LOGE("no free layer slots");
        return -1;
    }

    g_layers[slot].in_use = 1;
    g_layers[slot].width = width;
    g_layers[slot].height = height;
    g_layers[slot].format = p ? (int)p->format : GL_RGBA8;
    g_layers[slot].current_index = 0;

    /* Create swapchain textures */
    glGenTextures(SWAPCHAIN_LEN, g_layers[slot].textures);
    for (int i = 0; i < SWAPCHAIN_LEN; i++) {
        glBindTexture(GL_TEXTURE_2D, g_layers[slot].textures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    LOGI("Pxr_CreateLayer -> slot %d, textures [%u, %u]",
         slot, g_layers[slot].textures[0], g_layers[slot].textures[1]);

    if (p) p->layerId = slot;
    return slot;
}

JNIEXPORT int Pxr_DestroyLayer(int layerId) {
    LOGI("Pxr_DestroyLayer(%d)", layerId);
    if (layerId < 0 || layerId >= MAX_LAYERS || !g_layers[layerId].in_use)
        return -1;
    glDeleteTextures(SWAPCHAIN_LEN, g_layers[layerId].textures);
    memset(&g_layers[layerId], 0, sizeof(g_layers[layerId]));
    return 0;
}

JNIEXPORT int Pxr_GetLayerImageCount(int layerId, int eye, unsigned int* count) {
    if (count) *count = SWAPCHAIN_LEN;
    return 0;
}

JNIEXPORT int Pxr_GetLayerNextImageIndex(int layerId, int* imageIndex) {
    if (layerId < 0 || layerId >= MAX_LAYERS || !g_layers[layerId].in_use)
        return -1;
    int idx = g_layers[layerId].current_index;
    g_layers[layerId].current_index = (idx + 1) % SWAPCHAIN_LEN;
    if (imageIndex) *imageIndex = idx;
    return 0;
}

JNIEXPORT int Pxr_GetLayerImage(int layerId, int eye, int imageIndex, unsigned long long* image) {
    if (layerId < 0 || layerId >= MAX_LAYERS || !g_layers[layerId].in_use)
        return -1;
    if (imageIndex < 0 || imageIndex >= SWAPCHAIN_LEN)
        return -1;
    if (image) *image = (unsigned long long)g_layers[layerId].textures[imageIndex];
    return 0;
}

JNIEXPORT int Pxr_GetLayerFoveationImage(int layerId, int eye, int imageIndex, unsigned long long* image) {
    return Pxr_GetLayerImage(layerId, eye, imageIndex, image);
}

JNIEXPORT void Pxr_SetCreateLayerParam(void* param) {
    /* called by PxrPlatform before Pxr_CreateLayer, we handle it in CreateLayer */
}

JNIEXPORT void Pxr_DestroyLayerByRender(int layerId) {
    Pxr_DestroyLayer(layerId);
}

/* ---- Frame ---- */

JNIEXPORT int Pxr_WaitFrame() {
    return 0;
}

JNIEXPORT int Pxr_BeginFrame() {
    return 0;
}

JNIEXPORT int Pxr_EndFrame() {
    /* Trigger TimeWarp via PVR render event */
    if (pvr.RenderEvent) {
        pvr.RenderEvent(RENDER_EVENT_BOTH_EYE_END_FRAME);
    }
    return 0;
}

JNIEXPORT int Pxr_CanBeginVR() {
    return 1;
}

/* ---- Layer submission ---- */

JNIEXPORT int Pxr_SubmitLayer(void* layerPtr) {
    struct PxrLayerHeader* h = (struct PxrLayerHeader*)layerPtr;
    if (!h || h->layerId < 0 || h->layerId >= MAX_LAYERS || !g_layers[h->layerId].in_use)
        return -1;

    int texId = (int)g_layers[h->layerId].textures[h->imageIndex % SWAPCHAIN_LEN];

    /* Submit to PVR TimeWarp: layer 0, both eyes, texture, type=0, flags=0 */
    if (pvr.SetupLayerData) {
        float colorScaleOffset[8] = {1,1,1,1, 0,0,0,0};
        pvr.SetupLayerData(0, 3, texId, 0, 0, colorScaleOffset);
        pvr.SetupLayerData(0, 1, texId, 0, 0, colorScaleOffset);
    }
    return 0;
}

JNIEXPORT int Pxr_SubmitLayer2(void* layerPtr) {
    return Pxr_SubmitLayer(layerPtr);
}

JNIEXPORT int Pxr_SubmitLayerProjection(void* layer) {
    return 0;
}
JNIEXPORT int Pxr_SubmitLayerProjection2(void* layer) {
    return 0;
}
JNIEXPORT int Pxr_SubmitLayerQuad(void* layer) { return 0; }
JNIEXPORT int Pxr_SubmitLayerQuad2(void* layer) { return 0; }
JNIEXPORT int Pxr_SubmitLayerCylinder(void* layer) { return 0; }
JNIEXPORT int Pxr_SubmitLayerCylinder2(void* layer) { return 0; }
JNIEXPORT int Pxr_SubmitLayerEquirect(void* layer) { return 0; }
JNIEXPORT int Pxr_SubmitLayerEquirect2(void* layer) { return 0; }
JNIEXPORT int Pxr_SubmitLayerCube2(void* layer) { return 0; }

/* ---- Sensor / Pose ---- */

JNIEXPORT int Pxr_GetPredictedMainSensorState2(double predictTimeMs,
        struct PxrSensorState2* sensorState, int* sensorFrameIndex) {
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
    return Pxr_GetPredictedMainSensorState(t, (struct PxrSensorState*)s);
}

JNIEXPORT int Pxr_GetHeadSensorData(double t, void* s) {
    return Pxr_GetPredictedMainSensorState(t, (struct PxrSensorState*)s);
}

JNIEXPORT int Pxr_GetPredictedDisplayTime(double* t) {
    if (t) *t = 0.0;
    return 0;
}

/* ---- FOV / Frustum ---- */

JNIEXPORT int Pxr_GetFov(int eye, float* fovLeft, float* fovRight,
                         float* fovUp, float* fovDown) {
    if (!g_fov_cached && pvr.GetFOV) {
        float vfov, hfov;
        pvr.GetFOV(&vfov, &hfov);
        g_fov_up = g_fov_down = vfov * 0.5f;
        g_fov_left = g_fov_right = hfov * 0.5f;
        g_fov_cached = 1;
    }
    if (fovLeft) *fovLeft = g_fov_left;
    if (fovRight) *fovRight = g_fov_right;
    if (fovUp) *fovUp = g_fov_up;
    if (fovDown) *fovDown = g_fov_down;
    return 0;
}

JNIEXPORT int Pxr_SetFrustum(int eye, float fl, float fr, float fu,
                             float fd, float n, float f) {
    return 0;
}

JNIEXPORT int Pxr_GetFrustum(int eye, float* fl, float* fr, float* fu,
                             float* fd, float* n, float* f) {
    Pxr_GetFov(eye, fl, fr, fu, fd);
    if (n) *n = 0.1f;
    if (f) *f = 100.0f;
    return 0;
}

/* ---- Config ---- */

JNIEXPORT int Pxr_GetConfigInt(int configIndex, int* value) {
    if (!value) return -1;
    switch (configIndex) {
    case PXR_CFG_RENDER_TEX_W:
        if (pvr.GetIntConfig)
            return pvr.GetIntConfig(PVR_CFG_EYE_BUFFER_W, value);
        *value = 1440;
        return 0;
    case PXR_CFG_RENDER_TEX_H:
        if (pvr.GetIntConfig)
            return pvr.GetIntConfig(PVR_CFG_EYE_BUFFER_H, value);
        *value = 1584;
        return 0;
    case PXR_CFG_TARGET_FRAME_RATE:
        *value = 72;
        return 0;
    case PXR_CFG_SYSTEM_DISPLAY_RATE:
        *value = 72;
        return 0;
    default:
        *value = 0;
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
    return 0;
}

JNIEXPORT int Pxr_SetConfigString(int configIndex, const char* value) {
    return 0;
}

JNIEXPORT int Pxr_SetConfigUint64(int configIndex, unsigned long long value) {
    return 0;
}

JNIEXPORT int Pxr_SetConfigIntArray(int configIndex, int* data, int count) {
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

JNIEXPORT int Pxr_GetAppHasFocus() { return 1; }
JNIEXPORT int Pxr_GetTrackingState() { return 1; }
JNIEXPORT int Pxr_PollEvent(void* event) { return 0; }

/* ---- Multiview ---- */

JNIEXPORT int Pxr_EnableMultiview(int enable) {
    LOGI("Pxr_EnableMultiview(%d)", enable);
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
    if (pvr.GetPsensorState) return pvr.GetPsensorState();
    return 0;
}

/* ---- Controller stubs ---- */

JNIEXPORT int Pxr_GetControllerConnectStatus(unsigned int id, int* status) {
    if (status) *status = 1;
    return 0;
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
JNIEXPORT void JNICALL Java_com_pxr_xrlib_PicovrSDK_setSwapchainEXT(JNIEnv* env, jobject thiz, jint e) {}
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
