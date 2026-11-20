#include "meeting/demo_fixture.h"

#include <array>
#include <format>
#include <stdexcept>
#include <utility>
#include <vector>

#include "base/log.h"
#include "meeting/detail/py_value.h"
#include "meeting_state/types.h"
#include "pyport/text.h"

namespace fv::meeting {

namespace {

const log::Logger logger("meeting.dev_fixture");

using meeting_state::OpResult;

// A few lines keep speech-recognition artifacts ("um", doubled words) so transcript polish has
// something to clean.
constexpr std::array<DemoTurn, 25> kTurns{{
    {"me", 4.0, 12.0, "Welcome everyone. Thanks for making time for this planning sync."},
    {"jordan", 13.0, 18.5, "Yeah thanks for setting this up. I have the Q3 notes ready."},
    {"sam", 19.5, 25.0, "Same here. Can we start with the beta date?"},
    {"me", 26.0, 38.0,
     "um so I I think we should ship the beta in June. That's the date we floated last week."},
    {"jordan", 39.5, 52.0,
     "June works if we freeze the API by May fifteenth. Otherwise the mobile team slips."},
    {"sam", 53.0, 64.0,
     "I'm good with June. We we decided to go with FastAPI last Thursday, right?"},
    {"me", 65.0, 78.0,
     "Yes. FastAPI for the dashboard backend, React for the UI. Let's treat that as a decision."},
    {"jordan", 79.0, 94.0, "I'll draft the RFC this week. Who reviews it?"},
    {"me", 95.0, 108.0, "I can review it. Target Friday so we can socialize it Monday."},
    {"sam", 109.0, 128.0,
     "One risk: the vendor delay on the speech model license. If that slips we we have to ship "
     "with the fallback model."},
    {"jordan", 129.0, 145.0, "Yeah that's the main risk. Can we get a drop-dead date from them?"},
    {"me", 146.0, 162.0,
     "I'll ping them today. If we don't hear by next Wednesday we switch to the fallback and "
     "document it in the RFC."},
    {"sam", 163.0, 178.0, "Should the RFC also cover guest links and the LAN bind option?"},
    {"jordan", 179.0, 196.0, "Yes, and retention. People asked how long we keep meeting audio."},
    {"me", 197.0, 214.0,
     "Keep audio for fourteen days unless the host exports. That's the current proposal."},
    {"sam", 215.0, 232.0, "Works for me. Is the June deadline firm? Leadership keeps asking."},
    {"me", 233.0, 248.0,
     "It's firm unless the vendor miss forces a fallback. Let's keep that as an open question "
     "until I hear back."},
    {"jordan", 249.0, 268.0,
     "Timeline so far: kickoff is done, API freeze May fifteenth, beta in June, RFC draft this "
     "week."},
    {"sam", 269.0, 286.0,
     "I'll take the mobile client spike after the RFC lands. Need the auth section first."},
    {"me", 287.0, 304.0, "Noted. Anything else before we wrap the planning part?"},
    {"jordan", 305.0, 322.0,
     "Just the action items: I draft the RFC, you review, you chase the vendor, Sam owns the "
     "mobile spike."},
    {"sam", 323.0, 338.0, "And we should flag the license risk as high until we get a date."},
    {"me", 339.0, 356.0,
     "Agreed. I'll also add a timeline beat for kickoff so the dashboard isn't empty. Thanks "
     "everyone."},
    {"jordan", 357.0, 366.0, "Thanks. I'll send the RFC outline this afternoon."},
    {"sam", 367.0, 376.0, "Sounds good. Talk Friday."},
}};

constexpr std::array<DemoSpeaker, 2> kSpeakers{{
    {"jordan", "Jordan", "others_cluster"},
    {"sam", "Sam", "others_cluster"},
}};

std::string op_name(const OpResult& result) {
    return detail::py_str(detail::get(result.op, "op"));
}

void warn_dropped(const std::vector<OpResult>& results, std::string_view what) {
    std::size_t failed = 0;
    std::string listed;
    for (const OpResult& result : results) {
        if (result.ok) {
            continue;
        }
        ++failed;
        if (!listed.empty()) {
            listed += ", ";
        }
        listed += op_name(result) + ":" + result.reason.value_or("None");
    }
    if (failed > 0) {
        logger.warning("Demo seed dropped {} {}: {}", failed, what, listed);
    }
}

std::map<std::string, std::optional<std::string>, std::less<>>
seed_participants(meeting_state::MeetingStateStore& store,
                  const std::optional<std::string>& me_participant_id,
                  const std::optional<std::string>& title) {
    std::map<std::string, std::optional<std::string>, std::less<>> speaker_ids;
    speaker_ids["me"] = me_participant_id;
    std::string text = title && !title->empty() ? *title : std::string(DEMO_TITLE);
    text = py::strip(text);
    if (text.empty()) {
        text = std::string(DEMO_TITLE);
    }
    Json ops = Json::array();
    ops.push_back(Json{{"op", "set_title"}, {"text", text}});
    for (const DemoSpeaker& speaker : kSpeakers) {
        ops.push_back(Json{{"op", "upsert_participant"},
                           {"display_name", std::string(speaker.name)},
                           {"kind", std::string(speaker.kind)},
                           {"is_provisional", false}});
    }
    const std::vector<OpResult> results = store.apply("system", std::string("demo"), ops);
    for (std::size_t i = 0; i < results.size() && i <= kSpeakers.size(); ++i) {
        const OpResult& result = results[i];
        if (!result.ok) {
            throw std::runtime_error(std::format("Demo seed failed on {}: {}", op_name(result),
                                                 result.reason.value_or("None")));
        }
        if (i == 0) {
            continue;
        }
        const Json& participant = detail::get(result.effect, "participant");
        speaker_ids[std::string(kSpeakers[i - 1].key)] =
            detail::py_str(detail::get(participant, "id"));
    }
    const auto me = speaker_ids.find("me");
    if (me == speaker_ids.end() || !me->second || me->second->empty()) {
        throw std::runtime_error("Demo seed needs a host participant");
    }
    return speaker_ids;
}

Json card_ops(const std::vector<std::string>& segment_ids) {
    const auto evidence = [&segment_ids](std::initializer_list<std::size_t> indexes) {
        Json out = Json::array();
        for (const std::size_t index : indexes) {
            if (index < segment_ids.size()) {
                out.push_back(segment_ids[index]);
            }
        }
        return out;
    };
    Json ops = Json::array();
    ops.push_back(Json{
        {"op", "set_topic"}, {"text", "Q3 roadmap and June beta"}, {"evidence", evidence({3, 5})}});
    ops.push_back(Json{{"op", "set_rolling_summary"},
                       {"text", "The team is aligning on a June beta, FastAPI for the backend, and "
                                "an RFC this week. A vendor license delay is the main risk."},
                       {"evidence", evidence({3, 5, 9})}});
    ops.push_back(Json{{"op", "add_item"},
                       {"card", "key_points"},
                       {"text", "Ship the beta in June"},
                       {"evidence", evidence({3})}});
    ops.push_back(Json{{"op", "add_item"},
                       {"card", "decisions"},
                       {"text", "Use FastAPI for the dashboard backend"},
                       {"evidence", evidence({5, 6})}});
    ops.push_back(Json{{"op", "add_item"},
                       {"card", "action_items"},
                       {"text", "Draft the RFC this week"},
                       {"data", Json::object()},
                       {"evidence", evidence({7})}});
    ops.push_back(Json{{"op", "add_item"},
                       {"card", "risks"},
                       {"text", "Vendor delay on the speech model license"},
                       {"data", Json{{"severity", "high"}}},
                       {"evidence", evidence({9})}});
    ops.push_back(Json{{"op", "add_item"},
                       {"card", "timeline"},
                       {"text", "Kickoff done"},
                       {"data", Json{{"start_s", 4.0}}},
                       {"evidence", evidence({0})}});
    ops.push_back(Json{{"op", "ask_question"},
                       {"text", "Is the June deadline firm?"},
                       {"evidence", evidence({15})}});
    return ops;
}

// The FastAPI decision is confirmed and pinned, so it survives the proposed-card cleanup.
Json followup_ops(const std::vector<OpResult>& card_results) {
    Json ops = Json::array();
    for (const OpResult& result : card_results) {
        if (!result.ok || op_name(result) != "add_item") {
            continue;
        }
        if (detail::get(result.op, "card") != Json("decisions")) {
            continue;
        }
        if (!result.target_id || result.target_id->empty()) {
            continue;
        }
        ops.push_back(Json{{"op", "confirm_item"}, {"id", *result.target_id}});
        ops.push_back(Json{{"op", "pin_item"}, {"id", *result.target_id}});
    }
    return ops;
}

} // namespace

std::span<const DemoTurn> demo_turns() {
    return kTurns;
}

std::span<const DemoSpeaker> demo_speakers() {
    return kSpeakers;
}

DemoSeedResult seed_demo_meeting(const std::string& meeting_id,
                                 const std::optional<std::string>& me_participant_id,
                                 meeting_state::MeetingStateStore& store,
                                 meeting_state::MeetingRepository& repository, MeetingClock* clock,
                                 const std::optional<std::string>& title,
                                 meeting_state::IdSource& ids) {
    DemoSeedResult result;
    result.participant_ids = seed_participants(store, me_participant_id, title);

    std::vector<meeting_state::TranscriptSegment> segments;
    std::vector<std::string> segment_ids;
    for (const DemoTurn& turn : kTurns) {
        meeting_state::TranscriptSegment segment;
        segment.segment_id = meeting_state::new_id("sg", ids);
        segment.meeting_id = meeting_id;
        const bool is_host = turn.speaker == "me";
        segment.channel =
            std::string(is_host ? meeting_state::kChannelMic : meeting_state::kChannelLoopback);
        segment.start_s = turn.start_s;
        segment.end_s = turn.end_s;
        segment.text = std::string(turn.text);
        const auto found = result.participant_ids.find(turn.speaker);
        segment.speaker_participant_id =
            found != result.participant_ids.end() ? found->second : std::nullopt;
        segment.speaker_source = is_host ? "channel" : "diarizer";
        segment_ids.push_back(segment.segment_id);
        segments.push_back(std::move(segment));
    }
    repository.add_segments(segments);
    result.segment_count = segments.size();
    result.last_end_s = segments.empty() ? 0.0 : segments.back().end_s;

    const std::vector<OpResult> card_results =
        store.apply("system", std::string("demo"), card_ops(segment_ids));
    warn_dropped(card_results, "card ops");

    const Json followup = followup_ops(card_results);
    if (!followup.empty()) {
        warn_dropped(store.apply("system", std::string("demo"), followup), "follow-up ops");
    }

    if (clock != nullptr && result.last_end_s > 0) {
        clock->resume_from_recovery(result.last_end_s);
    }
    return result;
}

} // namespace fv::meeting
