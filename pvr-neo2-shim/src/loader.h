#pragma once

#include <dlfcn.h>

namespace pvr_shim {

bool load_original_library();
void* resolve_original(const char* name);

} // namespace pvr_shim
