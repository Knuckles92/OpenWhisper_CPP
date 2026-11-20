#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "dictation/events.h"
#include "dictation/job.h"

// Every event type the app publishes, with one builder per type (services/automation/catalog.py).
// Publishers call a builder rather than writing objects by hand, so a type's payload is the same
// everywhere. Payloads never carry selected text, caret context, window titles, file paths or
// audio. The job builders live in fv_dictation (dictation/events.h), which publishes them without
// depending on fv_automation; they are re-exported here. Every function is thread-safe.
namespace fv::automation {
struct Action;
}

namespace fv::automation::catalog {

using dictation::events::HISTORY_SAVED;
using dictation::events::RECORDING_CANCELED;
using dictation::events::RECORDING_STARTED;
using dictation::events::RECORDING_STOPPED;
using dictation::events::TRANSCRIPTION_COMPLETED;
using dictation::events::TRANSCRIPTION_FAILED;
using dictation::events::TRANSCRIPTION_PREVIEW;
inline constexpr std::string_view ACTION_RAN = "action.ran";
inline constexpr std::string_view CHANNEL_MESSAGE = "channel.message";
inline constexpr std::string_view ROUTE_DECIDED = "route.decided";
inline constexpr std::string_view FILTER_FAILED = "filter.failed";
inline constexpr std::string_view MEETING_STARTED = "meeting.started";
inline constexpr std::string_view MEETING_ENDED = "meeting.ended";
inline constexpr std::string_view MEETING_SEGMENTS = "meeting.segments";
inline constexpr std::string_view MEETING_VOICE_COMMAND = "meeting.voice_command";
// Synthetic, sent only on an event stream whose client missed events.
inline constexpr std::string_view EVENTS_GAP = "events.gap";

// EVENT_TYPES: (type, one-line description) in catalog order, for checklists, the docs and MCP
// events/list.
const std::vector<std::pair<std::string_view, std::string_view>>& EVENT_TYPES();
// EVENT_TYPES[type], or nullopt for a type that is not in the catalog.
std::optional<std::string_view> describe_type(std::string_view type);
// Every type but EVENTS_GAP, in catalog order.
const std::vector<std::string_view>& PUBLIC_TYPES();

// Values of `source`: who started the job.
using dictation::SOURCES;
using dictation::user_started;
// Sources where the user started the job; any other is a program's request.
inline constexpr std::string_view USER_SOURCES[] = {"hotkey", "ui", "dbus"};

// channel.message's `origin`: the user's own words, or words a program supplied or changed
// (DictationJob::program_words).
inline constexpr std::string_view ORIGIN_DICTATED = "dictated";
inline constexpr std::string_view ORIGIN_PROGRAM = "program";

using dictation::events::FAILURE_CANCELED;
using dictation::events::FAILURE_FAILED;
using dictation::events::FAILURE_NO_SPEECH;
using dictation::events::FAILURE_REFUSED;
using dictation::events::FILE_MODE;
inline constexpr std::string_view FAILURES[] = {FAILURE_CANCELED, FAILURE_NO_SPEECH,
                                                FAILURE_REFUSED, FAILURE_FAILED};

// events.gap's `reason`: the app no longer holds events published after the stream's position
// (more came than it keeps, or history was deleted); fetch history to catch up.
inline constexpr std::string_view GAP_EVICTED = "evicted";
// The position is from before the app restarted: what the earlier run published after it
// cannot be replayed; everything since the restart is.
inline constexpr std::string_view GAP_RESTARTED = "restarted";
// The stream fell too far behind and ends; reconnecting with Last-Event-ID replays the rest.
inline constexpr std::string_view GAP_SLOW = "slow";

// `types` filtered to public event types, order kept, duplicates dropped.
std::vector<std::string> known_types(const std::vector<std::string>& types);

using dictation::events::app_from_fields;
using dictation::events::history_saved;
using dictation::events::job_fields;
using dictation::events::recording_canceled;
using dictation::events::recording_started;
using dictation::events::recording_stopped;
using dictation::events::transcription_completed;
using dictation::events::transcription_failed;
using dictation::events::transcription_preview;

// `text` is the words the action got, for "Send to my scripts" only. `message` is cut to 500
// code points; an empty output reads "none".
Json action_ran(const dictation::DictationJob* job, const Action& action, bool ok,
                std::string_view message, std::string_view output,
                const std::optional<std::string>& text = std::nullopt);
// `source` says who started the job; `origin` whose words these are: "program" when a program
// supplied or changed them, else "dictated". A null job counts as a program's. `app` is copied
// when it is a non-empty object, else null.
Json channel_message(const dictation::DictationJob* job, const Action& action,
                     std::string_view text, const Json& app);
// confidence is rounded to 4 places as Python's round does.
Json route_decided(const dictation::DictationJob* job, std::string_view action_id,
                   std::optional<double> confidence, bool fallback);
// reason is cut to 200 code points.
Json filter_failed(std::string_view filter_id, std::string_view name, std::string_view stage,
                   std::string_view reason);
Json meeting_started(std::string_view meeting_id, std::string_view title);
Json meeting_ended(std::string_view meeting_id, std::string_view status);
// Each item: segment_id (its "segment_id", else its "id", as text), speaker, text, start_s and
// end_s (null when absent).
Json meeting_segments(std::string_view meeting_id, const Json& items);
Json meeting_voice_command(std::string_view meeting_id, std::string_view command);
// reason is GAP_EVICTED, GAP_RESTARTED or GAP_SLOW.
Json events_gap(std::int64_t missed_from_seq, std::string_view reason);

} // namespace fv::automation::catalog
