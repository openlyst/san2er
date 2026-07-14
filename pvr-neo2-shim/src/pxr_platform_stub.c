#include <jni.h>
#include <android/log.h>
#include <stdint.h>
#include <string.h>

#define TAG "pxr_platform_stub"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static char fake_instance[256] __attribute__((aligned(8)));

jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    LOGI("JNI_OnLoad (pxr_platform stub)");
    return JNI_VERSION_1_6;
}

void JNI_OnUnload(JavaVM *vm, void *reserved) {
    LOGI("JNI_OnUnload (pxr_platform stub)");
}

void *Pxr_Construct(void *settings) {
    LOGI("Pxr_Construct -> fake instance %p", fake_instance);
    return fake_instance;
}

int Pxr_SetUserDefinedSettings(void *settings) {
    LOGI("Pxr_SetUserDefinedSettings");
    return 1;
}

int Pxr_LoadPlugin(const char *name) {
    LOGI("Pxr_LoadPlugin");
    return 1;
}

int Pxr_UnloadPlugin() {
    LOGI("Pxr_UnloadPlugin");
    return 1;
}

void Pxr_SetSRPState(int state) {
    LOGI("Pxr_SetSRPState %d", state);
}

int Pxr_GetFocusState() {
    LOGI("Pxr_GetFocusState");
    return 1;
}

int Pxr_GetSeeThroughState(int *state) {
    LOGI("Pxr_GetSeeThroughState %p", state);
    return 0;
}

int Pxr_GetSensorStatus() {
    LOGI("Pxr_GetSensorStatus");
    return 1;
}

int Pxr_IsSensorReady() {
    LOGI("Pxr_IsSensorReady");
    return 1;
}

int Pxr_GetHomeKey(int *key) {
    LOGI("Pxr_GetHomeKey %p", key);
    return 0;
}

int Pxr_InitHomeKey(int mode) {
    LOGI("Pxr_InitHomeKey %d", mode);
    return 1;
}

void Pxr_SetFoveationLevelEnable(int level) {
    LOGI("Pxr_SetFoveationLevelEnable %d", level);
}

void Pxr_SetLogInfoActive(int level, int enable) {
    LOGI("Pxr_SetLogInfoActive %d %d", level, enable);
}

int Pxr_EnableEyeTracking(int enable) {
    LOGI("Pxr_EnableEyeTracking %d", enable);
    return 1;
}

int Pxr_EnableFaceTracking(int enable) {
    LOGI("Pxr_EnableFaceTracking %d", enable);
    return 1;
}

void Pxr_SetVideoSeethroughState(int state) {
    LOGI("Pxr_SetVideoSeethroughState %d", state);
}

void Pxr_RefreshRateChanged() {
    LOGI("Pxr_RefreshRateChanged");
}

void Pxr_SetControllerOriginOffset(int hand, float x, float y, float z) {
    LOGI("Pxr_SetControllerOriginOffset %d %f %f %f", hand, x, y, z);
}

void Pxr_SetInputDeviceChangedCallBack(void *cb) {
    LOGI("Pxr_SetInputDeviceChangedCallBack");
}

void *Pxr_GetLayerImagePtr(void *layer, int eye) {
    LOGI("Pxr_GetLayerImagePtr");
    return NULL;
}

int Pxr_CreateLayerParam(void *a, void *b, void *c, void *d, void *e) {
    LOGI("Pxr_CreateLayerParam");
    return 0;
}

void Pxr_DestroyLayerByRender(void *layer) {
    LOGI("Pxr_DestroyLayerByRender");
}

int Pxr_GetMRCEnable() {
    LOGI("Pxr_GetMRCEnable");
    return 0;
}
