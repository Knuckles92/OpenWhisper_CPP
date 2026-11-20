#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// The ids of transcript segments (meeting/asr/engine.py _stable_segment_id and
// meeting/asr/offline.py offline_segment_id). They are evidence anchors stored in the database,
// so the same meeting, chunk or window time and ordinal must give OpenWhisper's id. The ordinal
// counts every segment the decoder returned, the dropped empty ones included.
namespace fv::meeting {

// "sg_" + the first 20 hex digits of sha256("{meeting_id}:{chunk_id}:{ordinal}").
std::string stable_segment_id(std::string_view meeting_id, std::int64_t chunk_id,
                              std::int64_t ordinal);

// "sg_" + the first 20 hex digits of
// sha256("{meeting_id}:offline:{channel}:{int(round(start_s * 1000))}:{ordinal}"), rounding half
// to even. Throws py::ValueError for a NaN start and py::OverflowError for an infinite one, as
// Python's round() does.
std::string offline_segment_id(std::string_view meeting_id, std::string_view channel,
                               double start_s, std::int64_t ordinal);

} // namespace fv::meeting
