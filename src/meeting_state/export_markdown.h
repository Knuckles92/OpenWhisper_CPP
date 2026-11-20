#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "base/clock.h"
#include "base/json.h"

// meeting/export/markdown.py: the dashboard as a shareable Markdown document. Title, a metadata
// line (date, duration, participants), the host's brief, then, with intelligence, the user's
// clarifications, topic with its history, summary, meeting pulses, one section per card,
// questions and the requested reports, and finally the timestamped transcript. Empty sections
// and removed items are skipped; evidence ids are never rendered (the JSON export carries
// them). Inputs as export_txt.h describes.
// Affinity: thread-safe (pure functions).
namespace fv::meeting_state {

// Heading depth a report body is pushed down: the document owns "#", "##" and "###".
inline constexpr int REPORT_HEADING_OFFSET = 3;

// Every ATX heading pushed down `offset` levels, capped at "######"; fenced code is left alone
// (a "#" there is a comment). The lines are rejoined with "\n" (splitlines drops a final one).
std::string demote_headings(std::string_view markdown, int offset = REPORT_HEADING_OFFSET);

// The document, ending with a newline. Without the transcript the transcript section is left
// out; without intelligence everything but the title, the metadata line and the brief is.
std::string export_markdown(const Json& meeting, const Json& state,
                            const std::vector<Json>& segments, bool include_transcript = true,
                            bool include_intelligence = true, const Clock& clock = system_clock());

} // namespace fv::meeting_state
