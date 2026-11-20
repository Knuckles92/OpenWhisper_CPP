#include "process/detail/launch_internal.h"

#include <objbase.h>

#include "process/win/wide.h"

namespace fv::process_detail {

ComScope::ComScope() {
    // S_OK or S_FALSE; a thread already in another apartment keeps it.
    initialized_ =
        SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
}

ComScope::~ComScope() {
    if (initialized_) {
        CoUninitialize();
    }
}

} // namespace fv::process_detail
