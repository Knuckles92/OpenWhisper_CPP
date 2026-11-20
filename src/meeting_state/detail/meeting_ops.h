#pragma once

#include <functional>
#include <string>

#include "meeting_state/patches.h"

// How patches.cpp's registry takes the review, custom-report and fast ops while it is built,
// without register_op's lock (register_all.cpp lists them).
namespace fv::meeting_state::detail {

using OpAdder =
    std::function<void(std::string name, Handler handler, OpDomains domains, OpFlags flags)>;

void add_meeting_ops(const OpAdder& add);

} // namespace fv::meeting_state::detail
