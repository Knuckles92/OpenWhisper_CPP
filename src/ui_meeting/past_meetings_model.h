#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/json.h"

// The rules of the Past Meetings list that need no widget (ui_qt/widgets/past_meetings_panel.py):
// which rows are history, the filter haystack, the count header, which placeholder shows, the
// lifecycle suffix and the notes on a card. Rows are the JSON objects the meeting repository
// lists. Affinity: thread-safe (pure functions).
namespace fv::past_meetings {

// PastMeetingsPanel.MAX_MEETINGS: cards shown at most; one more row is read to know there is
// more.
inline constexpr int kMaxMeetings = 100;

// Meetings still being captured (active, paused, ending) are not history yet.
bool is_historical(const Json& meeting);

// _load_meetings's loop: the rows that are history, each given a content_summary by `summarize`
// (called with its id) when it has none and a summarizer is set.
std::vector<Json> historical_meetings(const std::vector<Json>& rows,
                                      const std::function<Json(const std::string& id)>& summarize);

// _filter_meetings: the meetings whose haystack (title, started text, duration, preview, pill
// label and status, those that are not empty, joined by spaces) holds the query, stripped and
// lower-cased. An empty query keeps every meeting.
std::string filter_haystack(const Json& meeting, const Clock& clock = system_clock());
std::vector<Json> filter_meetings(const std::vector<Json>& meetings, std::string_view query,
                                  const Clock& clock = system_clock());

// The count line over the list: "{shown}{+} meeting(s)".
std::string count_text(int shown, bool has_more);

// What _rebuild_list shows.
struct ListView {
    std::string count;
    // The count line hides while there is nothing to count.
    bool count_visible = false;
    // The one placeholder that replaces the list ("No past meetings yet", "No matching
    // meetings"); empty when cards show.
    std::string placeholder;
    // The cards, at most kMaxMeetings, in display order.
    std::vector<Json> cards;
    // The placeholder after the cards; empty when none.
    std::string footer;
};

// meetings is what was loaded, source_filtered whether the source applied the query already,
// has_more whether more than kMaxMeetings were loaded, query the search box's text.
ListView list_view(const std::vector<Json>& meetings, bool source_filtered, bool has_more,
                   std::string_view query, const Clock& clock = system_clock());

// The warning under a card's preview: "Meeting failed to start", "No audio or transcript
// captured", "No transcript captured", "No audio captured"; empty when the meeting is fine.
std::string content_note(const Json& meeting);

// The footer's left text: the duration, then " · " and the lifecycle word for a meeting that did
// not simply end ("Failed", or the status in title case).
std::string detail_text(const Json& meeting, const Clock& clock = system_clock());

// Whether "Copy transcript" applies: content_summary.has_transcript when it is there, else
// whether the row has a preview.
bool has_transcript(const Json& meeting);
// Whether the delete dialog offers recordings: content_summary.has_audio when it is there, else
// whether the row has a spool_dir.
bool has_audio(const Json& meeting);

// _meeting_location: (pill, tooltip) for where a meeting is kept, empty for a meeting kept here
// alone. Host-kept rows come from M9's listing.
std::pair<std::string, std::string> meeting_location(const Json& meeting);

} // namespace fv::past_meetings
