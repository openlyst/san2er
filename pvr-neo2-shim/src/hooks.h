#pragma once

namespace pvr_shim {

// Call once after the loader has resolved the original function pointers.
// These pointers are stored by the generated forward_vars.cpp file.
void resolve_all_hooks();

} // namespace pvr_shim
