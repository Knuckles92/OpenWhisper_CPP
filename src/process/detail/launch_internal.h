#pragma once

#include <memory>
#include <string_view>

#include "process/launch.h"

namespace fv::process_detail {

// COM in a single-threaded apartment on this thread for the object's lifetime (launch.py _com),
// uninitialised after only when this initialised it. Nothing outside Windows.
class ComScope {
public:
    ComScope();
    ~ComScope();
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool initialized_ = false;
};

// The real backend of this platform.
std::shared_ptr<LaunchBackend> platform_launch_backend();

// launch.py _ask_explorer: backend.explorer_open on a thread of its own with COM, waiting the
// backend's explorer_wait_s at most. Explorer's desktop takes one call at a time and no more calls
// queue up behind one it never answers: a call that can't get its turn in that time, or isn't
// answered in it, throws NoAnswer. What the backend throws reaches the caller.
bool ask_explorer(std::string_view file, std::string_view args);

} // namespace fv::process_detail
