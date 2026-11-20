#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "base/clock.h"
#include "base/json.h"

// services/format_utils.py: the display formats History, Quick Record and the downloads use.
// Python's helpers take whatever they are given (None, numbers, numeric text) and fall back
// instead of raising; the Json overloads keep that for callers holding loosely typed values.
// Affinity: thread-safe (pure functions).
namespace fv {

// Shown in place of a date that is missing or unreadable. The web dashboard repeats it
// (webui HistoryPane.tsx).
inline constexpr std::string_view UNKNOWN_DATE = "Unknown date";

// "Aug 20, 2026 04:46 PM" (strftime "%b %d, %Y %I:%M %p", English names as Python's C locale
// gives on Windows). An aware value is converted to the clock's zone; a naive one is local wall
// time already and is not shifted. Text fromisoformat refuses, or a moment outside years
// 1..9999 once converted, comes back unchanged.
std::string format_timestamp(std::string_view iso_timestamp, const Clock& clock = system_clock());
// None stays None, as Python's TypeError path returns its argument.
std::optional<std::string> format_timestamp(const std::optional<std::string>& iso_timestamp,
                                            const Clock& clock = system_clock());
// Text arguments would otherwise be ambiguous between the two overloads above.
inline std::string format_timestamp(const char* iso_timestamp,
                                    const Clock& clock = system_clock()) {
    return format_timestamp(std::string_view(iso_timestamp), clock);
}
inline std::string format_timestamp(const std::string& iso_timestamp,
                                    const Clock& clock = system_clock()) {
    return format_timestamp(std::string_view(iso_timestamp), clock);
}

// "3.7s" below a minute (one decimal, the binary value rounded half to even), else "2m 5s",
// "1h 3m" or "1h" from the seconds rounded half to even. Negative counts as 0.
std::string format_audio_duration(double seconds);
std::string format_audio_duration(std::optional<double> seconds); // None -> "0.0s"
// float(value) first: numbers, bools and numeric text; anything else -> "0.0s". Throws
// py::ValueError for NaN and py::OverflowError for an infinity at or above a minute, as int()
// does in Python.
std::string format_audio_duration(const Json& seconds);

// "31 ms" below a second, else format_audio_duration. Negative, NaN and unreadable values
// count as 0.
std::string format_short_duration(double seconds);
std::string format_short_duration(const Json& seconds);

// Advertised and download sizes in decimal units: "1.23 GB", "15 MB", "2 KB", "999 B".
std::string format_size_bytes(std::int64_t size_bytes);

// "44.1 kHz", "16 kHz"; "" for zero, negative or unreadable values.
std::string format_sample_rate(std::int64_t hertz);
// int(value) first: integers, bools, floats (truncated) and integer text.
std::string format_sample_rate(const Json& hertz);

// Local file sizes in binary units: "512 B", "1.5 KB", "2.0 MB", "5.0 GB".
std::string format_file_size(double size_bytes);

} // namespace fv
