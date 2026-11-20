// meeting/state/schema.py: the document's defaults, its round trip and the finalization rules
// (the golden file meeting.state.schema pins the full shapes).

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "base/testing/fake_clock.h"
#include "meeting_state/highlights.h"
#include "meeting_state/ids.h"
#include "meeting_state/schema.h"
#include "meeting_state/testing/fixed_ids.h"
#include "pyport/errors.h"

using fv::Json;
using namespace fv::meeting_state;

TEST_CASE("a new document has Python's defaults and key order", "[schema]") {
    MeetingState state;
    state.meeting_id = "m_new";
    const Json doc = state.to_json();

    std::vector<std::string> keys;
    for (const auto& [key, value] : doc.items()) {
        keys.push_back(key);
    }
    CHECK(keys == std::vector<std::string>{"meeting_id",
                                           "seq",
                                           "status",
                                           "cloud_enabled",
                                           "intelligence_online",
                                           "diarization_available",
                                           "diarization_reason",
                                           "speech",
                                           "title",
                                           "intent",
                                           "topic",
                                           "rolling_summary",
                                           "rolling_summary_evidence",
                                           "capture",
                                           "participants",
                                           "cards",
                                           "questions",
                                           "finalization",
                                           "report_views",
                                           "insight_review",
                                           "live_highlights",
                                           "live_highlights_status",
                                           "voice_feedback",
                                           "custom_reports"});
    CHECK(doc["report_views"] == Json{"ribbon", "brief", "signal"});
    CHECK(doc["capture"] == Json{{"mic_available", false},
                                 {"loopback_available", false},
                                 {"message", ""},
                                 {"audio_incomplete", false},
                                 {"integrity_error", ""}});
    CHECK(doc["cards"].size() == CARD_KEYS.size());
    CHECK(doc["finalization"]["status"] == "pending");
    CHECK(MeetingState::from_json(doc) == state);
}

TEST_CASE("legacy items are stamped from the clock", "[schema]") {
    const fv::FakeClock clock;
    const Json doc = {{"meeting_id", "m_old"},
                      {"cards", {{"risks", {{{"id", "it_1"}, {"card", "risks"}}}}}}};
    const MeetingState state = MeetingState::from_json(doc, clock);
    const CardItem& item = state.cards["risks"].at(0);
    CHECK(item.created_at == "2026-01-01T00:00:00Z");
    CHECK(item.updated_at == "2026-01-01T00:00:00Z");
    CHECK(item.status == "proposed");
    CHECK(item.revision == 1);
    // Legacy snapshot without finalization, AI insights off.
    CHECK(state.finalization.status == "disabled");
    CHECK(state.finalization.message == "AI insights are off for this meeting.");
}

TEST_CASE("malformed documents raise Python's exceptions", "[schema]") {
    const auto type_of = [](const Json& doc) {
        try {
            (void)MeetingState::from_json(doc);
        } catch (const fv::py::PyError& error) {
            return std::string(error.type_name());
        }
        return std::string();
    };
    CHECK(type_of(Json{{"status", "ended"}}) == "KeyError");
    CHECK(type_of(Json{{"meeting_id", "m"}, {"seq", "x"}}) == "ValueError");
    CHECK(type_of(Json{{"meeting_id", "m"}, {"cards", {{"risks", {"junk"}}}}}) == "TypeError");
    CHECK(type_of(Json{{"meeting_id", "m"}, {"cards", {"risks"}}}) == "AttributeError");
    CHECK(type_of(Json::array()) == "AttributeError");
}

TEST_CASE("finalization pills and historical normalization", "[schema]") {
    FinalizationState fin = FinalizationState::coerce(Json{{"status", "completed"}});
    CHECK(fin.history_pill() == std::pair<std::string, std::string>{"Ready", "success"});
    CHECK_FALSE(fin.history_pill("paused").has_value());
    fin.steps = Json::array({{{"status", "failed"}}});
    CHECK(fin.history_pill() == std::pair<std::string, std::string>{"Incomplete", "warning"});
    fin.card_deferred = true;
    CHECK(fin.history_pill() == std::pair<std::string, std::string>{"Saved for later", "warning"});

    const FinalizationState interrupted = FinalizationState::normalize_historical(
        Json{{"status", "running"}, {"steps", {{{"id", "a"}, {"status", "running"}}}}}, true,
        "ended");
    CHECK(interrupted.status == "failed");
    CHECK(interrupted.message == "Final insights were interrupted before they finished.");
    CHECK(interrupted.steps[0]["detail"] == "Interrupted before this step finished.");
    CHECK(FinalizationState::default_for_cloud(true).status == "pending");
    CHECK(FinalizationState::default_for_cloud(false).message ==
          "AI insights are off for this meeting.");
    CHECK(coerce_card_deferred(" Yes "));
    CHECK_FALSE(coerce_card_deferred("y"));
}

TEST_CASE("ids, ordered collections and pulse labels", "[schema]") {
    testing::FixedIds ids;
    CHECK(new_id("it", ids) == "it_000000000001");
    CHECK(new_id("q", ids) == "q_000000000002");
    const std::string random = new_id("p");
    CHECK(random.size() == 14);
    CHECK(random.starts_with("p_"));

    OrderedDict<int> dict;
    dict.set("b", 1);
    dict.set("a", 2);
    dict.set("b", 3);
    std::vector<std::pair<std::string, int>> entries(dict.begin(), dict.end());
    CHECK(entries == std::vector<std::pair<std::string, int>>{{"b", 3}, {"a", 2}});

    CHECK(pulse_label("commitment") == "Dated commitment");
    CHECK_FALSE(pulse_label("other").has_value());
    CHECK(parse_state_json("[1]") == std::nullopt);
    CHECK(parse_state_json(R"({"a": 1})") == Json{{"a", 1}});
}
