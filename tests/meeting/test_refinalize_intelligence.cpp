// tests/test_meeting_refinalize.py and tests/test_meeting_reinsight.py: the retry of a stored
// meeting's cleanup and report passes through the app's intelligence provider. OpenWhisper's
// rerun_polish and rerun_insights are MeetingIntelligence::polish and ::consolidate here, reached
// through rerun_finalization's "polish" and "consolidation" steps (what the dashboard's re-run
// and the desktop retry call), each with a throwaway core that is always shut down.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "base/log.h"
#include "base/testing/fake_clock.h"
#include "intelligence_support.h"
#include "meeting/finalization.h"
#include "meeting/meeting_intelligence.h"
#include "meeting/refinalize.h"
#include "meeting_state/schema.h"
#include "meeting_state/store.h"
#include "meeting_state/stored.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "recovery_support.h"

using fv::Json;
using fv::meeting_agent::AgentResult;
using fv::meeting_agent::CheckpointPayload;
using fv::meeting_state::CardItem;
using fv::meeting_state::MeetingState;
using fv::meeting_state::TranscriptSegment;
using namespace fv::meeting;
using namespace fv::meeting::test;
using Catch::Matchers::ContainsSubstring;

namespace {

using Core = fv::meeting_agent::testing::FakeAgentCore;

const fv::FakeClock& test_clock() {
    static const fv::FakeClock clock(
        std::chrono::sys_days{std::chrono::year{2026} / 1 / 1} + std::chrono::hours(12), "UTC");
    return clock;
}

TranscriptSegment segment(const std::string& id, const std::string& channel, double start,
                          double end, const std::string& text,
                          const std::string& meeting_id = "m_retry") {
    TranscriptSegment out;
    out.segment_id = id;
    out.meeting_id = meeting_id;
    out.channel = channel;
    out.start_s = start;
    out.end_s = end;
    out.text = text;
    return out;
}

// make_meeting: an ended meeting with AI insights on, recorded on openrouter with "test/model".
void make_meeting(SqlRepo& sql, const std::optional<std::string>& state_json = std::nullopt,
                  const Json& extra = Json::object(), const std::string& meeting_id = "m_retry") {
    Json fields{{"id", meeting_id},
                {"title", "Budget sync"},
                {"status", "ended"},
                {"started_at", "2026-01-01T11:00:00"},
                {"ended_at", "2026-01-01T11:30:00"},
                {"host_token", "host-token"},
                {"guest_token", "guest-token"},
                {"cloud_enabled", true},
                {"spool_dir", "/tmp/spool"},
                {"agent_provider", "openrouter"},
                {"agent_model", "test/model"},
                {"state_json", state_json ? Json(*state_json) : Json()},
                {"state_seq", 0}};
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        fields[it.key()] = it.value();
    }
    sql.repo->create_meeting(fields);
}

void add_transcript(SqlRepo& sql, const std::string& meeting_id = "m_retry") {
    sql.repo->add_segments({
        segment("sg_1", "mic", 0.0, 2.0, "We should ship the budget review on Friday.", meeting_id),
        segment("sg_2", "loopback", 2.0, 4.0, "Agreed, Friday works.", meeting_id),
    });
}

std::string seeded_state(const std::string& meeting_id, const Json& steps) {
    MeetingState state;
    state.meeting_id = meeting_id;
    state.status = "ended";
    state.cloud_enabled = true;
    state.title = "Budget sync";
    state.finalization.status = "failed";
    state.finalization.message = "needs retry";
    state.finalization.steps = steps;
    state.finalization.total_steps = static_cast<std::int64_t>(steps.size());
    return fv::py::dumps(state.to_json());
}

Json step(const std::string& id, const std::string& name, const std::string& status,
          const std::optional<std::string>& detail = std::nullopt) {
    Json out = {{"id", id}, {"name", name}, {"status", status}};
    if (detail) {
        out["detail"] = *detail;
    }
    return out;
}

const Json& default_steps() {
    static const Json steps = Json::array({
        step("redecode", "Audio Re-transcription", "failed",
             "Re-decoding failed; kept live transcript"),
        step("polish", "Transcript Cleanup", "completed", "Done"),
        step("consolidation", "Summary & Action Items", "completed", "Done"),
        step("finalize", "State Finalization", "completed", "Done"),
    });
    return steps;
}

std::vector<TranscriptSegment> rich_decode(const std::filesystem::path&, const std::vector<Json>&,
                                           const OfflineProgress&) {
    return {
        segment("sg_new_1", "mic", 0.0, 2.2,
                "We should ship the budget review on Friday as planned."),
        segment("sg_new_2", "loopback", 2.2, 4.4,
                "Agreed, Friday works for the whole team review."),
    };
}

const Json& step_of(const Json& result, const std::string& id) {
    for (const Json& entry : result.at("finalization").at("steps")) {
        if (entry.at("id") == id) {
            return entry;
        }
    }
    throw std::runtime_error("no step " + id);
}

// Python's ReplayAgentCore and PolishingAgentCore behind the app's provider: `ops` are applied
// through the tool host on the consolidation pass, a polish pass answers ok unless told to fail.
struct Agent {
    Agent()
        : provider(std::make_shared<MeetingIntelligence>(
              [this](std::string_view kind) -> std::unique_ptr<fv::meeting_agent::AgentCore> {
                  {
                      const std::lock_guard lock(mutex);
                      kinds.emplace_back(kind);
                  }
                  if (!creation_error.empty()) {
                      throw std::runtime_error(creation_error);
                  }
                  return std::make_unique<OwnedCore>(core);
              },
              [] { return Json::object(); },
              [](const fv::meeting_agent::MeetingAgentRoute&) { return true; })) {
        core->set_hook(
            [this](const CheckpointPayload& payload, fv::meeting_agent::AgentToolHost* tools) {
                AgentResult result;
                if (payload.is_polish) {
                    const int count = ++polish_calls;
                    if (fail_polish || (fail_polish_after && count > *fail_polish_after)) {
                        result.error = "polish failed";
                        return result;
                    }
                    result.ok = true;
                    return result;
                }
                if (!raises.empty()) {
                    throw std::runtime_error(raises);
                }
                if (!ops.empty() && tools != nullptr) {
                    result.op_results = tools->apply_agent_ops(ops);
                }
                result.ok = true;
                return result;
            });
    }

    RerunRequest request(SqlRepo& sql, const std::string& from_step) const {
        RerunRequest out;
        out.repository = sql.repo;
        out.from_step = from_step;
        out.clock = &test_clock();
        out.intelligence = provider;
        return out;
    }

    // What rerun_finalization hands the provider, for the cases that call a pass by itself.
    RerunContext context(SqlRepo& sql, const std::string& meeting_id = "m_retry") const {
        RerunContext out;
        out.meeting_id = meeting_id;
        out.meeting = sql.repo->get_meeting(meeting_id).value();
        out.repository = sql.repo;
        out.store = fv::meeting_state::open_store(sql.repo, out.meeting, false, Json::object(),
                                                  test_clock());
        out.state = out.store->snapshot();
        out.segments = sql.repo->get_segments(meeting_id, -1.0, std::nullopt);
        return out;
    }

    std::vector<Core::Pass> passes(bool polish) const {
        std::vector<Core::Pass> out;
        for (const Core::Pass& pass : core->passes()) {
            if (pass.payload.is_polish == polish) {
                out.push_back(pass);
            }
        }
        return out;
    }

    std::shared_ptr<Core> core = std::make_shared<Core>();
    Json ops = Json::array();
    bool fail_polish = false;
    std::optional<int> fail_polish_after;
    // The consolidation pass throws this.
    std::string raises;
    // The factory throws this instead of making a core.
    std::string creation_error;
    std::atomic<int> polish_calls{0};
    mutable std::mutex mutex;
    std::vector<std::string> kinds;
    std::shared_ptr<MeetingIntelligence> provider;
};

std::vector<Json> live_items(const Json& state, const std::string& card) {
    std::vector<Json> out;
    for (const Json& item : state.at("cards").at(card)) {
        if (item.value("status", std::string()) != "removed") {
            out.push_back(item);
        }
    }
    return out;
}

} // namespace

// ---- tests/test_meeting_refinalize.py::TestStepSelection ----

TEST_CASE("TestStepSelection::test_consolidation_retry_does_not_redecode",
          "[test_meeting_refinalize]") {
    SqlRepo sql;
    make_meeting(sql, seeded_state("m_retry", default_steps()));
    add_transcript(sql);
    Agent agent;
    agent.ops = Json::array({Json{{"op", "add_item"},
                                  {"card", "key_points"},
                                  {"text", "Budget review ships Friday"},
                                  {"evidence", Json::array({"sg_1"})}}});
    std::atomic<int> redecodes{0};
    RerunRequest request = agent.request(sql, "consolidation");
    request.transcribe = [&redecodes](const std::filesystem::path&, const std::vector<Json>&,
                                      const OfflineProgress&) {
        ++redecodes;
        return std::vector<TranscriptSegment>{};
    };

    const Json result = rerun_finalization("m_retry", request);

    CHECK(redecodes.load() == 0);
    std::set<std::string> ids;
    for (const Json& row : sql.repo->get_segments("m_retry", -1.0, std::nullopt)) {
        ids.insert(row.at("id").get<std::string>());
    }
    CHECK(ids.contains("sg_1"));
    CHECK(step_of(result, "redecode").at("status") == "failed");
    CHECK(step_of(result, "consolidation").at("status") == "completed");
    CHECK(step_of(result, "consolidation").at("detail") == "Summary & action items ready");
    // Overall stays failed until the leftover redecode is retried.
    CHECK(result.at("ok") == false);
    // The polish step was not asked for either.
    CHECK(agent.passes(true).empty());
    REQUIRE(agent.passes(false).size() == 1);
    CHECK(live_items(result.at("state"), "key_points").at(0).at("text") ==
          "Budget review ships Friday");
}

TEST_CASE("TestStepSelection::test_from_redecode_runs_dependents", "[test_meeting_refinalize]") {
    SqlRepo sql;
    make_meeting(sql, seeded_state("m_retry", default_steps()));
    add_transcript(sql);
    Agent agent;
    RerunRequest request = agent.request(sql, "redecode");
    request.transcribe = rich_decode;

    const Json result = rerun_finalization("m_retry", request);

    const auto polished = agent.passes(true);
    const auto consolidated = agent.passes(false);
    REQUIRE_FALSE(polished.empty());
    REQUIRE(consolidated.size() == 1);
    CHECK(consolidated.front().payload.is_consolidation);
    CHECK(consolidated.front().via_consolidate);
    // Both passes read the re-decoded transcript.
    CHECK(polished.front().payload.new_segments.front().at("id") == "sg_new_1");
    CHECK(consolidated.front().payload.new_segments.size() == 2);
    for (const char* id : {"redecode", "polish", "consolidation", "finalize"}) {
        CAPTURE(id);
        CHECK(step_of(result, id).at("status") == "completed");
    }
    CHECK(result.at("ok") == true);
}

TEST_CASE("TestStepSelection::test_polish_failure_is_recorded", "[test_meeting_refinalize]") {
    SqlRepo sql;
    make_meeting(
        sql, seeded_state("m_retry",
                          Json::array({step("polish", "Transcript Cleanup", "failed"),
                                       step("consolidation", "Summary & Action Items", "pending"),
                                       step("finalize", "State Finalization", "pending")})));
    add_transcript(sql);
    Agent agent;
    agent.fail_polish = true;

    const Json result = rerun_finalization("m_retry", agent.request(sql, "polish"));

    const Json& polish = step_of(result, "polish");
    CHECK(polish.at("status") == "failed");
    const auto polished = agent.passes(true);
    REQUIRE(polished.size() == 1);
    CHECK(polish.at("detail") ==
          "polish failed (block 1/1; request ID: " + polished.front().payload.request_id + ")");
    CHECK(result.at("finalization").at("status") == "failed");
    // The report is still written from the transcript as it stands.
    CHECK(agent.passes(false).size() == 1);
    CHECK(step_of(result, "consolidation").at("status") == "completed");
}

TEST_CASE("TestStepSelection::test_polish_retry_bounds_requests_and_covers_the_recording",
          "[test_meeting_refinalize]") {
    const auto [count, text] = GENERATE(std::tuple<int, std::string>{435, "short line"},
                                        std::tuple<int, std::string>{30, std::string(1'000, 'x')});
    CAPTURE(count);
    SqlRepo sql;
    make_meeting(sql);
    std::vector<TranscriptSegment> rows;
    for (int i = 0; i < count; ++i) {
        rows.push_back(segment("sg_" + std::to_string(i), "mic", i, i + 1, text));
    }
    sql.repo->add_segments(rows);
    Agent agent;
    std::vector<std::pair<int, int>> progress;
    RerunContext context = agent.context(sql);
    context.progress = [&progress](const std::string&, int current, int total) {
        progress.emplace_back(current, total);
    };

    const Json result = agent.provider->polish(context, fv::CancelToken());

    CHECK(result.at("status") == "completed");
    CHECK(result.at("message") == "Transcript cleanup finished");
    const auto payloads = agent.passes(true);
    REQUIRE(payloads.size() > 1);
    CHECK(agent.passes(false).empty());
    std::vector<Json> seen;
    std::set<std::string> seen_ids;
    std::set<std::string> request_ids;
    for (const Core::Pass& pass : payloads) {
        const CheckpointPayload& payload = pass.payload;
        CHECK(payload.is_polish);
        CHECK_FALSE(payload.is_consolidation);
        CHECK(payload.new_segments.size() <= kPolishMaxSegments);
        std::size_t chars = 0;
        for (const Json& row : payload.new_segments) {
            chars += row.at("text").get<std::string>().size();
            // Blocks overlap backwards; the first sight of a row keeps its place.
            if (seen_ids.insert(row.at("id").get<std::string>()).second) {
                seen.push_back(row);
            }
        }
        CHECK(chars <= kPolishMaxTextChars);
        request_ids.insert(payload.request_id);
    }
    CHECK(seen == sql.repo->get_segments("m_retry", -1.0, std::nullopt));
    CHECK(request_ids.size() == payloads.size());
    std::vector<std::pair<int, int>> expected;
    for (int i = 1; i <= static_cast<int>(payloads.size()); ++i) {
        expected.emplace_back(i, static_cast<int>(payloads.size()));
    }
    CHECK(progress == expected);
    CHECK(agent.core->shutdown_calls() == 1);
}

TEST_CASE("TestStepSelection::test_later_polish_block_failure_is_not_reported_as_success",
          "[test_meeting_refinalize]") {
    SqlRepo sql;
    make_meeting(sql, seeded_state("m_retry", default_steps()));
    std::vector<TranscriptSegment> rows;
    for (int i = 0; i < 401; ++i) {
        rows.push_back(
            segment("sg_" + std::to_string(i), "mic", i, i + 1, "segment " + std::to_string(i)));
    }
    sql.repo->add_segments(rows);
    Agent agent;
    agent.fail_polish_after = 1;
    const RerunContext context = agent.context(sql);

    const Json result = agent.provider->polish(context, fv::CancelToken());

    const auto payloads = agent.passes(true);
    REQUIRE(payloads.size() == 2);
    CHECK(result.at("status") == "failed");
    // The same detail live End reports: which block, and its request id.
    const std::size_t total = polish_blocks(context.segments).size();
    CHECK(result.at("message") == "polish failed (block 2/" + std::to_string(total) +
                                      "; request ID: " + payloads[1].payload.request_id + ")");
    CHECK(agent.core->shutdown_calls() == 1);
}

// Every log line written while it lives, as caplog.text.
class CapturedLog {
public:
    CapturedLog() : sink_(std::make_shared<Sink>()) { fv::log::add_sink(sink_); }
    ~CapturedLog() { fv::log::remove_sink(sink_); }
    CapturedLog(const CapturedLog&) = delete;
    CapturedLog& operator=(const CapturedLog&) = delete;
    std::string text() const {
        const std::lock_guard lock(sink_->mutex);
        return sink_->text;
    }

private:
    struct Sink final : fv::log::Sink {
        void write(const fv::log::Record& record) override {
            const std::lock_guard lock(mutex);
            text.append(record.message);
            text.push_back('\n');
        }
        mutable std::mutex mutex;
        std::string text;
    };
    std::shared_ptr<Sink> sink_;
};

TEST_CASE("TestFinalPolish::test_later_block_failure_keeps_prior_work_but_reports_failure",
          "[test_meeting_scheduler]") {
    // from tests/test_meeting_scheduler.py: End's polish runs the blocks through the provider
    // (MeetingIntelligence::polish, finalization.py polish_transcript), as here.
    const bool timeout = GENERATE(false, true);
    CAPTURE(timeout);
    SqlRepo sql;
    make_meeting(sql, seeded_state("m_retry", default_steps()));
    std::vector<TranscriptSegment> rows;
    for (std::size_t i = 0; i <= kPolishMaxSegments; ++i) {
        const auto at = static_cast<double>(i);
        rows.push_back(segment("sg_" + std::to_string(i), "mic", at, at + 1, "draft"));
    }
    sql.repo->add_segments(rows);
    Agent agent;
    std::atomic<int> calls{0};
    agent.core->set_hook(
        [&](const CheckpointPayload& payload, fv::meeting_agent::AgentToolHost* tools) {
            AgentResult result;
            if (++calls == 1) {
                REQUIRE(tools != nullptr);
                const auto applied = tools->apply_agent_ops(Json::array(
                    {Json{{"op", "revise_segment_text"},
                          {"segment_id", payload.new_segments.front().at("id")},
                          {"text", "Cleaned."},
                          {"evidence", Json::array({payload.new_segments.front().at("id")})}}}));
                result.ok = applied.size() == 1 && applied[0].ok;
                return result;
            }
            if (timeout) {
                // Outlasts the block's budget: waits for the watchdog's cancel (2 s at most).
                for (int i = 0; i < 200 && agent.core->cancel_calls() == 0; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            result.error = "second block failed";
            return result;
        });
    RerunContext context = agent.context(sql);
    if (timeout) {
        // Long enough for the first block's write under load; the second outlasts it.
        context.options.timeout_s = 1.0;
    }
    CapturedLog log;

    const Json result = agent.provider->polish(context, fv::CancelToken());

    CHECK(result.at("status") == "failed");
    const std::string message = result.at("message").get<std::string>();
    CHECK_THAT(message, ContainsSubstring(timeout ? "timed out" : "second block failed"));
    const auto payloads = agent.passes(true);
    REQUIRE(payloads.size() == 2);
    REQUIRE(calls == 2);
    const std::string request_id = payloads[1].payload.request_id;
    CHECK_THAT(message, ContainsSubstring(request_id));
    CHECK_THAT(message, ContainsSubstring("block 2/2"));
    CHECK_THAT(log.text(), ContainsSubstring(request_id));
    CHECK_THAT(log.text(), ContainsSubstring("block=2/2"));
    // The first block's cleanup is kept although the pass failed.
    const auto stored = sql.repo->get_segments("m_retry", -1.0, std::nullopt);
    REQUIRE_FALSE(stored.empty());
    CHECK(stored.front().at("id") == "sg_0");
    CHECK(stored.front().at("text") == "Cleaned.");
}

// ---- TestProtection ----

TEST_CASE("TestProtection::test_human_pinned_cards_survive", "[test_meeting_refinalize]") {
    SqlRepo sql;
    MeetingState state;
    state.meeting_id = "m_retry";
    state.seq = 4;
    state.title = "Budget sync";
    state.cloud_enabled = true;
    state.finalization.status = "failed";
    state.finalization.message = "needs retry";
    state.finalization.steps =
        Json::array({step("consolidation", "Summary & Action Items", "failed"),
                     step("finalize", "State Finalization", "pending")});
    state.finalization.total_steps = 2;
    CardItem human;
    human.id = "it_human";
    human.card = "key_points";
    human.text = "Human wrote this";
    human.status = "edited";
    human.author_type = "user";
    state.cards["key_points"].push_back(human);
    make_meeting(sql, fv::py::dumps(state.to_json()));
    add_transcript(sql);
    Agent agent;
    agent.ops = Json::array({Json{{"op", "update_item"},
                                  {"id", "it_human"},
                                  {"base_revision", 1},
                                  {"set", {{"text", "Agent overwrite"}}}}});

    const Json result = rerun_finalization("m_retry", agent.request(sql, "consolidation"));

    REQUIRE(agent.passes(false).size() == 1);
    Json found;
    for (const Json& item : result.at("state").at("cards").at("key_points")) {
        if (item.at("id") == "it_human") {
            found = item;
        }
    }
    REQUIRE_FALSE(found.is_null());
    CHECK(found.at("text") == "Human wrote this");
    CHECK(found.at("status") == "edited");
}

// ---- TestEndpointSnapshot ----

TEST_CASE("TestEndpointSnapshot::test_stored_snapshot_is_passed_to_agent",
          "[test_meeting_refinalize]") {
    const Json endpoint{{"profile_id", "custom_abcd1234"},
                        {"name", "LM Studio"},
                        {"kind", "custom"},
                        {"base_url", "http://127.0.0.1:1234/v1"},
                        {"api_key_env", ""}};
    SqlRepo sql;
    make_meeting(sql, seeded_state("m_retry", default_steps()),
                 Json{{"agent_endpoint_json", fv::py::dumps(endpoint)},
                      {"agent_provider", "custom_abcd1234"},
                      {"agent_model", "local-qwen"}});
    add_transcript(sql);
    Agent agent;

    rerun_finalization("m_retry", agent.request(sql, "consolidation"));

    const auto config = agent.core->config();
    REQUIRE(config);
    CHECK(config->endpoint.at("base_url") == endpoint.at("base_url"));
    CHECK(config->endpoint.at("profile_id") == "custom_abcd1234");
    CHECK(config->provider == "custom_abcd1234");
    CHECK(config->model == "local-qwen");
}

TEST_CASE("TestEndpointSnapshot::test_old_row_reconstructs_builtin_endpoint",
          "[test_meeting_refinalize]") {
    // Python asserts meeting.stored.meeting_endpoint; here the row goes the whole way: a meeting
    // recorded before endpoint snapshots existed hands its re-run's agent the built-in profile.
    SqlRepo sql;
    make_meeting(sql, seeded_state("m_retry", default_steps()), Json{{"agent_provider", "openai"}});
    add_transcript(sql);
    Agent agent;

    rerun_finalization("m_retry", agent.request(sql, "consolidation"));

    const auto config = agent.core->config();
    REQUIRE(config);
    CHECK(config->provider == "openai");
    CHECK(config->endpoint.at("profile_id") == "openai");
    CHECK(config->endpoint.at("kind") == "openai");
}

// ---- tests/test_meeting_reinsight.py ----

TEST_CASE("TestGuards::test_unknown_meeting_rejected", "[test_meeting_reinsight]") {
    SqlRepo sql;
    Agent agent;
    try {
        rerun_finalization("m_nope", agent.request(sql, "consolidation"));
        FAIL("an unknown meeting was re-run");
    } catch (const fv::py::ValueError& error) {
        CHECK_THAT(error.what(), ContainsSubstring("unknown meeting"));
    }
    CHECK(agent.kinds.empty());
}

TEST_CASE("TestGuards::test_meeting_without_transcript_rejected", "[test_meeting_reinsight]") {
    // Python's rerun_insights raises ValueError("meeting has no transcript"), which the retry
    // records on the step; no agent is built for a meeting with nothing to read.
    SqlRepo sql;
    make_meeting(sql);
    Agent agent;

    const Json result = rerun_finalization("m_retry", agent.request(sql, "consolidation"));

    CHECK(result.at("ok") == false);
    CHECK(result.at("error") == "meeting has no transcript");
    CHECK(step_of(result, "consolidation").at("status") == "failed");
    CHECK(step_of(result, "consolidation").at("detail") == "meeting has no transcript");
    CHECK(agent.kinds.empty());
    CHECK(agent.core->initialize_calls() == 0);
}

TEST_CASE("TestHappyPath::test_ops_land_and_persist", "[test_meeting_reinsight]") {
    SqlRepo sql;
    make_meeting(sql);
    add_transcript(sql);
    Agent agent;
    agent.ops = Json::array({
        Json{{"op", "add_item"},
             {"card", "key_points"},
             {"text", "Budget review ships Friday"},
             {"evidence", Json::array({"sg_1"})}},
        Json{{"op", "set_topic"}, {"text", "Budget review"}, {"evidence", Json::array({"sg_2"})}},
        // Rejected: evidence validation must behave as it does live.
        Json{{"op", "add_item"},
             {"card", "decisions"},
             {"text", "Bogus"},
             {"evidence", Json::array({"sg_missing"})}},
    });

    const Json result = rerun_finalization("m_retry", agent.request(sql, "consolidation"));

    CHECK(result.at("ok") == true);
    CHECK(result.at("error").is_null());
    // 2 agent ops + timeline backfill + summary backfill.
    CHECK(result.at("applied") == 4);
    // The built-in agent, made once (OpenWhisper also passes its Pi payload folder).
    CHECK(agent.kinds == std::vector<std::string>{"pi"});

    const Json& state = result.at("state");
    CHECK(state.at("topic").at("current") == "Budget review");
    const Json& key_points = state.at("cards").at("key_points");
    REQUIRE(key_points.size() == 1);
    CHECK(key_points[0].at("text") == "Budget review ships Friday");
    CHECK(key_points[0].at("status") == "proposed");
    CHECK(key_points[0].at("author_type") == "agent");
    CHECK(state.at("cards").at("decisions").empty());
    const auto timeline = live_items(state, "timeline");
    REQUIRE(timeline.size() == 1);
    CHECK(timeline[0].at("data").at("start_s") == 0.0);
    CHECK(timeline[0].at("author_type") == "system");

    // Write-through persistence, exactly as in a live meeting.
    const Json meeting = sql.repo->get_meeting("m_retry").value();
    const Json stored = fv::py::loads(meeting.at("state_json").get<std::string>());
    CHECK(stored.at("topic").at("current") == "Budget review");
    CHECK(meeting.at("state_seq") == stored.at("seq"));
    CHECK(stored.at("seq") == state.at("seq"));

    CHECK(agent.core->shutdown_calls() == 1);
}

TEST_CASE("TestHappyPath::test_agent_receives_full_transcript_and_prompt",
          "[test_meeting_reinsight]") {
    SqlRepo sql;
    make_meeting(sql);
    add_transcript(sql);
    Agent agent;

    rerun_finalization("m_retry", agent.request(sql, "consolidation"));

    const auto passes = agent.core->passes();
    REQUIRE(passes.size() == 1);
    const CheckpointPayload& payload = passes.front().payload;
    CHECK(payload.is_consolidation);
    REQUIRE(payload.new_segments.size() == 2);
    CHECK(payload.new_segments[0].at("id") == "sg_1");
    CHECK(payload.new_segments[1].at("id") == "sg_2");
    CHECK_FALSE(payload.request_id.empty());
    const auto config = agent.core->config();
    REQUIRE(config);
    CHECK(config->meeting_id == "m_retry");
    CHECK(config->provider == "openrouter");
    CHECK(config->model == "test/model");
    // An installed agent runs under this charter alone, so every core is handed it.
    CHECK_THAT(config->system_prompt, ContainsSubstring("meeting-intelligence copilot"));
}

TEST_CASE("TestHappyPath::test_existing_state_is_preserved_and_protected",
          "[test_meeting_reinsight]") {
    // Human-touched items survive a re-run, as they do live.
    SqlRepo sql;
    MeetingState state;
    state.meeting_id = "m_retry";
    state.seq = 4;
    state.title = "Budget sync";
    state.status = "ended";
    state.cloud_enabled = true;
    CardItem human;
    human.id = "it_human";
    human.card = "key_points";
    human.text = "Human wrote this";
    human.status = "edited";
    human.author_type = "user";
    state.cards["key_points"].push_back(human);
    make_meeting(sql, fv::py::dumps(state.to_json()));
    add_transcript(sql);
    Agent agent;
    agent.ops = Json::array({Json{{"op", "update_item"},
                                  {"id", "it_human"},
                                  {"base_revision", 1},
                                  {"set", {{"text", "Agent overwrite"}}}}});

    const Json result = rerun_finalization("m_retry", agent.request(sql, "consolidation"));

    // Agent update rejected; summary and topic can use the existing human text.
    CHECK(result.at("applied") == 2);
    Json found;
    for (const Json& item : result.at("state").at("cards").at("key_points")) {
        if (item.at("id") == "it_human") {
            found = item;
        }
    }
    REQUIRE_FALSE(found.is_null());
    CHECK(found.at("text") == "Human wrote this");
    CHECK(found.at("status") == "edited");
    // No evidence on the human note: do not borrow unrelated speech.
    CHECK(live_items(result.at("state"), "timeline").empty());
}

TEST_CASE("TestFailure::test_agent_error_is_reported_not_raised", "[test_meeting_reinsight]") {
    SqlRepo sql;
    make_meeting(sql);
    add_transcript(sql);
    Agent agent;
    agent.raises = "model exploded";

    const Json result = rerun_finalization("m_retry", agent.request(sql, "consolidation"));

    CHECK(result.at("ok") == false);
    CHECK_THAT(result.at("error").get<std::string>(), ContainsSubstring("model exploded"));
    CHECK(step_of(result, "consolidation").at("status") == "failed");
    // Failure leaves the transcript intact without manufacturing insights.
    CHECK(result.at("applied") == 0);
    for (const auto& [card, items] : result.at("state").at("cards").items()) {
        CAPTURE(card);
        CHECK(items.empty());
    }
    CHECK(sql.repo->get_segments("m_retry", -1.0, std::nullopt).size() == 2);
    CHECK(result.at("state").at("meeting_id") == "m_retry");
    CHECK(agent.core->shutdown_calls() == 1);
}

TEST_CASE("TestFailure::test_core_creation_failure_is_reported", "[test_meeting_reinsight]") {
    SqlRepo sql;
    make_meeting(sql);
    add_transcript(sql);
    Agent agent;
    agent.creation_error = "no core";

    const Json result = rerun_finalization("m_retry", agent.request(sql, "consolidation"));

    CHECK(result.at("ok") == false);
    CHECK_THAT(result.at("error").get<std::string>(), ContainsSubstring("no core"));
    CHECK(result.at("applied") == 0);
    CHECK(agent.core->initialize_calls() == 0);
}

TEST_CASE("TestFailure::test_core_that_fails_to_start_is_still_shut_down",
          "[test_meeting_reinsight]") {
    // A half-started agent must not outlive the request.
    const bool polish = GENERATE(false, true);
    CAPTURE(polish);
    SqlRepo sql;
    make_meeting(sql);
    add_transcript(sql);
    Agent agent;
    agent.core->fail_initialize("sidecar handshake failed");
    const RerunContext context = agent.context(sql);

    const Json result = polish ? agent.provider->polish(context, fv::CancelToken())
                               : agent.provider->consolidate(context, fv::CancelToken());

    CHECK(result.at("status") == "failed");
    CHECK_THAT(result.at("message").get<std::string>(), ContainsSubstring("handshake failed"));
    CHECK(agent.core->passes().empty());
    CHECK(agent.core->shutdown_calls() == 1);
}

// Audit A24-42 (lead decision): OpenWhisper's rerun_polish and rerun_insights run whatever the
// stored state's cloud_enabled says. In FioriVoce a re-run reaching AI steps needs fresh desktop
// approval naming the meeting and destination (a30-dashboard-fresh-approval); that approval, not
// the toggle recorded at the time, authorises it. Without one, the stored toggle still refuses.
TEST_CASE("a re-run of a meeting recorded with AI insights off runs only with fresh approval",
          "[test_meeting_reinsight]") {
    const std::string step_key = GENERATE("polish", "consolidation");
    const bool approved = GENERATE(false, true);
    MeetingState state;
    state.meeting_id = "m_retry";
    state.status = "ended";
    state.cloud_enabled = false;
    state.title = "Budget sync";
    state.finalization.status = "failed";
    state.finalization.steps = default_steps();
    state.finalization.total_steps = 4;
    SqlRepo sql;
    make_meeting(sql, fv::py::dumps(state.to_json()), Json{{"cloud_enabled", false}});
    add_transcript(sql);
    Agent agent;
    RerunRequest request = agent.request(sql, step_key);
    if (approved) {
        request.approved_route =
            fv::meeting_agent::MeetingAgentRoute{"pi", "openrouter", "test/model", Json()};
        request.saved_destination_allowed = [] {
            return true;
        };
    }

    const Json result = rerun_finalization("m_retry", request);

    const Json& entry = step_of(result, step_key);
    if (approved) {
        // A polish retry runs its dependent consolidation too, each on a throwaway core.
        CHECK(agent.kinds.size() == (step_key == "polish" ? 2u : 1u));
        CHECK(entry.at("status") == "completed");
        CHECK(agent.core->shutdown_calls() == static_cast<int>(agent.kinds.size()));
    } else {
        CHECK(agent.kinds.empty());
        CHECK(agent.core->passes().empty());
        CHECK_THAT(result.dump(), ContainsSubstring("AI insights are off for this meeting."));
    }
}
