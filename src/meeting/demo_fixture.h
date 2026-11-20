#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "meeting_capture/meeting_clock.h"
#include "meeting_state/ids.h"
#include "meeting_state/repository.h"
#include "meeting_state/store.h"

// meeting/dev_fixture.py: the canned demo meeting of developer mode. It seeds a live meeting with
// a multi-speaker transcript and a handful of mid-meeting cards, so End can exercise polish,
// state repair and the final report without capturing audio.
// Affinity: functions; they block on the store and the repository.
namespace fv::meeting {

inline constexpr std::string_view DEMO_TITLE = "Demo Planning Sync";
inline constexpr std::string_view DEMO_NOTE = "Demo meeting loaded — no audio is being captured.";

// One transcript turn: "me" is the host's microphone, the others are system-audio speakers.
// Times are meeting seconds.
struct DemoTurn {
    std::string_view speaker;
    double start_s;
    double end_s;
    std::string_view text;
};

// The 25 turns, in order.
std::span<const DemoTurn> demo_turns();

// The two system-audio speakers: (key, display name, participant kind).
struct DemoSpeaker {
    std::string_view key;
    std::string_view name;
    std::string_view kind;
};
std::span<const DemoSpeaker> demo_speakers();

struct DemoSeedResult {
    std::size_t segment_count = 0;
    double last_end_s = 0.0;
    // Participant ids by speaker key ("me", "jordan", "sam").
    std::map<std::string, std::optional<std::string>, std::less<>> participant_ids;
};

// Sets the title (`title`, else DEMO_TITLE), adds the demo speakers, stores the transcript,
// applies the cards (topic, summary, key point, decision, action item, risk, timeline beat,
// question) as system/demo, confirms and pins the decision, and re-anchors `clock` (when given)
// at the last turn's end. Throws std::runtime_error "Demo seed failed on <op>: <reason>" when a
// participant or the title is refused, and "Demo seed needs a host participant" without one.
DemoSeedResult seed_demo_meeting(const std::string& meeting_id,
                                 const std::optional<std::string>& me_participant_id,
                                 meeting_state::MeetingStateStore& store,
                                 meeting_state::MeetingRepository& repository,
                                 MeetingClock* clock = nullptr,
                                 const std::optional<std::string>& title = std::nullopt,
                                 meeting_state::IdSource& ids = meeting_state::random_ids());

} // namespace fv::meeting
