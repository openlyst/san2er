#include "hooks.h"
#include "loader.h"
#include "log.h"

namespace pvr_shim {

namespace {

typedef int (*Pvr_Init_t)(int);
typedef int (*Pvr_ChangeScreenParameters_t)(const char*, int, int, double, double, double);
typedef void (*Pvr_SetInitActivity_t)(void*, void*);
typedef void (*Pvr_SetCurrentHMDType_t)(const char*);
typedef void* (*Pvr_GetSupportHMDTypes_t)();

Pvr_Init_t orig_Pvr_Init = nullptr;
Pvr_ChangeScreenParameters_t orig_Pvr_ChangeScreenParameters = nullptr;
Pvr_SetInitActivity_t orig_Pvr_SetInitActivity = nullptr;
Pvr_SetCurrentHMDType_t orig_Pvr_SetCurrentHMDType = nullptr;
Pvr_GetSupportHMDTypes_t orig_Pvr_GetSupportHMDTypes = nullptr;

// Placeholders until we can read the real Neo 2 parameters from the device.
// Pico Neo 2 has a single 5.5" 2560x1440 LCD at ~75 Hz.
const int NEO2_WIDTH = 2560;
const int NEO2_HEIGHT = 1440;
const double NEO2_XPPI = 600.0;
const double NEO2_YPPI = 600.0;
const double NEO2_DPI = 600.0;

} // namespace

void resolve_all_hooks() {
    orig_Pvr_Init = (Pvr_Init_t)resolve_original("Pvr_Init");
    orig_Pvr_ChangeScreenParameters = (Pvr_ChangeScreenParameters_t)resolve_original("Pvr_ChangeScreenParameters");
    orig_Pvr_SetInitActivity = (Pvr_SetInitActivity_t)resolve_original("Pvr_SetInitActivity");
    orig_Pvr_SetCurrentHMDType = (Pvr_SetCurrentHMDType_t)resolve_original("Pvr_SetCurrentHMDType");
    orig_Pvr_GetSupportHMDTypes = (Pvr_GetSupportHMDTypes_t)resolve_original("Pvr_GetSupportHMDTypes");
}

} // namespace pvr_shim

extern "C" int Pvr_Init(int index) {
    LOGI("Pvr_Init(%d)", index);
    int ret = 0;
    if (pvr_shim::orig_Pvr_Init) {
        ret = pvr_shim::orig_Pvr_Init(index);
    }
    LOGI("Pvr_Init -> %d", ret);
    return ret;
}

extern "C" int Pvr_ChangeScreenParameters(const char* model, int width, int height,
                                          double xppi, double yppi, double densityDpi) {
    LOGI("Pvr_ChangeScreenParameters model=%s w=%d h=%d xppi=%f yppi=%f dpi=%f",
         model ? model : "(null)", width, height, xppi, yppi, densityDpi);

    if (pvr_shim::orig_Pvr_ChangeScreenParameters) {
        return pvr_shim::orig_Pvr_ChangeScreenParameters(model, width, height, xppi, yppi, densityDpi);
    }
    return 0;
}

extern "C" void Pvr_SetInitActivity(void* activity, void* vrActivityClass) {
    LOGI("Pvr_SetInitActivity(%p, %p)", activity, vrActivityClass);
    if (pvr_shim::orig_Pvr_SetInitActivity) {
        pvr_shim::orig_Pvr_SetInitActivity(activity, vrActivityClass);
    }
}

extern "C" void Pvr_SetCurrentHMDType(const char* type) {
    LOGI("Pvr_SetCurrentHMDType(%s)", type ? type : "(null)");
    if (pvr_shim::orig_Pvr_SetCurrentHMDType) {
        pvr_shim::orig_Pvr_SetCurrentHMDType(type);
    }
}

extern "C" void* Pvr_GetSupportHMDTypes() {
    LOGI("Pvr_GetSupportHMDTypes");
    if (pvr_shim::orig_Pvr_GetSupportHMDTypes) {
        return pvr_shim::orig_Pvr_GetSupportHMDTypes();
    }
    return nullptr;
}
