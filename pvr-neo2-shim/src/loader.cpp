#include "loader.h"
#include "hooks.h"
#include "log.h"
#include <pthread.h>

namespace pvr_shim {

namespace {

pthread_once_t init_once = PTHREAD_ONCE_INIT;
void* original_handle = nullptr;

void init_original() {
    original_handle = dlopen("libPvr_UnitySDK_orig.so", RTLD_NOW | RTLD_LOCAL);
    if (!original_handle) {
        LOGE("failed to load libPvr_UnitySDK_orig.so: %s", dlerror());
    } else {
        LOGI("loaded original libPvr_UnitySDK_orig.so");
    }
}

void ensure_loaded() {
    pthread_once(&init_once, init_original);
}

} // namespace

bool load_original_library() {
    ensure_loaded();
    return original_handle != nullptr;
}

void* resolve_original(const char* name) {
    ensure_loaded();
    if (!original_handle) {
        return nullptr;
    }
    void* sym = dlsym(original_handle, name);
    if (!sym) {
        LOGE("failed to resolve %s: %s", name, dlerror());
    }
    return sym;
}

// Defined in generated/forward_vars.cpp.
void pvr_shim_resolve_forward_symbols();

} // namespace pvr_shim

extern "C" __attribute__((constructor)) void pvr_shim_ctor() {
    if (pvr_shim::load_original_library()) {
        pvr_shim::resolve_all_hooks();
        pvr_shim::pvr_shim_resolve_forward_symbols();
    }
}
