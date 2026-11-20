// Golden vectors of the scheduler (meeting.agent.scheduler: content words, the Jaccard decision,
// the intervals, the outcomes and scripted run_due traces), the route
// (services.meeting_agent_route) and the TypeSafe questions (meeting.agent.typesafe_signals).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "golden/golden.h"
#include "golden_support.h"
#include "llm/testing/fake_judge.h"
#include "meeting_agent/detail/scheduler_support.h"
#include "meeting_agent/route.h"
#include "meeting_agent/scheduler.h"
#include "meeting_agent/tool_policy.h"
#include "meeting_agent/typesafe_signals.h"
#include "scheduler_support.h"

using fv::Json;
using namespace fv::meeting_agent;
using namespace fv::meeting_agent::scheduler_test;
using fv::meeting_agent_test::replay;
namespace golden = fv::golden;
using P = SchedulerProbe;

namespace {

SchedulerOptions intervals_from(const Json& intervals) {
    SchedulerOptions options;
    if (intervals.contains("base_interval_s")) {
        options.base_interval_s = intervals["base_interval_s"].get<double>();
    }
    if (intervals.contains("min_interval_s")) {
        options.min_interval_s = intervals["min_interval_s"].get<double>();
    }
    if (intervals.contains("max_interval_s")) {
        options.max_interval_s = intervals["max_interval_s"].get<double>();
    }
    return options;
}

// The trace engine: FakeSchedulerHost's transcript and ledger, with every acknowledgement and
// write revocation recorded in the trace in the order they happen.
class TraceHost final : public SchedulerHost {
public:
    TraceHost(bool ledger, std::function<void(Json)> record)
        : inner_({}, FakeSchedulerHost::Options{.ledger = ledger}), record_(std::move(record)) {}

    FakeSchedulerHost& inner() { return inner_; }

    std::shared_ptr<fv::meeting_state::MeetingStateStore> store() override {
        return inner_.store();
    }
    std::vector<Json> get_transcript(double after_start_s) override {
        return inner_.get_transcript(after_start_s);
    }
    double meeting_clock_s() const override { return inner_.meeting_clock_s(); }
    std::vector<Json> delivery_page(std::int64_t after_seq, int limit) override {
        return inner_.delivery_page(after_seq, limit);
    }
    std::int64_t delivery_cursor(std::string_view consumer) override {
        return inner_.delivery_cursor(consumer);
    }
    void advance_delivery_cursor(std::string_view consumer, std::int64_t seq) override {
        record_(Json::array({"ack", consumer, seq}));
        inner_.advance_delivery_cursor(consumer, seq);
    }
    std::int64_t delivery_high_water() override { return inner_.delivery_high_water(); }
    void revoke_agent_writes() override { record_(Json::array({"revoke"})); }
    bool has_meeting_clock() const override { return true; }
    bool has_delivery_ledger() const override { return inner_.has_delivery_ledger(); }

private:
    FakeSchedulerHost inner_;
    std::function<void(Json)> record_;
};

Json ids(const std::vector<Json>& rows) {
    Json out = Json::array();
    for (const auto& row : rows) {
        out.push_back(row.contains("id") ? row["id"] : Json());
    }
    return out;
}

Json run_trace(const Json& spec) {
    std::mutex mutex;
    Json trace = Json::array();
    const auto record = [&](Json entry) {
        std::lock_guard lock(mutex);
        trace.push_back(std::move(entry));
    };
    TraceHost host(spec.value("ledger", false), record);
    fv::FakeClock clock;
    clock.advance(std::chrono::seconds(1000));
    auto core = std::make_shared<FakeAgentCore>();
    std::deque<std::string> results;
    core->set_hook([&](const CheckpointPayload& payload, AgentToolHost*) {
        const Json& snapshot = payload.state_snapshot;
        Json context;
        if (snapshot.contains("recent_transcript_context")) {
            context = ids(snapshot["recent_transcript_context"].get<std::vector<Json>>());
        }
        record(Json::array({"pass", pass_kind_for(payload), ids(payload.new_segments), context,
                            snapshot.value("notes_review_requested", false),
                            snapshot.contains("note_adjustment_request")
                                ? snapshot["note_adjustment_request"]
                                : Json()}));
        std::string result = "ok";
        {
            std::lock_guard lock(mutex);
            if (!results.empty()) {
                result = results.front();
                results.pop_front();
            }
        }
        if (result == "ok") {
            return ok_with("set_topic");
        }
        if (result == "revise") {
            return ok_with("revise_segment_text");
        }
        return failed(result.substr(std::string("fail:").size()));
    });
    CheckpointScheduler sched(
        host, core, clock,
        [&](bool online, const std::string& error) {
            record(Json::array({"health", online, error}));
        },
        {}, intervals_from(spec.value("intervals", Json::object())));
    for (const auto& step : spec["steps"]) {
        const std::string op = step[0].get<std::string>();
        if (op == "add") {
            std::vector<Json> rows;
            for (const auto& r : step[1]) {
                const double start = r[1].get<double>();
                rows.push_back(
                    Json{{"id", r[0]}, {"start_s", r[1]}, {"end_s", start + 2.0}, {"text", r[2]}});
            }
            host.inner().add_segments(rows);
        } else if (op == "notify") {
            sched.notify_segments(step[1].get<int>());
        } else if (op == "guidance") {
            sched.notify_guidance();
        } else if (op == "advance") {
            clock.advance(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::duration<double>(step[1].get<double>())));
        } else if (op == "clock") {
            host.inner().set_clock_s(step[1].get<double>());
        } else if (op == "script") {
            std::lock_guard lock(mutex);
            for (const auto& r : step[1]) {
                results.push_back(r.get<std::string>());
            }
        } else if (op == "healthy") {
            core->set_healthy(step[1].get<bool>());
        } else if (op == "no_store") {
            host.inner().set_store(nullptr);
        } else if (op == "run_due") {
            sched.run_due();
        } else if (op == "seed") {
            sched.seed_sent_segments();
        } else if (op == "consolidate") {
            const auto outcome = sched.run_consolidation(step[1].get<double>());
            record(Json::array({"outcome", outcome.status, outcome.message}));
        } else if (op == "pending") {
            record(Json::array({"pending", P::pending_segments(sched)}));
        } else if (op == "online") {
            record(Json::array({"online", sched.online()}));
        } else {
            FAIL("unknown step " << op);
        }
    }
    std::lock_guard lock(mutex);
    return trace;
}

// The engine of jaccard_shift: the older texts from 85 s, the newer from 145 s, now 200 s.
bool jaccard_shift(const Json& older, const Json& newer) {
    std::vector<Json> rows;
    for (std::size_t i = 0; i < older.size(); ++i) {
        rows.push_back(Json{{"start_s", 85.0 + static_cast<double>(i)}, {"text", older[i]}});
    }
    for (std::size_t i = 0; i < newer.size(); ++i) {
        rows.push_back(Json{{"start_s", 145.0 + static_cast<double>(i)}, {"text", newer[i]}});
    }
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host(rows);
    host.set_store(nullptr);
    SchedulerOptions options;
    options.shift_check_spacing_s = 0.0;
    CheckpointScheduler sched(host, std::make_shared<FakeAgentCore>(), clock, {}, {}, options);
    P::set_last_shift_check_mono(sched, 0.0);
    return P::detect_topic_shift(sched);
}

// A FakeJudge's calls as the Python recording judge lists them.
Json judge_calls(const fv::llm::FakeJudge& judge) {
    Json calls = Json::array();
    for (const auto& call : judge.calls()) {
        const Json& question = call.questions["q"];
        calls.push_back(Json::array({question["type"], call.state, question["instructions"],
                                     question.contains("criteria") ? question["criteria"] : Json(),
                                     call.timeout_s ? Json(*call.timeout_s) : Json()}));
    }
    return calls;
}

} // namespace

TEST_CASE("golden meeting.agent.scheduler _content_words", "[golden]") {
    golden::for_each("meeting.agent.scheduler", "_content_words", [](const golden::Case& c) {
        replay(c, [&] {
            Json out = Json::array();
            for (const auto& word : content_words(c.arg<std::string>(0))) {
                out.push_back(word);
            }
            return out;
        });
    });
}

TEST_CASE("golden meeting.agent.scheduler jaccard_shift", "[golden]") {
    golden::for_each("meeting.agent.scheduler", "jaccard_shift", [](const golden::Case& c) {
        replay(c, [&] { return Json(jaccard_shift(c.args()[0], c.args()[1])); });
    });
}

TEST_CASE("golden meeting.agent.scheduler _interval_for", "[golden]") {
    golden::for_each("meeting.agent.scheduler", "_interval_for", [](const golden::Case& c) {
        replay(c, [&] {
            fv::FakeClock clock;
            FakeSchedulerHost host;
            CheckpointScheduler sched(host, std::make_shared<FakeAgentCore>(), clock, {}, {},
                                      intervals_from(c.args()[1]));
            return Json(P::interval_for(sched, c.arg<int>(0)));
        });
    });
}

TEST_CASE("golden meeting.agent.scheduler ConsolidationOutcome", "[golden]") {
    golden::for_each("meeting.agent.scheduler", "ConsolidationOutcome", [](const golden::Case& c) {
        replay(c, [&] {
            return ConsolidationOutcome{c.arg<std::string>(0), c.arg<std::string>(1)}.to_json();
        });
    });
}

TEST_CASE("golden meeting.agent.scheduler _consolidation_outcome", "[golden]") {
    golden::for_each(
        "meeting.agent.scheduler", "_consolidation_outcome", [](const golden::Case& c) {
            replay(c, [&] {
                const Json& spec = c.args()[0];
                std::optional<AgentResult> result;
                if (spec.is_array()) {
                    AgentResult value;
                    if (spec[0] == "ok") {
                        value = AgentResult{};
                        value.ok = true;
                        for (int i = 0; i < spec[1].get<int>(); ++i) {
                            value.op_results.push_back(ok_with("set_topic").op_results[0]);
                        }
                    } else {
                        value.error = spec[1].get<std::string>();
                    }
                    result = value;
                }
                return detail::consolidation_outcome(result, c.arg<double>(1)).to_json();
            });
        });
}

TEST_CASE("golden meeting.agent.scheduler trace", "[golden]") {
    golden::for_each("meeting.agent.scheduler", "trace", [](const golden::Case& c) {
        replay(c, [&] { return run_trace(c.args()[0]); });
    });
}

TEST_CASE("golden services.meeting_agent_route resolve_meeting_agent_route", "[golden]") {
    golden::for_each("services.meeting_agent_route", "resolve_meeting_agent_route",
                     [](const golden::Case& c) {
                         replay(c, [&] {
                             std::optional<Json> meeting;
                             if (!c.args()[1].is_null()) {
                                 meeting.emplace(c.args()[1]);
                             }
                             const auto route = resolve_meeting_agent_route(c.args()[0], meeting);
                             return Json{{"kind", route.kind},
                                         {"provider", route.provider},
                                         {"model", route.model},
                                         {"endpoint", route.endpoint},
                                         {"installed", route.installed()}};
                         });
                     });
}

TEST_CASE("golden meeting.agent.typesafe_signals constants", "[golden]") {
    golden::for_each("meeting.agent.typesafe_signals", "constants", [](const golden::Case& c) {
        replay(c, [] {
            return Json{{"TOPIC_CHANGED_INSTRUCTIONS", std::string(TOPIC_CHANGED_INSTRUCTIONS)},
                        {"TOPIC_TIMEOUT_S", TOPIC_TIMEOUT_S},
                        {"_WINDOW_MAX_CHARS", TOPIC_WINDOW_MAX_CHARS},
                        {"VOICE_COMMAND_CONFIDENCE", VOICE_COMMAND_CONFIDENCE},
                        {"VOICE_COMMAND_CRITERIA", voice_command_criteria()},
                        {"VOICE_COMMAND_INSTRUCTIONS", std::string(VOICE_COMMAND_INSTRUCTIONS)}};
        });
    });
}

TEST_CASE("golden meeting.agent.typesafe_signals topic_shift_probability", "[golden]") {
    golden::for_each(
        "meeting.agent.typesafe_signals", "topic_shift_probability", [](const golden::Case& c) {
            replay(c, [&] {
                fv::llm::FakeJudge judge;
                const Json& answer = c.args()[2];
                judge.push(answer.is_null() ? std::nullopt
                                            : std::optional<Json>(fv::llm::FakeJudge::noul_answer(
                                                  answer.get<double>())));
                const auto result =
                    topic_shift_probability(judge, c.arg<std::string>(0), c.arg<std::string>(1));
                return Json::array({result ? Json(*result) : Json(), judge_calls(judge)});
            });
        });
}

TEST_CASE("golden meeting.agent.typesafe_signals voice_command_choice", "[golden]") {
    golden::for_each(
        "meeting.agent.typesafe_signals", "voice_command_choice", [](const golden::Case& c) {
            replay(c, [&] {
                fv::llm::FakeJudge judge;
                const Json& answer = c.args()[3];
                judge.push(answer.is_null()
                               ? std::nullopt
                               : std::optional<Json>(fv::llm::FakeJudge::choice_answer(
                                     answer[0].get<std::string>(), answer[1].get<double>())));
                const auto result = voice_command_choice(judge, c.arg<std::string>(0),
                                                         c.arg<std::vector<std::string>>(1),
                                                         c.arg<std::vector<std::string>>(2));
                Json out = Json();
                if (result) {
                    out = Json{{"choice", result->choice},
                               {"confidence", result->confidence},
                               {"probabilities", result->probabilities}};
                }
                return Json::array({out, judge_calls(judge)});
            });
        });
}
