// services/dictation_language.py: tests/test_personalize_s8_language.py and the golden vectors of
// services.dictation_language.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "golden/golden.h"
#include "settings/store.h"
#include "settings/testing/memory_store.h"
#include "speech/dictation_language.h"

namespace dl = fv::speech;
using fv::Json;
using Codes = std::vector<std::string>;

namespace {

// _settings(engine, chosen, active, **extra) of the Python test.
Json settings(const std::string& engine, const Json& chosen = Json::array(),
              const std::string& active = "", Json extra = Json::object()) {
    Json made = Json::object();
    made["selected_model"] = engine;
    made["dictation_languages"] = chosen;
    made["dictation_active_language"] = active;
    for (auto& [key, value] : extra.items()) {
        made[key] = value;
    }
    return made;
}

Json list(std::initializer_list<const char*> codes) {
    Json made = Json::array();
    for (const char* code : codes) {
        made.push_back(code);
    }
    return made;
}

} // namespace

// from tests/test_personalize_s8_language.py::TestChoices::test_only_languages_the_engine_takes
// The rows of local_whisper, moonshine and parakeet_mlx are dropped engines: OpenWhisper_CPP reads
// those keys as its default engine, Parakeet.
TEST_CASE("only languages the engine takes", "[test_personalize_s8_language]") {
    const auto [engine, expected] = GENERATE(table<std::string, Codes>({
        {"api", {"en", "es", "de"}},
        {"parakeet", {"en", "es"}},
        {"remote", {"en", "es", "de"}},
    }));
    CHECK(dl::language_choices(settings(engine, list({"en", "es", "de"}))) == expected);
}

// from
// tests/test_personalize_s8_language.py::TestChoices::test_saved_list_is_cleaned_up_without_losing_order
TEST_CASE("saved list is cleaned up without losing order", "[test_personalize_s8_language]") {
    Json chosen = list({"fr", "xx", "auto", "en", "fr"});
    chosen.push_back(3);
    chosen.push_back(nullptr);
    CHECK(dl::chosen_languages(settings("api", chosen)) == Codes{"fr", "en"});
    CHECK(dl::chosen_languages(Json{{"dictation_languages", "en"}}).empty());
}

// from tests/test_personalize_s8_language.py::TestChoices::test_single_language_engines_say_why
TEST_CASE("single language engines say why", "[test_personalize_s8_language]") {
    CHECK(dl::single_language_reason(settings("parakeet")).empty());
    CHECK(dl::single_language_reason(settings("api")).empty());
    CHECK(dl::single_language_reason(settings("remote")).empty());
}

// from
// tests/test_personalize_s8_language.py::TestChoices::test_legacy_api_selection_counts_as_the_api
TEST_CASE("legacy api selection counts as the api", "[test_personalize_s8_language]") {
    CHECK(dl::engine(Json{{"selected_model", "api_whisper"}}) == "api");
    CHECK(dl::engine(Json{{"selected_model", "nonsense"}}) == dl::engine(Json::object()));
}

// from
// tests/test_personalize_s8_language.py::TestJobLanguage::test_active_language_is_used_only_when_the_engine_takes_it
TEST_CASE("active language is used only when the engine takes it",
          "[test_personalize_s8_language]") {
    CHECK(dl::job_language(settings("api", list({"en", "es"}), "es")) == "es");
    CHECK(dl::job_language(settings("parakeet", list({"en", "de"}), "de")).empty());
    CHECK(dl::job_language(settings("api", list({"en", "es"}), "")).empty());
    CHECK(dl::job_language(settings("api", Json::array(), "es")).empty());
}

// from
// tests/test_personalize_s8_language.py::TestJobLanguage::test_current_language_falls_back_to_the_engine
TEST_CASE("current language falls back to the engine", "[test_personalize_s8_language]") {
    CHECK(dl::current_language(settings("api", list({"en", "es"}), "es")) == "es");
    CHECK(dl::current_language(settings("api", list({"en", "es"}))) == "auto");
    const Json parakeet =
        settings("parakeet", list({"en", "es"}), "", Json{{"local_asr_language", "fr"}});
    CHECK(dl::current_language(parakeet) == "fr");
}

// from tests/test_personalize_s8_language.py::TestMutators::test_cycle_wraps_through_the_choices
TEST_CASE("cycle wraps through the choices", "[test_personalize_s8_language]") {
    Json state = settings("api", list({"en", "es", "fr"}), "en");
    Codes seen;
    for (int i = 0; i < 4; ++i) {
        dl::cycle(state);
        seen.push_back(state["dictation_active_language"].get<std::string>());
    }
    CHECK(seen == Codes{"es", "fr", "en", "es"});
}

// from
// tests/test_personalize_s8_language.py::TestMutators::test_cycle_starts_after_the_engines_own_language
TEST_CASE("cycle starts after the engines own language", "[test_personalize_s8_language]") {
    Json state =
        settings("parakeet", list({"es", "en", "fr"}), "", Json{{"local_asr_language", "en"}});
    dl::cycle(state);
    CHECK(state["dictation_active_language"] == "fr");

    Json unknown = settings("api", list({"es", "fr"}));
    dl::cycle(unknown);
    CHECK(unknown["dictation_active_language"] == "es");
}

// from
// tests/test_personalize_s8_language.py::TestMutators::test_cycle_leaves_one_or_no_choice_alone
// Moonshine (English only) is dropped; Parakeet with one or no accepted choice stands in.
TEST_CASE("cycle leaves one or no choice alone", "[test_personalize_s8_language]") {
    const Json chosen = GENERATE(Json::array(), list({"en"}), list({"de", "en"}));
    Json state = settings("parakeet", chosen, "");
    const Json before = state;
    dl::cycle(state);
    CHECK(state == before);
}

// from
// tests/test_personalize_s8_language.py::TestMutators::test_set_chosen_keeps_the_active_language_valid
TEST_CASE("set chosen keeps the active language valid", "[test_personalize_s8_language]") {
    Json state = settings("api", list({"en", "es"}), "es");
    dl::set_chosen(state, {"fr", "es", "fr", "auto"});
    CHECK(state["dictation_languages"] == list({"fr", "es"}));
    CHECK(state["dictation_active_language"] == "es");

    dl::set_chosen(state, {"de", "fr"});
    CHECK(state["dictation_active_language"] == "de");

    dl::set_chosen(state, {});
    CHECK(state["dictation_active_language"] == "");
}

// from
// tests/test_personalize_s8_language.py::TestMutators::test_set_active_ignores_languages_that_are_not_choices
TEST_CASE("set active ignores languages that are not choices", "[test_personalize_s8_language]") {
    Json state = settings("api", list({"en", "es"}), "en");
    dl::set_active(state, "de");
    CHECK(state["dictation_active_language"] == "en");
    dl::set_active(state, "es");
    CHECK(state["dictation_active_language"] == "es");
}

// from
// tests/test_personalize_s8_language.py::TestMutators::test_switching_never_touches_the_engine_language
TEST_CASE("switching never touches the engine language", "[test_personalize_s8_language]") {
    fv::MemorySettingsStore store;
    store.update(Json{{"selected_model", "parakeet"}, {"local_asr_language", "ru"}});
    store.mutate([](Json& s) { dl::set_chosen(s, {"en", "es"}); });
    store.mutate([](Json& s) { dl::cycle(s); });
    store.mutate([](Json& s) { dl::set_active(s, "en"); });
    const Json saved = *store.snapshot();
    CHECK(saved["local_asr_language"] == "ru");
    CHECK(saved["dictation_active_language"] == "en");
    CHECK(dl::job_language(saved) == "en");
}

// from tests/test_personalize_s8_language.py::test_labels
TEST_CASE("labels", "[test_personalize_s8_language]") {
    CHECK(dl::label("es") == "Spanish");
    CHECK(dl::label("zh") == "Chinese");
    CHECK(dl::label("xx") == "xx");
    CHECK(dl::short_label("es") == "ES");
    CHECK(dl::short_label("auto") == "AUTO");
}

namespace {

Json strings(const std::vector<std::string>& codes) {
    Json made = Json::array();
    for (const auto& code : codes) {
        made.push_back(code);
    }
    return made;
}

void replay_reader(const char* function, const std::function<Json(const Json&)>& read) {
    fv::golden::for_each("services.dictation_language", function, [&](const fv::golden::Case& c) {
        CHECK(fv::golden::equal(c.result<Json>(), read(c.arg<Json>(0))));
    });
}

void replay_mutator(const char* function,
                    const std::function<void(Json&, const fv::golden::Case&)>& change) {
    fv::golden::for_each("services.dictation_language", function, [&](const fv::golden::Case& c) {
        Json state = c.arg<Json>(0);
        change(state, c);
        CHECK(fv::golden::equal(c.result<Json>(), state));
    });
}

} // namespace

// One test case per function: each case of a function is a section, and a test case runs once
// per section.
TEST_CASE("golden dictation_language engine", "[golden]") {
    replay_reader("engine", [](const Json& s) { return Json(dl::engine(s)); });
}

TEST_CASE("golden dictation_language accepted_languages", "[golden]") {
    replay_reader("accepted_languages",
                  [](const Json& s) { return strings(dl::accepted_languages(s)); });
}

TEST_CASE("golden dictation_language single_language_reason", "[golden]") {
    replay_reader("single_language_reason",
                  [](const Json& s) { return Json(dl::single_language_reason(s)); });
}

TEST_CASE("golden dictation_language chosen_languages", "[golden]") {
    replay_reader("chosen_languages",
                  [](const Json& s) { return strings(dl::chosen_languages(s)); });
}

TEST_CASE("golden dictation_language language_choices", "[golden]") {
    replay_reader("language_choices",
                  [](const Json& s) { return strings(dl::language_choices(s)); });
}

TEST_CASE("golden dictation_language job_language", "[golden]") {
    replay_reader("job_language", [](const Json& s) { return Json(dl::job_language(s)); });
}

TEST_CASE("golden dictation_language engine_language", "[golden]") {
    replay_reader("engine_language", [](const Json& s) { return Json(dl::engine_language(s)); });
}

TEST_CASE("golden dictation_language current_language", "[golden]") {
    replay_reader("current_language", [](const Json& s) { return Json(dl::current_language(s)); });
}

TEST_CASE("golden dictation_language cycle", "[golden]") {
    replay_mutator("cycle", [](Json& state, const fv::golden::Case&) { dl::cycle(state); });
}

TEST_CASE("golden dictation_language set_active", "[golden]") {
    replay_mutator("set_active", [](Json& state, const fv::golden::Case& c) {
        dl::set_active(state, c.arg<std::string>(1));
    });
}

TEST_CASE("golden dictation_language add_chosen", "[golden]") {
    replay_mutator("add_chosen", [](Json& state, const fv::golden::Case& c) {
        dl::add_chosen(state, c.arg<std::string>(1));
    });
}

TEST_CASE("golden dictation_language set_chosen", "[golden]") {
    replay_mutator("set_chosen", [](Json& state, const fv::golden::Case& c) {
        dl::set_chosen(state, c.arg<std::vector<std::string>>(1));
    });
}

TEST_CASE("golden dictation_language labels", "[golden]") {
    fv::golden::for_each("services.dictation_language", "label", [](const fv::golden::Case& c) {
        CHECK(dl::label(c.arg<std::string>(0)) == c.result<std::string>());
    });
    fv::golden::for_each(
        "services.dictation_language", "short_label", [](const fv::golden::Case& c) {
            CHECK(dl::short_label(c.arg<std::string>(0)) == c.result<std::string>());
        });
}
TEST_CASE("Remote language choices belong to the currently paired fingerprint",
          "[dictation_language]") {
    const std::string fingerprint = "synthetic-language-host";
    struct Reset {
        const std::string& id;
        ~Reset() { fv::speech::forget_remote_runtime(id); }
    };
    const Reset clean{fingerprint};
    fv::Json saved{{"selected_model", "remote"},
                   {"remote_engine_client", {{"fingerprint", fingerprint}}},
                   {"dictation_languages", fv::Json::array({"en", "fr", "ja"})},
                   {"dictation_active_language", "fr"}};
    fv::speech::note_remote_runtime(fingerprint,
                                    {.host = "Synthetic desk",
                                     .engine_label = "Parakeet",
                                     .family = "parakeet",
                                     .languages = std::vector<std::string>{"en", "auto", "ja"},
                                     .own_language = "ja",
                                     .recognition_hints = true});
    CHECK(fv::speech::language_choices(saved) == std::vector<std::string>{"en", "ja"});
    CHECK(fv::speech::current_language(saved) == "ja");
    REQUIRE(fv::speech::remote_voice_capabilities(saved));
    CHECK(fv::speech::remote_voice_capabilities(saved)->recognition_hints);

    saved["remote_engine_client"]["fingerprint"] = "another-host";
    CHECK_FALSE(fv::speech::remote_voice_capabilities(saved));
    CHECK(fv::speech::current_language(saved) == "fr");
}
