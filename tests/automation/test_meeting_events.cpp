#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "automation/catalog.h"
#include "automation/event_bus.h"
#include "automation/meeting_events.h"
#include "base/events.h"
#include "golden/golden.h"
#include "pyport/json.h"

// Meeting engine events become public meeting.* events, and nothing private leaks:
// test_automation_api_meeting_events.py, then the replay of
// tests/golden/services.automation.meeting_events.json.

using namespace fv;
using namespace fv::automation;
namespace me = fv::automation::meeting_events;

namespace {

using Published = std::vector<std::pair<std::string, Json>>;

// A fresh bus and a forwarder that has announced nothing, as the Python fixture's.
struct Rig {
    ~Rig() { bus.close(); }

    void forward(std::string_view kind, const Json& payload, std::string_view meeting_id = {}) {
        forwarder.forward(kind, payload, meeting_id, bus);
    }
    Published published() const {
        Published out;
        for (const Event& event : bus.recent(500)) {
            out.emplace_back(event.type, event.data);
        }
        return out;
    }

    EventBus bus;
    me::MeetingEventForwarder forwarder;
};

Json with_private(Json payload) {
    payload["_retry_generation"] = 3;
    payload["_finalization_claim"] = Json{{"owner", "x"}};
    return payload;
}

std::string type(std::string_view name) {
    return std::string(name);
}

} // namespace

TEST_CASE("test_the_first_running_status_announces_the_meeting_once",
          "[test_automation_api_meeting_events]") {
    Rig rig;
    rig.forward("server_started",
                Json{{"url", "http://127.0.0.1:9/m?token=guest-secret"},
                     {"host_url", "http://127.0.0.1:9/host?t=host-secret"},
                     {"guest_url", "http://127.0.0.1:9/guest?t=guest-secret"}},
                "m_1");
    rig.forward("status", with_private(Json{{"status", "starting"}}), "m_1");
    rig.forward("status", with_private(Json{{"status", "active"}, {"note", "Mic only"}}), "m_1");
    rig.forward("status", Json{{"status", "paused"}}, "m_1");
    rig.forward("status", Json{{"status", "active"}}, "m_1");
    CHECK(rig.published() ==
          Published{{type(catalog::MEETING_STARTED), Json{{"meeting_id", "m_1"}, {"title", ""}}}});
    rig.forward("status", Json{{"status", "active"}, {"title", " Standup "}}, "m_2");
    CHECK(rig.published().back() ==
          std::pair<std::string, Json>{type(catalog::MEETING_STARTED),
                                       Json{{"meeting_id", "m_2"}, {"title", "Standup"}}});
}

TEST_CASE("test_a_meeting_resumed_after_pause_is_not_announced_again",
          "[test_automation_api_meeting_events]") {
    Rig rig;
    rig.forward("status", Json{{"status", "paused"}}, "m_1");
    rig.forward("status", Json{{"status", "active"}}, "m_1");
    const Published published = rig.published();
    REQUIRE(published.size() == 1);
    CHECK(published[0].first == catalog::MEETING_STARTED);
}

TEST_CASE("test_ended_carries_the_status", "[test_automation_api_meeting_events]") {
    Rig rig;
    rig.forward("ended", with_private(Json{{"meeting_id", "m_1"},
                                           {"canceled", false},
                                           {"status", "needs_recovery"},
                                           {"unfinished_chunks", 2},
                                           {"capture_error", "device lost"}}));
    rig.forward("ended", Json{{"status", ""}}, "m_2");
    CHECK(rig.published() ==
          Published{
              {type(catalog::MEETING_ENDED),
               Json{{"meeting_id", "m_1"}, {"status", "needs_recovery"}}},
              {type(catalog::MEETING_ENDED), Json{{"meeting_id", "m_2"}, {"status", "ended"}}},
          });
}

TEST_CASE("test_committed_segments_are_published_as_lines",
          "[test_automation_api_meeting_events]") {
    Rig rig;
    const Json rows = Json::array({
        Json{{"id", "sg_1"},
             {"meeting_id", "m_1"},
             {"chunk_id", 4},
             {"channel", "mic"},
             {"start_s", 1.23456},
             {"end_s", 3.5},
             {"text", "Ship it Friday."},
             {"original_text", "ship it friday"},
             {"speaker_participant_id", "p_me"},
             {"speaker_source", "mic"},
             {"speaker_pinned", false},
             {"created_at", "x"}},
        Json{{"id", "sg_2"}, {"text", "   "}, {"start_s", 4}, {"end_s", 5}},
        Json{{"id", "sg_3"},
             {"text", "No speaker yet."},
             {"start_s", 6},
             {"end_s", 7.25},
             {"speaker_participant_id", nullptr}},
        "not a row",
    });
    rig.forward("segments", with_private(Json{{"items", rows}}), "m_1");
    const Published published = rig.published();
    REQUIRE(published.size() == 1);
    CHECK(published[0].first == catalog::MEETING_SEGMENTS);
    CHECK(py::py_equal(published[0].second,
                       Json{{"meeting_id", "m_1"},
                            {"items", Json::array({Json{{"segment_id", "sg_1"},
                                                        {"speaker", "p_me"},
                                                        {"text", "Ship it Friday."},
                                                        {"start_s", 1.235},
                                                        {"end_s", 3.5}},
                                                   Json{{"segment_id", "sg_3"},
                                                        {"speaker", ""},
                                                        {"text", "No speaker yet."},
                                                        {"start_s", 6.0},
                                                        {"end_s", 7.25}}})}}));
    // Seconds are floats, as Python's round(float(value), 3) gives them.
    CHECK(published[0].second.at("items").at(1).at("start_s").is_number_float());
}

TEST_CASE("test_a_redecoded_replacement_transcript_and_empty_batches_publish_nothing",
          "[test_automation_api_meeting_events]") {
    Rig rig;
    rig.forward("segments",
                Json{{"items", Json::array({Json{{"id", "sg_9"}, {"text", "Re-decoded."}}})},
                     {"removed_ids", Json::array({"sg_1"})}},
                "m_1");
    rig.forward("segments", Json{{"items", Json::array()}}, "m_1");
    rig.forward("segments", Json{{"items", Json::array({Json{{"id", "sg_1"}, {"text", ""}}})}},
                "m_1");
    CHECK(rig.published().empty());
}

TEST_CASE("test_voice_commands_are_published_by_name", "[test_automation_api_meeting_events]") {
    Rig rig;
    rig.forward("voice_command", Json{{"command", "recap"}, {"applied", 1}, {"rejected", 0}},
                "m_1");
    rig.forward("voice_command", Json{{"command", ""}}, "m_1");
    CHECK(rig.published() == Published{{type(catalog::MEETING_VOICE_COMMAND),
                                        Json{{"meeting_id", "m_1"}, {"command", "recap"}}}});
}

TEST_CASE("test_other_engine_events_stay_private", "[test_automation_api_meeting_events]") {
    for (std::string_view kind : {"server_started", "error", "intelligence", "background_ready",
                                  "asr_released", "voice_feedback", "retry_status", "unknown"}) {
        INFO(kind);
        Rig rig;
        rig.forward(kind,
                    Json{{"message", "guest link http://x/?t=secret"},
                         {"online", true},
                         {"guest_url", "http://x/?t=secret"},
                         {"phase", "saved"}},
                    "m_1");
        CHECK(rig.published().empty());
    }
}

TEST_CASE("test_nothing_private_reaches_any_event", "[test_automation_api_meeting_events]") {
    const std::string secret = "guest-secret-token";
    Rig rig;
    rig.forward("server_started", Json{{"guest_url", "http://x/?t=" + secret}}, "m");
    rig.forward("status",
                Json{{"status", "active"},
                     {"capture", Json{{"message", secret}}},
                     {"finalization", Json{{"message", secret}}},
                     {"speech", Json{{"x", secret}}},
                     {"_retry_generation", 1}},
                "m");
    rig.forward("segments",
                Json{{"items", Json::array({Json{{"id", "sg"},
                                                 {"text", "hello"},
                                                 {"original_text", secret},
                                                 {"channel", secret},
                                                 {"_private", secret}}})}},
                "m");
    rig.forward("ended", Json{{"status", "ended"}, {"error", secret}, {"capture_error", secret}},
                "m");
    const auto events = rig.bus.recent(500);
    Json dicts = Json::array();
    for (const Event& event : events) {
        dicts.push_back(event.to_dict());
    }
    const std::string dumped = py::dumps(dicts);
    CHECK(events.size() == 3);
    CHECK(dumped.find(secret) == std::string::npos);
    CHECK(dumped.find("_retry") == std::string::npos);
    CHECK(dumped.find("_private") == std::string::npos);
}

TEST_CASE("test_forward_never_raises", "[test_automation_api_meeting_events]") {
    // Python's {"status": object()} is a status that is not text.
    for (const Json& payload : {Json(), Json::array(), Json("status"),
                                Json{{"status", Json{{"not", "text"}}}}, Json{{"items", 5}}}) {
        Rig rig;
        for (std::string_view kind : {"status", "segments", "ended", "voice_command"}) {
            CHECK_NOTHROW(rig.forward(kind, payload, "m_1"));
        }
    }
}

TEST_CASE("test_forward_never_raises_when_publishing_fails",
          "[test_automation_api_meeting_events]") {
    class BrokenSink final : public EventSink {
    public:
        void publish(std::string_view, Json, bool) override {
            throw std::runtime_error("bus gone");
        }
    } broken;
    me::MeetingEventForwarder forwarder;
    CHECK_NOTHROW(forwarder.forward("status", Json{{"status", "active"}}, "m_1", broken));
}

TEST_CASE("test_events_without_a_meeting_are_dropped", "[test_automation_api_meeting_events]") {
    Rig rig;
    rig.forward("status", Json{{"status", "active"}});
    rig.forward("ended", Json{{"status", "ended"}});
    CHECK(rig.published().empty());
}

TEST_CASE("test_the_meeting_runtime_seam_forwards_engine_events",
          "[test_automation_api_meeting_events]") {
    // The meeting runtime (M7) forwards each engine event with its engine's meeting id through
    // the process-wide forwarder.
    me::forget_announced();
    EventBus bus;
    me::forward("status", Json{{"status", "active"}}, "m_7", bus);
    me::forward("segments",
                Json{{"items", Json::array({Json{{"id", "sg_1"},
                                                 {"meeting_id", "m_7"},
                                                 {"text", "Hi."},
                                                 {"start_s", 0.0},
                                                 {"end_s", 1.0}}})}},
                "m_7", bus);
    me::forward("ended", Json{{"meeting_id", "m_7"}, {"status", "ended"}}, "m_7", bus);
    std::vector<std::string> types;
    for (const Event& event : bus.recent(500)) {
        types.push_back(event.type);
    }
    bus.close();
    me::forget_announced();
    CHECK(types == std::vector<std::string>{type(catalog::MEETING_STARTED),
                                            type(catalog::MEETING_SEGMENTS),
                                            type(catalog::MEETING_ENDED)});
}

TEST_CASE("the process-wide forwarder remembers announced meetings until told to forget",
          "[meeting_events]") {
    me::forget_announced();
    EventBus bus;
    me::forward("status", Json{{"status", "active"}}, "m_8", bus);
    me::forward("status", Json{{"status", "active"}}, "m_8", bus);
    CHECK(bus.recent(500).size() == 1);
    me::forget_announced();
    me::forward("status", Json{{"status", "active"}}, "m_8", bus);
    CHECK(bus.recent(500).size() == 2);
    bus.close();
    me::forget_announced();
}

TEST_CASE("meeting events replay OpenWhisper", "[golden]") {
    golden::for_each("services.automation.meeting_events", "forward", [](const golden::Case& c) {
        Rig rig;
        for (const Json& step : c.arg<Json>(0)) {
            rig.forward(step.at(0).get<std::string>(), step.at(1), step.at(2).get<std::string>());
        }
        Json published = Json::array();
        for (const auto& [event_type, data] : rig.published()) {
            published.push_back(Json::array({event_type, data}));
        }
        CHECK(golden::equal(c.result<Json>(), published));
    });
}
