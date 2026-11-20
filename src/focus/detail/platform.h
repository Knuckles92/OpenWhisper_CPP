#pragma once

#include <memory>
#include <string>

#include "focus/focus_capture.h"

namespace fv::focus::detail {

// Names the calling thread for debuggers and crash dumps.
void set_thread_name(const std::string& name);

// This computer's platform for the capture service, or nullptr where none is ported.
std::shared_ptr<FocusPlatform> make_native_platform();
// Whether make_native_platform's platform reads text near the cursor.
bool native_text_supported();

} // namespace fv::focus::detail
