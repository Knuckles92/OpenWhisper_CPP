// The meeting features of tests/test_text_judge.py that ask the user's own text model: live
// highlights, a citation check, a voice command and an insight review, each against a
// TextModelJudge on a profile at 127.0.0.1 whose OpenAI-compatible server is an
// http::FakeHttpClient (OpenWhisper's FakeOpenAI). Nothing leaves the process.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "../llm/llm_test_support.h"
#include "base/identity.h"
#include "llm/judge.h"
#include "llm/model_catalog.h"
#include "llm/openai_client.h"
#include "llm/profiles.h"
#include "llm/text_judge.h"
#include "meeting_intel/citation_verifier.h"
#include "meeting_intel/insight_review.h"
#include "meeting_intel/intel_settings.h"
#include "meeting_intel/live_highlights.h"
#include "meeting_intel/voice_commands.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "pyport/json.h"
#include "rows_repo.h"

namespace {

using namespace fv;
using namespace fv::llm;
using Catch::Approx;
using fv::meeting_intel::test::eventually;
using fv::meeting_intel::test::RowsRepo;

// LOCAL: a text model on this computer.
TextLLMProfile local_profile() {
    TextLLMProfile profile;
    profile.id = "custom_box";
    profile.name = "My server";
    profile.kind = "custom";
    profile.base_url = "http://127.0.0.1:9000/v1";
    return profile;
}

JudgeRoute route_for(const TextLLMProfile& profile) {
    JudgeRoute route;
    route.id = std::string(kJudgeProviderTextModel);
    route.name = profile.name;
    route.endpoint = profile.base_url.value_or("");
    route.model = "judge-1";
    route.credential_env = profile.api_key_env;
    route.timeout_s = kTextModelJudgeTimeoutS;
    route.profile = profile;
    return route;
}

Json body_of(const http::Request& request) {
    const auto* text = std::get_if<std::string>(&request.body);
    return text && !text->empty() ? py::loads(*text) : Json();
}

// The JSON payload the model was given.
Json asked(const Json& request) {
    return py::loads(request.at("messages").back().at("content").get<std::string>());
}

// A model that answers every question: yes/no with noul(name), options with the label of
// choice(name, options) at probability 0.95.
using Noul = std::function<double(const std::string&)>;
using Choose = std::function<std::string(const std::string&, const Json&)>;

std::string number_of(const Json& options, const std::string& name) {
    for (const auto& [label, option] : options.items()) {
        if (option.at("name") == name) {
            return label;
        }
    }
    FAIL("no option " << name);
    return {};
}

struct LocalModel {
    LocalModel() {
        forget_server_specs();
        http->set_handler([this](const http::Request& request) {
            if (!test::url_path(request.url).ends_with("/chat/completions")) {
                http::ScriptedResponse refused;
                refused.error = http::Error{http::ErrorKind::Connect, "connection refused"};
                return refused;
            }
            const Json body = body_of(request);
            Json out = Json::object();
            const Json questions = asked(body).at("questions");
            for (const auto& [name, question] : questions.items()) {
                if (question.at("answer") == "yes/no") {
                    out[name] = noul ? noul(name) : 0.0;
                } else {
                    out[name] = Json{{"option", choose(name, question.at("options"))},
                                     {"probability", 0.95}};
                }
            }
            {
                std::lock_guard lock(mutex);
                requests.push_back(body);
            }
            return test::json_reply(
                200, Json{{"id", "chatcmpl-1"},
                          {"object", "chat.completion"},
                          {"created", 0},
                          {"model", "judge-1"},
                          {"choices",
                           Json::array({Json{{"index", 0},
                                             {"finish_reason", "stop"},
                                             {"message", Json{{"role", "assistant"},
                                                              {"content", py::dumps(out)}}}}})},
                          {"usage", Json{{"prompt_tokens", 123}, {"completion_tokens", 5}}}});
        });
        JudgeHooks hooks;
        hooks.proxy_for = [](std::string_view) {
            return std::optional<std::string>();
        };
        hooks.make_text_client = [this](const TextLLMProfile& profile, double timeout_s,
                                        std::string_view key) {
            return create_openai_client(http, profile, timeout_s, key, {}, 0);
        };
        env = std::make_unique<JudgeEnvironment>(http, keys.store, keys.env, keys.dotenv,
                                                 std::move(hooks));
    }
    ~LocalModel() {
        env.reset();
        forget_server_specs();
    }

    std::shared_ptr<Judge> judge() {
        return std::make_shared<TextModelJudge>(std::string(kAuthFreeApiKey),
                                                route_for(local_profile()), *env);
    }
    std::size_t sent() const {
        std::lock_guard lock(mutex);
        return requests.size();
    }

    Noul noul;
    Choose choose;
    std::shared_ptr<http::FakeHttpClient> http = std::make_shared<http::FakeHttpClient>();
    test::TestKeys keys;
    std::unique_ptr<JudgeEnvironment> env;
    mutable std::mutex mutex;
    std::vector<Json> requests;
};

std::shared_ptr<meeting_state::MeetingStateStore> store(const std::string& id) {
    meeting_state::MeetingState state;
    state.meeting_id = id;
    state.cloud_enabled = false; // AI insights are off: the judge is on this computer
    return std::make_shared<meeting_state::MeetingStateStore>(std::move(state));
}

TEST_CASE("live highlights from a local model", "[test_text_judge]") {
    // from tests/test_text_judge.py::test_live_highlights_from_a_local_model
    LocalModel model;
    model.noul = [](const std::string& name) {
        return name == "decision" ? 0.95 : 0.05;
    };
    model.choose = [](const std::string& name, const Json& options) {
        return number_of(options, name == "decision_anchor" ? "sg_one" : "none");
    };
    auto target = store("m_live");
    auto repo =
        std::make_shared<RowsRepo>(std::vector<Json>{Json{{"id", "sg_one"},
                                                          {"text", "We decided to ship on Friday."},
                                                          {"start_s", 10},
                                                          {"end_s", 15}}});
    const auto judge = model.judge();
    REQUIRE(judge->is_local());
    meeting_intel::LiveHighlights worker(target, repo,
                                         {.judge = [judge] { return judge; },
                                          .enabled =
                                              [] {
                                                  return true;
                                              }});
    worker.observe({}, 60);
    REQUIRE(eventually([&] { return !worker.busy(); }));
    worker.shutdown();
    const auto pulses = target->snapshot().at("live_highlights");
    REQUIRE(pulses.size() == 1);
    CHECK(pulses[0].at("kind") == "decision");
    CHECK(pulses[0].at("segment_id") == "sg_one");
    // No full distribution on this route, so no source ranking is claimed.
    CHECK_FALSE(pulses[0].at("assessment").contains("source_rank"));
    CHECK(model.sent() == 1);
}

TEST_CASE("citation check from a local model", "[test_text_judge]") {
    // from tests/test_text_judge.py::test_citation_check_from_a_local_model
    LocalModel model;
    model.choose = [](const std::string&, const Json& options) {
        return number_of(options, "supported");
    };
    auto target = store("m_cite");
    auto repo = std::make_shared<RowsRepo>(
        std::vector<Json>{Json{{"id", "sg_one"},
                               {"text", "We agree to ship the beta on Friday."},
                               {"start_s", 10},
                               {"end_s", 15}}});
    const auto judge = model.judge();
    meeting_intel::CitationVerifier verifier(target, repo,
                                             {.judge = [judge] { return judge; },
                                              .allowed =
                                                  [] {
                                                      return true;
                                                  }});
    REQUIRE(target
                ->apply("agent", std::string("notes"),
                        Json::array({Json{{"op", "add_item"},
                                          {"card", "decisions"},
                                          {"text", "The beta ships on Friday."},
                                          {"evidence", Json::array({"sg_one"})}}}))[0]
                .ok);
    REQUIRE(eventually([&] { return !verifier.busy(); }));
    verifier.shutdown();
    const auto check = target->snapshot().at("cards").at("decisions")[0].at("citation_check");
    CHECK(check.at("status") == "supported");
    CHECK(check.at("model") == "judge-1");
    // confidence(0.95, 3): (p - 1/n) / (1 - 1/n)
    CHECK(check.at("confidence").get<double>() == Approx((0.95 - 1.0 / 3) / (1 - 1.0 / 3)));
}

TEST_CASE("voice command from a local model", "[test_text_judge]") {
    // from tests/test_text_judge.py::test_voice_command_from_a_local_model
    LocalModel model;
    model.choose = [](const std::string&, const Json& options) {
        return number_of(options, "mark_decision");
    };
    auto target = store("m_voice");
    const auto judge = model.judge();
    std::promise<void> saved;
    auto done = saved.get_future();
    meeting_intel::VoiceCommandListener listener(target,
                                                 meeting_intel::default_voice_command_names(),
                                                 {.judge = [judge] { return judge; },
                                                  .allowed = [] { return true; },
                                                  .feedback =
                                                      [&](const Json& event) {
                                                          if (event.at("phase") == "saved") {
                                                              saved.set_value();
                                                          }
                                                      }});
    const Json previous{{"id", "sg_prev"},
                        {"text", "Let's ship on Friday."},
                        {"start_s", 1.0},
                        {"end_s", 3.0},
                        {"channel", "mic"}};
    const Json command{{"id", "sg_cmd"},
                       {"text", "Note taker, mark that as a decision."},
                       {"start_s", 4.0},
                       {"end_s", 6.0},
                       {"channel", "mic"}};
    CHECK(listener.observe({previous}) == 0);
    CHECK(listener.observe({command}) == 1);
    REQUIRE(done.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    listener.shutdown();
    const auto decisions = target->snapshot().at("cards").at("decisions");
    REQUIRE(decisions.size() == 1);
    CHECK(decisions[0].at("text") == "Let's ship on Friday.");
}

TEST_CASE("insight review on a local model", "[test_text_judge]") {
    // from tests/test_text_judge.py::test_insight_review_on_a_local_model
    // OpenWhisper asks JudgeReviewer.evaluate directly; here the review of a saved meeting asks
    // the text model, which answers every yes/no question 0.97.
    LocalModel model;
    model.noul = [](const std::string&) {
        return 0.97;
    };
    meeting_state::testing::FakeMeetingRepository repo;
    repo.create_meeting(Json{{"id", "m_review"},
                             {"status", "ended"},
                             {"started_at", "2026-11-17T09:00:00+00:00"},
                             {"host_token", "host"},
                             {"guest_token", "guest"},
                             {"spool_dir", "spool"}});
    meeting_state::TranscriptSegment segment;
    segment.segment_id = "sg_claim";
    segment.meeting_id = "m_review";
    segment.channel = "mic";
    segment.start_s = 10;
    segment.end_s = 12;
    segment.text = "We agreed the beta ships Friday.";
    repo.add_segments({segment});
    meeting_state::MeetingState state;
    state.meeting_id = "m_review";
    state.cloud_enabled = true; // consented(): the review runs only in a meeting with insights on
    state.insight_review = Json{{"enabled", true},
                                {"status", "pending"},
                                {"consent", "typesafe-text-v1"},
                                {"sensitivity", "normal"},
                                {"questions", Json::array()}};
    auto target = std::make_shared<meeting_state::MeetingStateStore>(std::move(state));
    REQUIRE(target
                ->apply("agent", std::string("notes"),
                        Json::array({Json{{"op", "add_item"},
                                          {"card", "decisions"},
                                          {"text", "The beta ships Friday."},
                                          {"evidence", Json::array({"sg_claim"})}}}))[0]
                .ok);
    REQUIRE(target->update_runtime_fields(
        Json{{"status", "ended"}, {"finalization", {{"status", "completed"}}}}));
    const auto judge = model.judge();
    CHECK(judge->model() == "judge-1");
    const auto result =
        meeting_intel::run_insight_review(*target, repo,
                                          {.judge = [judge](double) { return judge; },
                                           .allowed =
                                               [] {
                                                   return true;
                                               }});
    CHECK(result.at("ok") == true);
    REQUIRE(model.sent() == 1);
    const auto questions = asked(model.requests[0]).at("questions");
    CHECK(questions.contains("support"));
    CHECK(questions.at("support").at("answer") == "yes/no");
    const auto claim = target->snapshot().at("cards").at("decisions")[0];
    CHECK(claim.contains("review"));
    CHECK(claim.at("review").at("scores").at("support") == 0.97);
}

} // namespace
