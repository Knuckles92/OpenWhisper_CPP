#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "base/json.h"

// meeting/web/api.py's keyset cursors: opaque to the dashboard, base64url (no padding) of a
// compact JSON pair. The transcript cursor is [start_s, segment id], the History cursor
// [started_at, meeting id]. The decoders turn every malformed cursor into a CursorError, the
// route's 400.
// Affinity: thread-safe (functions).
namespace fv::meeting_web {

// Longest History cursor worth decoding, in characters; real ones are far shorter.
inline constexpr std::size_t kHistoryCursorMaxChars = 1024;

inline constexpr std::string_view kInvalidTranscriptCursor = "invalid transcript cursor";
inline constexpr std::string_view kInvalidMeetingCursor = "invalid meeting cursor";

// A cursor that cannot be decoded; what() is the 400 response's detail.
class CursorError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct TranscriptCursor {
    double start_s = 0.0;
    std::string segment_id;
};

struct HistoryCursor {
    std::string started_at;
    std::string meeting_id;
};

// _encode_cursor(segment row): [float(start_s, default 0.0), str(id, default "")] with
// separators (",", ":"), ASCII-escaped, floats as repr. Throws py::TypeError or py::ValueError
// where Python's float() does.
std::string encode_cursor(const Json& segment);

// _decode_cursor: nullopt for an empty cursor. Python unpacks whatever the JSON holds into two
// values (a two-item list, a two-character string, a two-key object), then float() and str().
std::optional<TranscriptCursor> decode_cursor(std::string_view cursor);

// _history_cursor(meeting row): [started_at, id] as stored. Throws py::PyError("KeyError") for
// a row without either key.
std::string history_cursor(const Json& meeting);

// _decode_history_cursor: nullopt for an empty cursor; a list of exactly two strings, from at
// most kHistoryCursorMaxChars characters.
std::optional<HistoryCursor> decode_history_cursor(std::string_view cursor);

} // namespace fv::meeting_web
