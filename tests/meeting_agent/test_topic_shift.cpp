// The scheduler's semantic topic-shift judge overrides Jaccard and falls back cleanly
// (tests/test_scheduler_semantic_topic_shift.py), and the topic question's own budget on a slow
// text model (tests/test_text_judge.py::test_topic_shift_caps_a_slow_model).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "base/testing/fake_clock.h"
#include "base/testing/map_env_reader.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "http/testing/fake_http_client.h"
#include "llm/judge.h"
#include "llm/model_catalog.h"
#include "llm/openai_client.h"
#include "llm/testing/fake_judge.h"
#include "llm/text_judge.h"
#include "meeting_agent/scheduler.h"
#include "meeting_agent/typesafe_signals.h"
#include "pyport/json.h"
#include "scheduler_support.h"

using fv::Json;
using namespace fv::meeting_agent;
using namespace fv::meeting_agent::scheduler_test;
using P = SchedulerProbe;

namespace {

const std::vector<Json> kFinance{Json{{"start_s", 85.0},
                                      {"end_s", 95.0},
                                      {"text", "budget forecast revenue pipeline margin earnings"}},
                                 Json{{"start_s", 100.0},
                                      {"end_s", 115.0},
                                      {"text", "quarterly finance spreadsheet ledger accounting"}}};
const std::vector<Json> kHiring{
    Json{{"start_s", 145.0},
         {"end_s", 155.0},
         {"text", "hiring onboarding interview candidates staffing"}},
    Json{{"start_s", 165.0},
         {"end_s", 185.0},
         {"text", "recruitment culture retention engineers designers"}}};
const std::vector<Json> kFinanceAgain{
    Json{{"start_s", 145.0},
         {"end_s", 155.0},
         {"text", "budget forecast revenue pipeline margin earnings"}},
    Json{{"start_s", 165.0},
         {"end_s", 185.0},
         {"text", "quarterly finance spreadsheet ledger accounting"}}};

std::vector<Json> joined(std::vector<Json> a, const std::vector<Json>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// RecordingJudge: a scripted probability (or none), or an exception, and every window pair.
struct RecordingJudge {
    std::optional<double> result;
    bool raise = false;
    std::vector<std::pair<std::string, std::string>> calls;

    TopicJudge fn() {
        return [this](const std::string& older, const std::string& newer) -> std::optional<double> {
            calls.emplace_back(older, newer);
            if (raise) {
                throw std::runtime_error("boom");
            }
            return result;
        };
    }
};

// make() and detect() of the Python test: the shift check reads only the clock and the
// transcript, so there is no store.
struct Shift {
    fv::FakeClock clock;
    FakeSchedulerHost host;
    FakeAgent agent;
    std::unique_ptr<CheckpointScheduler> sched;

    Shift(const std::vector<Json>& segments, TopicJudge judge) : host(segments) {
        start_late(clock);
        host.set_store(nullptr);
        SchedulerOptions options;
        options.base_interval_s = 45.0;
        options.min_interval_s = 0.05;
        options.max_interval_s = 60.0;
        options.shift_check_spacing_s = 0.0;
        sched = std::make_unique<CheckpointScheduler>(host, agent.core, clock,
                                                      std::function<void(bool, std::string)>{},
                                                      std::move(judge), options);
        P::set_last_fire_mono(*sched, P::mono(*sched) - 5.0);
        P::set_last_shift_check_mono(*sched, 0.0);
    }
    bool detect() { return P::detect_topic_shift(*sched); }
};

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

// from tests/test_scheduler_semantic_topic_shift.py::test_semantic_yes_overrides_similar_words
TEST_CASE("test_semantic_yes_overrides_similar_words", "[test_scheduler_semantic_topic_shift]") {
    RecordingJudge judge{0.91};
    Shift shift(joined(kFinance, kFinanceAgain), judge.fn()); // Jaccard says "same topic"
    CHECK(shift.detect());
    REQUIRE(judge.calls.size() == 1);
    CHECK(contains(judge.calls[0].first, "budget forecast"));
    CHECK(contains(judge.calls[0].second, "budget forecast"));
}

// from tests/test_scheduler_semantic_topic_shift.py::test_semantic_no_overrides_disjoint_words
TEST_CASE("test_semantic_no_overrides_disjoint_words", "[test_scheduler_semantic_topic_shift]") {
    RecordingJudge judge{0.12};
    CHECK_FALSE(Shift(joined(kFinance, kHiring), judge.fn()).detect()); // Jaccard says "shifted"
}

// from tests/test_scheduler_semantic_topic_shift.py::test_threshold_boundary
TEST_CASE("test_threshold_boundary", "[test_scheduler_semantic_topic_shift]") {
    RecordingJudge at{0.5};
    CHECK(Shift(joined(kFinance, kFinanceAgain), at.fn()).detect());
    RecordingJudge below{0.49};
    CHECK_FALSE(Shift(joined(kFinance, kFinanceAgain), below.fn()).detect());
}

// from tests/test_scheduler_semantic_topic_shift.py::test_no_answer_falls_back_to_jaccard
TEST_CASE("test_no_answer_falls_back_to_jaccard", "[test_scheduler_semantic_topic_shift]") {
    RecordingJudge none;
    CHECK(Shift(joined(kFinance, kHiring), none.fn()).detect());
    CHECK_FALSE(Shift(joined(kFinance, kFinanceAgain), none.fn()).detect());
}

// from tests/test_scheduler_semantic_topic_shift.py::test_judge_exception_falls_back_to_jaccard
TEST_CASE("test_judge_exception_falls_back_to_jaccard", "[test_scheduler_semantic_topic_shift]") {
    RecordingJudge raising;
    raising.raise = true;
    CHECK(Shift(joined(kFinance, kHiring), raising.fn()).detect());
}

// from tests/test_scheduler_semantic_topic_shift.py::test_judge_not_asked_for_tiny_windows
TEST_CASE("test_judge_not_asked_for_tiny_windows", "[test_scheduler_semantic_topic_shift]") {
    RecordingJudge judge{0.99};
    const std::vector<Json> tiny{Json{{"start_s", 90.0}, {"end_s", 91.0}, {"text", "okay"}},
                                 Json{{"start_s", 150.0}, {"end_s", 151.0}, {"text", "sure"}}};
    CHECK_FALSE(Shift(tiny, judge.fn()).detect());
    CHECK(judge.calls.empty());
}

// from tests/test_scheduler_semantic_topic_shift.py::test_without_judge_behaviour_is_unchanged
TEST_CASE("test_without_judge_behaviour_is_unchanged", "[test_scheduler_semantic_topic_shift]") {
    CHECK(Shift(joined(kFinance, kHiring), {}).detect());
    CHECK_FALSE(Shift(joined(kFinance, kFinanceAgain), {}).detect());
}

TEST_CASE("the shift check keeps its spacing and needs the meeting clock", "[scheduler]") {
    fv::FakeClock clock;
    start_late(clock);
    FakeSchedulerHost host(joined(kFinance, kHiring));
    FakeAgent agent;
    CheckpointScheduler sched(host, agent.core, clock); // 10 s spacing
    P::set_last_shift_check_mono(sched, 0.0);
    CHECK(P::detect_topic_shift(sched));
    // Inside the spacing nothing is read.
    const auto reads = host.transcript_reads().size();
    CHECK_FALSE(P::detect_topic_shift(sched));
    CHECK(host.transcript_reads().size() == reads);
    clock.advance(std::chrono::seconds(10));
    CHECK(P::detect_topic_shift(sched));
    CHECK(host.transcript_reads().back() == 80.0);

    // Under two minutes of meeting there are no two windows to compare.
    host.set_clock_s(119.0);
    clock.advance(std::chrono::seconds(10));
    CHECK_FALSE(P::detect_topic_shift(sched));

    FakeSchedulerHost clockless(joined(kFinance, kHiring), {.meeting_clock = false});
    CheckpointScheduler without(clockless, agent.core, clock);
    P::set_last_shift_check_mono(without, 0.0);
    CHECK_FALSE(P::detect_topic_shift(without));
    CHECK(clockless.transcript_reads().empty());
}

// from tests/test_text_judge.py::test_topic_shift_caps_a_slow_model
TEST_CASE("test_topic_shift_caps_a_slow_model", "[test_text_judge]") {
    // A text model on a server the user runs, which answers 0.7; its model list is not served.
    fv::llm::forget_server_specs();
    auto http = std::make_shared<fv::http::FakeHttpClient>();
    std::mutex mutex;
    std::vector<double> timeouts;
    http->set_handler([&](const fv::http::Request& request) {
        fv::http::ScriptedResponse reply;
        if (request.url.find("/chat/completions") == std::string::npos) {
            reply.error = fv::http::Error{fv::http::ErrorKind::Connect, "refused"};
            return reply;
        }
        {
            std::lock_guard lock(mutex);
            timeouts.push_back(static_cast<double>(request.idle_timeout.count()) / 1000.0);
        }
        reply.headers = {{"Content-Type", "application/json"}};
        reply.body = fv::py::dumps(
            Json{{"id", "chatcmpl-1"},
                 {"object", "chat.completion"},
                 {"created", 0},
                 {"model", "judge-1"},
                 {"choices", Json::array({Json{{"index", 0},
                                               {"finish_reason", "stop"},
                                               {"message", Json{{"role", "assistant"},
                                                                {"content", R"({"q": 0.7})"}}}}})},
                 {"usage", Json{{"prompt_tokens", 12}, {"completion_tokens", 3}}}});
        return reply;
    });
    auto backend = std::make_shared<fv::MemoryCredentialBackend>();
    fv::CredentialStore store{backend, fv::identity()};
    fv::MapEnvReader env;
    fv::DotEnv dotenv{std::filesystem::path("fv-meeting-agent-tests-no-such-dir") / ".env"};
    fv::llm::JudgeHooks hooks;
    hooks.proxy_for = [](std::string_view) {
        return std::optional<std::string>();
    };
    hooks.make_text_client = [&](const fv::llm::TextLLMProfile& profile, double timeout_s,
                                 std::string_view key) {
        return fv::llm::create_openai_client(http, profile, timeout_s, key, {}, 0);
    };
    {
        fv::llm::JudgeEnvironment environment(http, store, env, dotenv, std::move(hooks));
        fv::llm::TextLLMProfile profile;
        profile.id = "custom_box";
        profile.name = "My server";
        profile.kind = "custom";
        profile.base_url = "http://127.0.0.1:9000/v1";
        fv::llm::JudgeRoute route;
        route.id = std::string(fv::llm::kJudgeProviderTextModel);
        route.name = profile.name;
        route.endpoint = *profile.base_url;
        route.model = "judge-1";
        route.timeout_s = fv::llm::kTextModelJudgeTimeoutS;
        route.profile = profile;
        fv::llm::TextModelJudge judge(std::string(fv::llm::kAuthFreeApiKey), route, environment);
        const auto probability = topic_shift_probability(judge, "budget talk", "hiring plans");
        REQUIRE(probability.has_value());
        CHECK(*probability == 0.7);
    }
    std::lock_guard lock(mutex);
    REQUIRE(timeouts.size() == 1);
    CHECK(timeouts[0] == Catch::Approx(TOPIC_TIMEOUT_S).margin(0.5));
    fv::llm::forget_server_specs();
}

TEST_CASE("a canceled meeting topic signal does not ask for a judgment", "[scheduler]") {
    fv::llm::FakeJudge judge;
    fv::CancelSource stop;
    stop.cancel();
    CHECK_FALSE(topic_shift_probability(judge, "budget talk", "hiring plans", stop.token()));
    CHECK(judge.calls().empty());
}
