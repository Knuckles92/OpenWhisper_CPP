// benchmarks/cleanup_levels/run.py: the cleanup-level golden set and its runner, without a
// network (tests/test_personalize_s2_benchmark.py and the golden-set test of
// tests/test_personalize_fix2_g4_unknown_app_paragraphs.py).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "base/testing/map_env_reader.h"
#include "bench/cleanup_levels.h"
#include "bench/commands.h"
#include "process/testing/map_env.h"
#include "pyport/text.h"
#include "text/cleanup_config.h"
#include "text/cleanup_profiles.h"
#include "text/cleanup_prompts.h"

using fv::Json;
namespace config = fv::text::config;

namespace {

// FakeCleaner: answers from a table (its input otherwise) and records each call.
struct FakeCleaner {
    std::map<std::string, std::string> outputs;
    std::optional<std::string> error;
    std::vector<std::pair<std::string, std::string>> calls;
    std::optional<std::string> last_error = "not run";

    fv::bench::Cleaner cleaner() {
        return fv::bench::Cleaner{[this](std::string_view text, std::string_view prompt) {
                                      calls.emplace_back(std::string(text), std::string(prompt));
                                      last_error = error;
                                      const auto found = outputs.find(std::string(text));
                                      return found == outputs.end() ? std::string(text)
                                                                    : found->second;
                                  },
                                  [this] {
                                      return last_error;
                                  }};
    }
};

std::map<std::string, Json> by_id(const std::vector<Json>& cases) {
    std::map<std::string, Json> out;
    for (const Json& item : cases) {
        out[item["id"].get<std::string>()] = item;
    }
    return out;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("test_the_golden_set_covers_every_level_and_behaviour",
          "[test_personalize_s2_benchmark]") {
    // from
    // tests/test_personalize_s2_benchmark.py::test_the_golden_set_covers_every_level_and_behaviour
    const auto cases = fv::bench::load_cases();
    std::set<std::string> labels;
    std::set<std::string> categories;
    std::set<std::string> ids;
    for (const Json& item : cases) {
        labels.insert(fv::bench::case_label(item));
        categories.insert(item["category"].get<std::string>());
        ids.insert(item["id"].get<std::string>());
    }

    CHECK(cases.size() >= 30);
    CHECK(ids.size() == cases.size());
    for (const std::string level : {"light", "medium", "high"}) {
        CHECK(labels.contains(level));
    }
    for (const auto& profile : fv::text::starter_profiles()) {
        CHECK(labels.contains("profile:" + profile.id));
    }
    for (const std::string category :
         {"corrections", "corrections-negative", "lists", "lists-negative", "lists-inline", "light",
          "high", "language", "guard", "profile"}) {
        CHECK(categories.contains(category));
    }
}

TEST_CASE("test_bad_cases_are_named", "[test_personalize_s2_benchmark]") {
    // from tests/test_personalize_s2_benchmark.py::test_bad_cases_are_named
    const auto [line, message] = GENERATE(table<std::string, std::string>({
        {R"({"id": "a", "input": "hi"})", "exactly one of level and profile"},
        {R"({"id": "a", "level": "extreme", "input": "hi", "must_contain": ["hi"]})",
         "unknown level"},
        {R"({"id": "a", "profile": "memo", "input": "hi", "must_contain": ["hi"]})",
         "unknown profile"},
        {R"({"id": "a", "level": "light", "input": "hi"})", "checks nothing"},
        {R"({"id": "a", "level": "light", "input": " ", "must_contain": ["hi"]})", "no input"},
        {R"({"id": "a", "level": "light", "input": "hi", "must_contain": ["hi"], "destination": "fax"})",
         "unknown destination"},
        {R"({"id": "a", "level": "light", "input": "hi", "must_contain": ["hi"], "expected": "Hi."})",
         "unknown keys"},
    }));
    CAPTURE(line);
    CHECK_THROWS_WITH(fv::bench::load_cases(line + "\n"),
                      Catch::Matchers::ContainsSubstring(message));
}

TEST_CASE("bad cases are named with Python's spelling", "[cleanup_levels]") {
    CHECK_THROWS_WITH(
        fv::bench::load_cases(
            R"({"id": "a", "level": "extreme", "input": "hi", "must_contain": ["hi"], "x": 1})"),
        "a: unknown keys ['x']; unknown level 'extreme'");
    CHECK_THROWS_WITH(fv::bench::load_cases("\n"
                                            R"({"level": "light", "input": "hi"})"),
                      "line 2: checks nothing");
}

TEST_CASE("test_duplicate_ids_are_refused", "[test_personalize_s2_benchmark]") {
    // from tests/test_personalize_s2_benchmark.py::test_duplicate_ids_are_refused
    const std::string line =
        R"({"id": "a", "level": "light", "input": "hi", "must_contain": ["hi"]})";
    CHECK_THROWS_WITH(fv::bench::load_cases(line + "\n\n" + line + "\n"),
                      Catch::Matchers::ContainsSubstring("a: duplicate id"));
}

TEST_CASE("test_prompts_are_the_shipped_ones", "[test_personalize_s2_benchmark]") {
    // from tests/test_personalize_s2_benchmark.py::test_prompts_are_the_shipped_ones
    const auto cases = by_id(fv::bench::load_cases());

    CHECK(fv::bench::compose_prompt(cases.at("light-keeps-retraction")) ==
          fv::text::preset("light"));
    CHECK(fv::bench::compose_prompt(cases.at("list-numbers")) == fv::text::preset("medium"));
    CHECK(fv::bench::compose_prompt(cases.at("list-unknown-app-inline")) ==
          fv::text::preset("medium") + "\n\n" + config::cleanup_sentences().unknown_app_lines);
    const std::string profile = fv::bench::compose_prompt(cases.at("profile-email-correction"));
    CHECK(contains(profile, config::cleanup_sentences().spoken_corrections));
    CHECK(contains(profile, "Subject line"));
}

TEST_CASE("test_checks", "[test_personalize_s2_benchmark]") {
    // from tests/test_personalize_s2_benchmark.py::test_checks
    const auto [item, output, failures] =
        GENERATE(table<std::string, std::string, std::vector<std::string>>({
            {R"({"must_contain": [["3", "three"]], "must_not_contain": ["2"]})", "Meet at 3.", {}},
            {R"({"must_contain": [["3", "three"]]})", "Meet at Three.", {}},
            {R"({"must_contain": ["eggs"]})", "Buy eggshells.", {"missing 'eggs'"}},
            {R"({"must_not_contain": ["um"]})", "Umbrella, please.", {}},
            {R"({"must_not_contain": ["Mark"]})", "Send it to mark.", {"contains 'Mark'"}},
            {R"({"must_contain": ["milk"], "min_lines": 3})",
             "1. Milk\n\n2. Eggs",
             {"2 lines, wanted at least 3"}},
            {R"({"must_contain": ["milk"], "max_lines": 1})",
             "Milk\nEggs",
             {"2 lines, wanted at most 1"}},
        }));
    CAPTURE(item, output);
    CHECK(fv::bench::check(Json::parse(item), output) == failures);
}

TEST_CASE("test_run_checks_each_answer_and_counts_rates", "[test_personalize_s2_benchmark]") {
    // from tests/test_personalize_s2_benchmark.py::test_run_checks_each_answer_and_counts_rates
    std::vector<Json> chosen;
    for (const Json& item : fv::bench::load_cases()) {
        const auto id = item["id"].get<std::string>();
        if (id == "corrections-time" || id == "light-keeps-retraction") {
            chosen.push_back(item);
        }
    }
    FakeCleaner cleaner;
    cleaner.outputs = {{"let's meet at 2 actually 3", "Let's meet at 3."}};

    const auto results = fv::bench::run(chosen, cleaner.cleaner());
    const auto summary = fv::bench::summarize(results);

    REQUIRE(results.size() == 2);
    CHECK(results[0].passed);
    CHECK_FALSE(results[1].passed);
    CHECK(results[1].failures == std::vector<std::string>{"missing '2 / two'"});
    REQUIRE(cleaner.calls.size() == 2);
    CHECK(cleaner.calls[0].second == fv::text::preset("medium"));
    CHECK(cleaner.calls[1].second == fv::text::preset("light"));
    CHECK(summary.passed == 1);
    CHECK(summary.total == 2);
    using Counts = std::map<std::string, std::pair<std::size_t, std::size_t>>;
    CHECK(summary.by_label == Counts{{"light", {0, 1}}, {"medium", {1, 1}}});
    CHECK(summary.by_category == Counts{{"corrections", {1, 1}}, {"light", {0, 1}}});
    CHECK(summary.to_json()["by_label"]["light"] == Json::array({0, 1}));
    CHECK(fv::bench::rate(1, 2) == "1/2 (50%)");
    CHECK(fv::bench::rate(0, 0) == "0/0");
}

TEST_CASE("test_a_failed_cleanup_fails_the_case", "[test_personalize_s2_benchmark]") {
    // from tests/test_personalize_s2_benchmark.py::test_a_failed_cleanup_fails_the_case
    FakeCleaner cleaner;
    cleaner.error = "empty response";
    const auto cases = fv::bench::load_cases();

    const auto results = fv::bench::run({cases.front()}, cleaner.cleaner());

    CHECK(results[0].failures == std::vector<std::string>{"cleanup failed: empty response"});
}

TEST_CASE("test_main_stops_without_a_provider", "[test_personalize_s2_benchmark]") {
    // from tests/test_personalize_s2_benchmark.py::test_main_stops_without_a_provider
    fv::MapEnvReader reader;
    fv::MapEnv env(false);
    std::ostringstream out;
    std::ostringstream err;
    fv::bench::CommandContext context{out, err, reader, env};
    std::string asked_for;
    context.load_settings = [] {
        return Json::object();
    };
    context.make_cleaner = [&](const std::string& provider, const std::string&,
                               const std::string&) {
        asked_for = provider;
        fv::bench::CleanupClientHandle handle;
        handle.provider = provider;
        handle.is_available = [] {
            return false;
        };
        return handle;
    };

    CHECK(fv::bench::run_command({"cleanup-levels", "--case", "corrections-time"}, context) == 2);
    CHECK_THAT(out.str(), Catch::Matchers::ContainsSubstring("not set up"));
    CHECK(asked_for == "openrouter");
    CHECK(fv::bench::run_command({"cleanup-levels", "--case", "no-such-case"}, context) == 2);
    CHECK_THAT(out.str(), Catch::Matchers::ContainsSubstring("No cases match."));
}

TEST_CASE("the cleanup-levels command runs a scripted provider and writes its results",
          "[cleanup_levels]") {
    fv::MapEnvReader reader;
    fv::MapEnv env(false);
    std::ostringstream out;
    std::ostringstream err;
    fv::bench::CommandContext context{out, err, reader, env};
    auto fake = std::make_shared<FakeCleaner>();
    fake->outputs = {{"let's meet at 2 actually 3", "Let's meet at 3."}};
    fake->error = std::nullopt;
    context.make_cleaner = [fake](const std::string& provider, const std::string& model,
                                  const std::string& reasoning) {
        fv::bench::CleanupClientHandle handle;
        handle.cleaner = fake->cleaner();
        handle.is_available = [] {
            return true;
        };
        handle.provider = provider;
        handle.model = model;
        handle.reasoning = reasoning;
        handle.owner = fake;
        return handle;
    };
    CHECK(fv::bench::run_command({"cleanup-levels", "--provider", "openai", "--model", "m",
                                  "--case", "corrections-time"},
                                 context) == 0);
    CHECK_THAT(out.str(),
               Catch::Matchers::ContainsSubstring("1 cases · openai · m · thinking off"));
    CHECK_THAT(out.str(),
               Catch::Matchers::ContainsSubstring("pass  medium                 corrections-time"));
    CHECK_THAT(out.str(), Catch::Matchers::ContainsSubstring("All: 1/1 (100%)"));
}

TEST_CASE("test_the_golden_set_checks_a_long_dictation_into_an_unknown_app_stays_one_line",
          "[test_personalize_fix2_g4_unknown_app_paragraphs]") {
    // from
    // tests/test_personalize_fix2_g4_unknown_app_paragraphs.py::test_the_golden_set_checks_a_long_dictation_into_an_unknown_app_stays_one_line
    std::vector<Json> cases;
    for (const Json& item : fv::bench::load_cases()) {
        if (item.value("destination", std::string()) == "unknown" &&
            item.value("max_lines", 0) == 1 &&
            fv::py::split(item["input"].get<std::string>()).size() >= 30) {
            cases.push_back(item);
        }
    }

    REQUIRE_FALSE(cases.empty());
    for (const Json& item : cases) {
        CHECK(fv::bench::compose_prompt(item).ends_with(
            "\n\n" + config::cleanup_sentences().unknown_app_lines));
    }
}
