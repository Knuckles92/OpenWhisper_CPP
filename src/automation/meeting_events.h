#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

#include "base/events.h"
#include "base/json.h"

// Meeting engine events as public meeting.* events (services/automation/meeting_events.py). The
// meeting runtime calls forward for every event of the current meeting's engine, after its own
// handling. Only four things become public, each built from named fields so nothing private
// leaks (never the engine's "_"-prefixed keys, dashboard links or guest tokens, capture or
// finalization details):
// - meeting.started: the first "status" event that says the meeting is running (active or
//   paused), once per meeting. The engine sends it once capture, the dashboard and processing
//   are up, so a start that fails earlier publishes nothing.
// - meeting.ended: the engine's "ended" event, with its status ("ended" when it has none).
// - meeting.segments: transcript lines as the engine commits them, blank ones dropped. The
//   replacement transcript a re-decode writes after the meeting (a payload with "removed_ids")
//   is not new lines.
// - meeting.voice_command: a spoken command the note taker applied.
namespace fv::automation::meeting_events {

// Meetings remembered as announced; one runs at a time, so a handful is plenty.
inline constexpr std::size_t ANNOUNCED_LIMIT = 32;

// Affinity: thread-safe.
class MeetingEventForwarder {
public:
    // Publishes the public event for one engine event on `sink`; never throws. `kind` is the
    // engine listener kind (status, segments, ended, voice_command, ...); `payload` the engine's,
    // which may carry private keys; `meeting_id` the engine's meeting, for payloads that don't
    // name it. An event of no meeting is dropped.
    void forward(std::string_view kind, const Json& payload, std::string_view meeting_id,
                 EventSink& sink);
    // Forgets the meetings already announced.
    void forget();

private:
    bool first_announcement(const std::string& meeting_id);

    std::mutex mutex_;
    std::deque<std::string> announced_; // oldest first
};

// forward over the process-wide forwarder, which remembers the announced meetings for the run.
void forward(std::string_view kind, const Json& payload, std::string_view meeting_id,
             EventSink& sink);
// Forgets the meetings the process-wide forwarder announced (tests).
void forget_announced();

} // namespace fv::automation::meeting_events
