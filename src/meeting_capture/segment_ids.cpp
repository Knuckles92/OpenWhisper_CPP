#include "meeting_capture/segment_ids.h"

#include <cmath>
#include <format>

#include "base/hash.h"
#include "pyport/errors.h"
#include "pyport/number.h"

namespace fv::meeting {

namespace {

std::string anchor(std::string_view raw) {
    return "sg_" + sha256_hex(raw).substr(0, 20);
}

} // namespace

std::string stable_segment_id(std::string_view meeting_id, std::int64_t chunk_id,
                              std::int64_t ordinal) {
    return anchor(std::format("{}:{}:{}", meeting_id, chunk_id, ordinal));
}

std::string offline_segment_id(std::string_view meeting_id, std::string_view channel,
                               double start_s, std::int64_t ordinal) {
    const double millis = start_s * 1000;
    if (std::isnan(millis)) {
        throw py::ValueError("cannot convert float NaN to integer");
    }
    if (std::isinf(millis)) {
        throw py::OverflowError("cannot convert float infinity to integer");
    }
    // The rounded value is a whole number, which "{:.0f}" prints exactly (Python's int has no
    // size limit); adding 0.0 turns -0.0 into 0, as int() does.
    const double rounded = py::round_half_even(millis) + 0.0;
    return anchor(std::format("{}:offline:{}:{:.0f}:{}", meeting_id, channel, rounded, ordinal));
}

} // namespace fv::meeting
