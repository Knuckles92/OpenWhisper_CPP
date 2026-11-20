#pragma once

#include <string>
#include <string_view>

#include "base/json.h"

// What the custom-report files share: dict.get over loosely typed rows and snapshots, and the
// worker thread's name.
namespace fv::meeting_intel::detail {

// d.get(key): the member, or null when `d` is not an object or lacks it.
const Json& member(const Json& d, std::string_view key);
// participants.get(key) for a dict keyed by participant id: null for a key no participant has.
// Throws py::TypeError for a list or dict key (unhashable, as Python's lookup does).
const Json& participant(const Json& participants, const Json& key);
// float(value or 0.0): py::ValueError for text that is not a number, py::TypeError for a
// container, as Python's float() raises.
double float_or_zero(const Json& value);

// Names the calling thread for debuggers (Linux keeps 15 bytes).
void set_current_thread_name(std::string_view name);

} // namespace fv::meeting_intel::detail
