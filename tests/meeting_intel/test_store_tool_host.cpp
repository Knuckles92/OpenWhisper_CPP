// meeting/reinsight.py's StoreToolHost: the two offline-host tests of
// tests/test_meeting_reinsight.py (Python reaches the host through rerun_insights, which is
// M8.10's; here the host is built directly over the same kind of store and repository), its
// golden vectors (tools/parity/cases/meeting.reinsight.py) and its own rules.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "golden/golden.h"
#include "meeting_intel/store_tool_host.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "report_support.h"

using namespace fv::meeting_intel;
using namespace fv::meeting_intel::test;
using fv::Json;
using fv::golden::Case;
using fv::meeting_state::MeetingStateStore;
using fv::meeting_state::OpResult;

namespace {

constexpr std::string_view kModule = "meeting.reinsight";
constexpr std::string_view kRerunId = "m_rerun";

fv::meeting_state::MeetingState rerun_state() {
    auto state = ended_state();
    state.meeting_id = std::string(kRerunId);
    state.title = "Re-run";
    return state;
}

// A host over the store of the ended meeting m_rerun (or over no store), as the vectors build it.
struct HostFixture {
    std::optional<StoreFixture> stored;
    ReportRepository repository;
    StubHistory history;
    Json settings = Json::object();
    RecallDeps recall{&history, [this] { return settings; }, nullptr, nullptr};
    bool writes = true;
    StoreToolHost host;

    explicit HostFixture(bool has_store, bool writes_allowed = true)
        : writes(writes_allowed),
          host(
              [this]() -> std::shared_ptr<MeetingStateStore> {
                  return stored ? stored->store : nullptr;
              },
              repository, [this] { return writes; }, &recall, [this] { return settings; }) {
        if (has_store) {
            stored.emplace(rerun_state());
        }
    }

    HostFixture(const HostFixture&) = delete;
    HostFixture& operator=(const HostFixture&) = delete;
};

Json result_json(const OpResult& result) {
    Json out =
        Json{{"ok", result.ok}, {"reason", result.reason ? Json(*result.reason) : Json(nullptr)}};
    if (!result.ok && (result.reason == "inactive" || result.reason == "agent_writes_revoked")) {
        out["op"] = result.op;
    }
    return out;
}

void check_equal(const Case& c, const Json& actual) {
    INFO(c.label());
    INFO("actual: " << actual.dump());
    CHECK(fv::golden::equal(c.raw().at("result"), actual));
}

std::string text_or_empty(const Json& value) {
    return value.is_string() ? value.get<std::string>() : std::string();
}

} // namespace

TEST_CASE("TestReadTools::test_offline_host_exposes_context_folder_search",
          "[test_meeting_reinsight]") {
    // from
    // tests/test_meeting_reinsight.py::TestReadTools::test_offline_host_exposes_context_folder_search
    HostFixture fixture(true);
    fixture.settings = Json{{"meeting_context_folder_enabled", false}};
    const Json seen = fixture.host.search_context_files("budget", "", Json(10));
    CHECK(seen.at("disabled") == true);
    CHECK(seen.at("hits") == Json::array());
}

TEST_CASE("a revoked meeting agent cannot read the past or the knowledge folder",
          "[test_meeting_reinsight]") {
    HostFixture fixture(true, false);
    fixture.settings =
        Json{{"meeting_context_folder_enabled", true}, {"meeting_past_recall_enabled", true}};
    const auto recall = fixture.host.search_past_meetings("private words", "", Json(10));
    const auto files = fixture.host.search_context_files("private words", "", Json(10));
    CHECK(recall.at("disabled") == true);
    CHECK(files.at("disabled") == true);
    CHECK(recall.at("hits").empty());
    CHECK(files.at("hits").empty());
}

TEST_CASE("TestReadTools::test_offline_host_checks_evidence_like_a_live_one",
          "[test_meeting_reinsight]") {
    // from
    // tests/test_meeting_reinsight.py::TestReadTools::test_offline_host_checks_evidence_like_a_live_one
    // The agent's evidence repair sees stored segments on a re-run too.
    fv::FakeClock clock;
    fv::meeting_state::testing::FakeMeetingRepository repository(clock);
    repository.create_meeting(Json{{"id", kRerunId},
                                   {"title", "Re-run"},
                                   {"status", "ended"},
                                   {"started_at", "2026-11-17T09:00:00+00:00"},
                                   {"host_token", "h"},
                                   {"guest_token", "g"},
                                   {"spool_dir", "spool"}});
    fv::meeting_state::TranscriptSegment segment;
    segment.segment_id = "sg_1";
    segment.meeting_id = std::string(kRerunId);
    segment.channel = "mic";
    segment.start_s = 1.0;
    segment.end_s = 3.0;
    segment.text = "We ship the beta.";
    repository.add_segments({segment});
    StoreFixture stored(rerun_state());
    StoreToolHost host([&stored] { return stored.store; }, repository, {}, nullptr,
                       [] { return Json::object(); });
    CHECK(host.segment_exists("sg_1"));
    CHECK_FALSE(host.segment_exists("sg_missing"));
}

TEST_CASE("the store tool host matches OpenWhisper", "[golden][meeting.reinsight]") {
    fv::golden::for_each(kModule, "apply_agent_ops", [](const Case& c) {
        HostFixture fixture(c.arg<bool>(0), c.arg<bool>(1));
        Json results = Json::array();
        for (const auto& result : fixture.host.apply_agent_ops(c.args().at(2))) {
            results.push_back(result_json(result));
        }
        check_equal(c, Json{{"results", results}, {"applied", fixture.host.applied()}});
    });
    fv::golden::for_each(kModule, "question", [](const Case& c) {
        HostFixture fixture(c.arg<bool>(0), c.arg<bool>(1));
        const Json& args = c.args().at(3);
        const OpResult result =
            c.arg<std::string>(2) == "ask_question"
                ? fixture.host.ask_question(args.at(0).get<std::string>(), args.at(1))
                : fixture.host.resolve_question(args.at(0).get<std::string>(),
                                                args.at(1).get<std::string>(),
                                                args.at(2).get<double>(), args.at(3));
        Json actual = result_json(result);
        actual["applied"] = fixture.host.applied();
        check_equal(c, actual);
    });
    fv::golden::for_each(kModule, "segment_exists", [](const Case& c) {
        HostFixture fixture(c.arg<bool>(0));
        fixture.repository.known_segments = c.arg<std::vector<std::string>>(1);
        fixture.repository.fail_segment_checks = c.arg<bool>(3);
        const bool exists = fixture.host.segment_exists(c.arg<std::string>(2));
        Json asked = nullptr;
        if (!fixture.repository.segment_checks.empty()) {
            const auto& [meeting_id, segment_id] = fixture.repository.segment_checks.back();
            asked = Json::array({meeting_id, segment_id});
        }
        check_equal(c, Json{{"exists", exists}, {"asked", asked}});
    });
    fv::golden::for_each(kModule, "search_past_meetings", [](const Case& c) {
        HostFixture fixture(c.arg<bool>(0));
        fixture.settings = Json{{"meeting_past_recall_enabled", c.arg<bool>(1)}};
        for (const auto& row : c.args().at(2)) {
            fixture.history.rows.push_back(row);
        }
        check_equal(c, fixture.host.search_past_meetings(
                           c.arg<std::string>(3), text_or_empty(c.args().at(4)), c.args().at(5)));
    });
    fv::golden::for_each(kModule, "search_context_files", [](const Case& c) {
        HostFixture fixture(true);
        fixture.settings = Json{{"meeting_context_folder_enabled", c.arg<bool>(0)}};
        check_equal(c, fixture.host.search_context_files(
                           c.arg<std::string>(1), text_or_empty(c.args().at(2)), c.args().at(3)));
    });
}

TEST_CASE("the store tool host refuses writes without a store or once they are revoked",
          "[store_tool_host]") {
    HostFixture fixture(true);
    const Json topic = Json{{"op", "set_topic"}, {"text", "Beta"}};
    const auto seq = fixture.stored->store->seq();
    fixture.writes = false;
    auto refused = fixture.host.apply_agent_ops(Json::array({topic}));
    REQUIRE(refused.size() == 1);
    CHECK(refused[0].reason == "agent_writes_revoked");
    CHECK(refused[0].op == topic);
    // Nothing reached the store.
    CHECK(fixture.stored->store->seq() == seq);
    fixture.writes = true;
    fixture.stored.reset();
    CHECK(fixture.host.ask_question("Who signs?", Json::array()).reason == "inactive");
    CHECK_FALSE(fixture.host.segment_exists("sg_1"));
    CHECK(fixture.repository.segment_checks.empty());
    CHECK(fixture.host.applied() == 0);
}

TEST_CASE("the store tool host counts the ops the store applied", "[store_tool_host]") {
    HostFixture fixture(true);
    const auto results = fixture.host.apply_agent_ops(
        Json::array({Json{{"op", "no_such_op"}}, Json{{"op", "reassign_segment_speaker"},
                                                      {"segment_id", "sg_1"},
                                                      {"participant_id", "p_1"}}}));
    REQUIRE(results.size() == 2);
    CHECK(results[0].reason == "unknown_op");
    CHECK(results[1].reason == "agent_forbidden");
    CHECK(fixture.host.applied() == 0);
    // The agent must cite evidence; a cited question lands and counts.
    CHECK(fixture.host.ask_question("What is the budget?", Json::array()).reason ==
          "missing_evidence");
    CHECK(fixture.host.applied() == 0);
    const OpResult asked = fixture.host.ask_question("What is the budget?", Json::array({"sg_1"}));
    CHECK(asked.ok);
    CHECK(fixture.host.applied() == 1);
    CHECK(fixture.stored->store->snapshot().at("questions").size() == 1);
}

TEST_CASE("the store tool host searches past meetings around the store's meeting",
          "[store_tool_host]") {
    HostFixture fixture(true);
    fixture.settings = Json{{"meeting_past_recall_enabled", true}};
    fixture.history.rows = {Json{{"meeting_id", kRerunId}, {"text", "this meeting's budget"}},
                            Json{{"meeting_id", "m_past"},
                                 {"text", "we adopted the budget"},
                                 {"title", "Q1 review"},
                                 {"started_at", "2026-03-01T10:00:00"},
                                 {"start_s", 12.0}}};
    const Json found = fixture.host.search_past_meetings("budget", "", Json(10));
    REQUIRE(found.at("hits").size() == 1);
    CHECK(found.at("hits").at(0).at("meeting_id") == "m_past");
    REQUIRE(fixture.history.search_call.has_value());
    CHECK(fixture.history.search_call->exclude_meeting_id == std::string(kRerunId));
    // Without a recall source the host answers as Python does over no repository.
    StoreToolHost bare([&fixture] { return fixture.stored->store; }, fixture.repository, {},
                       nullptr, [] { return Json{{"meeting_past_recall_enabled", true}}; });
    CHECK(bare.search_past_meetings("budget", "", Json(10)).at("text") ==
          "Past-meeting recall is not available.");
}

TEST_CASE("AgentUnavailable carries its reason as reinsight.py's RuntimeError does",
          "[store_tool_host]") {
    try {
        throw AgentUnavailable("No agent core for the re-run.");
    } catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()) == "No agent core for the re-run.");
        const auto* typed = dynamic_cast<const fv::py::PyError*>(&error);
        REQUIRE(typed != nullptr);
        CHECK(typed->type_name() == "AgentUnavailable");
    }
}

TEST_CASE("the store tool host recalls through the repository it checks evidence in",
          "[store_tool_host]") {
    // Python's host reads recall and evidence from one repository; here the composition hands
    // it a RepositoryHistorySource over that repository (lead amendment 26).
    fv::FakeClock clock;
    fv::meeting_state::testing::FakeMeetingRepository repository(clock);
    const auto add_meeting = [&](std::string_view id, std::string_view title,
                                 std::string_view started_at, std::string_view segment_id,
                                 std::string_view text) {
        repository.create_meeting(Json{{"id", id},
                                       {"title", title},
                                       {"status", "ended"},
                                       {"started_at", started_at},
                                       {"host_token", std::string("h") + std::string(id)},
                                       {"guest_token", std::string("g") + std::string(id)},
                                       {"spool_dir", std::string("spool_") + std::string(id)}});
        fv::meeting_state::TranscriptSegment segment;
        segment.segment_id = std::string(segment_id);
        segment.meeting_id = std::string(id);
        segment.channel = "mic";
        segment.start_s = 12.0;
        segment.end_s = 15.0;
        segment.text = std::string(text);
        repository.add_segments({segment});
    };
    add_meeting(kRerunId, "Re-run", "2026-11-17T09:00:00+00:00", "sg_1", "this meeting's budget");
    add_meeting("m_q1", "Q1 review", "2026-03-01T10:00:00+00:00", "sg_2",
                "we adopted the budget ceiling");
    RepositoryHistorySource history(repository);
    const Json settings = Json{{"meeting_past_recall_enabled", true}};
    RecallDeps recall{&history, [settings] { return settings; }, nullptr, nullptr};
    StoreFixture stored(rerun_state());
    StoreToolHost host([&stored] { return stored.store; }, repository, {}, &recall,
                       [settings] { return settings; });

    const Json found = host.search_past_meetings("budget", "", Json(10));
    REQUIRE(found.at("hits").size() == 1);
    CHECK(found.at("hits").at(0).at("meeting_id") == "m_q1");
    const std::string text = found.at("text").get<std::string>();
    CHECK(text.find("adopted the budget ceiling") != std::string::npos);
    CHECK(text.find("this meeting's budget") == std::string::npos);
    // Past-meeting segment ids never reach the agent.
    CHECK(text.find("sg_2") == std::string::npos);

    const Json slice = host.search_past_meetings("", "m_q1", Json(10));
    CHECK(slice.at("text").get<std::string>().find("adopted the budget ceiling") !=
          std::string::npos);
    CHECK(host.segment_exists("sg_1"));
    CHECK_FALSE(host.segment_exists("sg_2")); // another meeting's segment is not evidence here
}
