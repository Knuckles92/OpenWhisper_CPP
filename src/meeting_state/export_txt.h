#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/clock.h"
#include "base/json.h"

// meeting/export/transcript_txt.py: the plain timestamped transcript, and the formatting the
// other exporters share. Every exporter is a pure function of three JSON inputs:
//   meeting   a meeting row (MeetingRepository::get_meeting)
//   state     a MeetingState::to_json() document
//   segments  segment rows (get_segments), ordered by start_s
// read as Python reads dicts, so hand-made documents with missing keys work as they do there.
// Dates are local to the clock's zone (aware stamps converted, legacy naive ones kept).
// Affinity: thread-safe (pure functions).
namespace fv::meeting_state {

// Speaker labels when a segment has no resolvable participant.
inline constexpr std::string_view FALLBACK_SPEAKER_MIC = "Me";
inline constexpr std::string_view FALLBACK_SPEAKER_LOOPBACK = "Others";

// A meeting-clock offset as zero-padded "hh:mm:ss" (the hours widen past two digits) and
// "mm:ss" (the minutes pass 59); negative offsets read as 0. Throws py::ValueError for NaN and
// py::OverflowError for an infinity, as int() does.
std::string format_clock(double seconds);
std::string format_mmss(double seconds);

// The row's title, the document's title, "Meeting <YYYY-MM-DD>" from started_at, or "Meeting".
std::string resolve_title(const Json& meeting, const Json& state,
                          const Clock& clock = system_clock());
// "YYYY-MM-DD HH:MM" of started_at, the raw value when it does not parse, nullopt without one.
std::optional<std::string> format_meeting_date(const Json& meeting,
                                               const Clock& clock = system_clock());
// Non-empty display names, the host ("me") first, everyone else in document order.
std::vector<std::string> participant_names(const Json& state);
// The speaker's display name, else "Me" on the mic channel and "Others" on loopback.
std::string speaker_name(const Json& segment, const Json& participants);
// "[hh:mm:ss] Speaker: text" per segment with text, in input order.
std::vector<std::string> transcript_lines(const std::vector<Json>& segments,
                                          const Json& participants);

// Title, date and participants, a blank line, then the transcript ("(no transcript)" without
// one); ends with a newline.
std::string export_transcript_txt(const Json& meeting, const Json& state,
                                  const std::vector<Json>& segments,
                                  const Clock& clock = system_clock());

} // namespace fv::meeting_state
