// Opt-in semantic reranking of history search (the semantic tests of OpenWhisper's
// tests/test_meeting_fast_features.py and the local-judge test of tests/test_text_judge.py), over
// a stub source and llm::FakeJudge.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "intel_support.h"
#include "llm/testing/fake_judge.h"
#include "meeting_intel/intel_settings.h"
#include "meeting_intel/semantic_search.h"

using namespace fv::meeting_intel;
using namespace fv::meeting_intel::test;
using Catch::Matchers::ContainsSubstring;
using fv::Json;
using fv::llm::FakeJudge;

namespace {

Json semantic_on(std::string_view provider = "typesafe") {
    return Json{{"typesafe_enabled", true},
                {"typesafe_semantic_search_enabled", true},
                {"typesafe_provider", std::string(provider)}};
}

Json answers(std::initializer_list<std::pair<const char*, double>> scores) {
    Json out = Json::object();
    for (const auto& [key, score] : scores) {
        out[key] = Json{{"type", "noul"}, {"noul", score}};
    }
    return out;
}

} // namespace

// from tests/test_meeting_fast_features.py::test_new_features_require_explicit_opt_in
TEST_CASE("test_new_features_require_explicit_opt_in", "[test_meeting_fast_features]") {
    for (const char* feature : {"citations", "semantic_search", "question_radar", "highlights"}) {
        CAPTURE(feature);
        const std::string key = std::string("typesafe_") + feature + "_enabled";
        CHECK_FALSE(resolve_typesafe_feature_enabled(Json{{"typesafe_enabled", true}}, feature));
        CHECK_FALSE(resolve_typesafe_feature_enabled(Json{{key, true}}, feature));
        CHECK(resolve_typesafe_feature_enabled(Json{{"typesafe_enabled", true}, {key, true}},
                                               feature));
    }
}

// from tests/test_meeting_fast_features.py::test_semantic_ranking_and_failure_fallback
TEST_CASE("test_semantic_ranking_and_failure_fallback", "[test_meeting_fast_features]") {
    StubHistory repo;
    repo.rows = {Json{{"text", "keyword"}}};
    repo.candidates = {
        Json{{"segment_id", "sg_a"}, {"meeting_id", "m_a"}, {"text", "Launch postponed"}},
        Json{{"segment_id", "sg_b"}, {"meeting_id", "m_b"}, {"text", "Coffee break"}}};
    repo.meetings["m_a"] = Json{{"cloud_enabled", true}};
    repo.meetings["m_b"] = Json{{"cloud_enabled", true}};
    auto judge = std::make_shared<FakeJudge>();
    judge->push(answers({{"p0", 0.96}, {"p1", 0.08}}));
    Json result =
        search_history(repo, semantic_on(), "shipping delay", true, std::nullopt, 50, judge);
    CHECK(result.at("mode") == "semantic");
    REQUIRE(result.at("results").size() == 1);
    CHECK(result.at("results").at(0).at("semantic_score") == 0.96);
    judge->push(std::nullopt);
    result = search_history(repo, semantic_on(), "shipping delay", true, std::nullopt, 50, judge);
    CHECK(result.at("mode") == "keyword");
    CHECK(result.at("results") == Json::array({Json{{"text", "keyword"}}}));
}

// from tests/test_meeting_fast_features.py::test_semantic_without_a_key_names_the_chosen_routes_key
TEST_CASE("test_semantic_without_a_key_names_the_chosen_routes_key",
          "[test_meeting_fast_features]") {
    StubHistory repo;
    repo.rows = {Json{{"text", "keyword"}}};
    const Json result = search_history(repo, semantic_on("openrouter"), "shipping delay", true,
                                       std::nullopt, 50, nullptr);
    CHECK(result.at("mode") == "keyword");
    CHECK_THAT(result.at("message").get<std::string>(), ContainsSubstring("an OpenRouter API key"));
    CHECK(repo.candidate_calls.empty());
}

// from tests/test_meeting_fast_features.py::test_semantic_disabled_never_reads_remote_candidates
TEST_CASE("test_semantic_disabled_never_reads_remote_candidates", "[test_meeting_fast_features]") {
    StubHistory repo;
    auto judge = std::make_shared<FakeJudge>();
    const Json off{{"typesafe_enabled", true}, {"typesafe_semantic_search_enabled", false}};
    const Json result = search_history(repo, off, "shipping delay", true, std::nullopt, 50, judge);
    CHECK(result.at("mode") == "keyword");
    CHECK_THAT(result.at("message").get<std::string>(), ContainsSubstring("Enable"));
    CHECK(repo.candidate_calls.empty());
    CHECK(judge->calls().empty());
}

// from tests/test_meeting_fast_features.py::test_semantic_rechecks_meeting_consent_before_sending
TEST_CASE("test_semantic_rechecks_meeting_consent_before_sending", "[test_meeting_fast_features]") {
    StubHistory repo;
    repo.candidates = {Json{{"meeting_id", "private"}, {"text", "Do not send"}}};
    repo.meetings["private"] = Json{{"cloud_enabled", false}};
    auto judge = std::make_shared<FakeJudge>();
    const Json result =
        search_history(repo, semantic_on(), "shipping delay", true, std::nullopt, 50, judge);
    CHECK(result.at("results") == Json::array());
    CHECK(judge->calls().empty());
}

// from
// tests/test_text_judge.py::test_semantic_search_ranks_every_meeting_only_when_the_judge_is_local
TEST_CASE("test_semantic_search_ranks_every_meeting_only_when_the_judge_is_local",
          "[test_text_judge]") {
    StubHistory repo;
    repo.candidates = {
        Json{{"segment_id", "sg_a"}, {"meeting_id", "m_private"}, {"text", "Launch postponed"}}};
    repo.meetings["m_private"] = Json{{"cloud_enabled", false}};
    auto local = std::make_shared<FakeJudge>();
    local->set_local(true);
    local->set_handler([](const Json&, const Json& questions) -> std::optional<Json> {
        Json out = Json::object();
        for (const auto& [key, _] : questions.items()) {
            out[key] = Json{{"type", "noul"}, {"noul", 0.9}};
        }
        return out;
    });
    Json result =
        search_history(repo, semantic_on(), "shipping delay", true, std::nullopt, 50, local);
    REQUIRE(repo.candidate_calls.size() == 1);
    CHECK(repo.candidate_calls.back().shared_only == false);
    CHECK(result.at("mode") == "semantic");
    CHECK(result.at("message") == std::string(kLocalScope));
    REQUIRE(result.at("results").size() == 1);
    CHECK(result.at("results").at(0).at("meeting_id") == "m_private");

    auto hosted = std::make_shared<FakeJudge>();
    hosted->set_local(false);
    result = search_history(repo, semantic_on(), "shipping delay", true, std::nullopt, 50, hosted);
    CHECK(repo.candidate_calls.back().shared_only == true);
    // The meeting had AI insights off, so nothing of it is sent.
    CHECK(result.at("results") == Json::array());
    CHECK(result.at("message") == std::string(kScope));
    CHECK(hosted->calls().empty());
}

TEST_CASE("the ranking stops at its budget and shows keyword results", "[semantic_search]") {
    fv::FakeClock clock;
    StubHistory repo;
    repo.rows = {Json{{"text", "keyword"}}};
    for (int index = 0; index < 24; ++index) {
        repo.candidates.push_back(
            Json{{"meeting_id", "m"}, {"text", "passage " + std::to_string(index)}});
    }
    auto judge = std::make_shared<FakeJudge>();
    judge->set_local(true);
    judge->set_handler([&clock](const Json&, const Json& questions) -> std::optional<Json> {
        // Each batch takes 15 s of the 20 s budget.
        clock.advance(std::chrono::seconds(15));
        Json out = Json::object();
        for (const auto& [key, _] : questions.items()) {
            out[key] = Json{{"type", "noul"}, {"noul", 0.9}};
        }
        return out;
    });
    const Json result =
        search_history(repo, semantic_on(), "query", true, std::nullopt, 50, judge, clock);
    CHECK(result.at("mode") == "keyword");
    CHECK(result.at("message") == std::string(kUnavailableMessage));
    REQUIRE(judge->calls().size() == 2);
    // The second batch was given what was left of the budget.
    CHECK(judge->calls().at(0).timeout_s == std::optional<double>(20.0));
    CHECK(judge->calls().at(1).timeout_s == std::optional<double>(5.0));
}

TEST_CASE("a failing judge is logged by its type only", "[semantic_search]") {
    StubHistory repo;
    repo.rows = {Json{{"text", "keyword"}}};
    repo.candidates = {Json{{"meeting_id", "m"}, {"text", "Launch postponed"}}};
    auto judge = std::make_shared<FakeJudge>();
    judge->set_local(true);
    judge->set_handler([](const Json&, const Json&) -> std::optional<Json> {
        throw std::invalid_argument("passage quoted: Launch postponed");
    });
    LogCapture log;
    const Json result =
        search_history(repo, semantic_on(), "shipping delay", true, std::nullopt, 50, judge);
    CHECK(result.at("mode") == "keyword");
    CHECK(result.at("message") == std::string(kUnavailableMessage));
    CHECK(result.at("results") == Json::array({Json{{"text", "keyword"}}}));
    CHECK(log.count("Semantic history ranking failed") == 1);
    CHECK(log.text().find("postponed") == std::string::npos);
    CHECK(log.text().find("shipping") == std::string::npos);
}

TEST_CASE("a source without candidate search falls back to keyword results", "[semantic_search]") {
    StubHistory repo;
    repo.candidates_fail = true;
    repo.rows = {Json{{"text", "keyword"}}};
    auto judge = std::make_shared<FakeJudge>();
    const Json result = search_history(repo, semantic_on(), "query", true, std::nullopt, 50, judge);
    CHECK(result.at("mode") == "keyword");
    CHECK(result.at("message") == std::string(kUnavailableMessage));
}

TEST_CASE("the judge is built only when a ranking runs", "[semantic_search]") {
    StubHistory repo;
    int built = 0;
    const JudgeFactory factory = [&built]() -> std::shared_ptr<fv::llm::Judge> {
        ++built;
        return nullptr;
    };
    const Json off{{"typesafe_enabled", false}};
    search_history(repo, fixed(off), "query", true, std::nullopt, 50, factory);
    search_history(repo, fixed(semantic_on()), "query", false, std::nullopt, 50, factory);
    search_history(repo, fixed(semantic_on()), "   ", true, std::nullopt, 50, factory);
    CHECK(built == 0);
    search_history(repo, fixed(semantic_on()), "query", true, std::nullopt, 50, factory);
    CHECK(built == 1);
}
