// tests/test_personalize_s2_prompts.py (the presets, levels and custom prompts; the prompt a
// dictation is sent is M4.9's), tests/test_settings.py TestTranscriptCleanupRules and two
// tests of tests/test_transcript_cleanup.py; plus the services.cleanup_prompts,
// ui_qt.dialogs.cleanup_levels and services.settings (rules and prompt) golden vectors.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json.h"
#include "focus/snapshot.h"
#include "golden/golden.h"
#include "prompt_test_support.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "text/cleanup_config.h"
#include "text/cleanup_levels.h"
#include "text/cleanup_prompts.h"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;
using Catch::Matchers::StartsWith;
using fv::Json;
using fv::prompt_test::on;
using fv::prompt_test::snapshot_from;
using fv::prompt_test::TempDir;
using fv::text::CleanupLevel;
namespace text = fv::text;
namespace config = fv::text::config;
namespace golden = fv::golden;

namespace {

const std::string& corrections() {
    return config::cleanup_sentences().spoken_corrections;
}
const std::string& lists() {
    return config::cleanup_sentences().spoken_lists;
}
const std::string& paragraphs() {
    return config::cleanup_sentences().paragraphs;
}
const std::string& tighten() {
    return config::cleanup_sentences().tighten;
}
const std::string& light_only() {
    return config::cleanup_sentences().light_only;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

} // namespace

// --- presets ------------------------------------------------------------------

TEST_CASE("test_every_level_keeps_the_guard_and_the_language", "[test_personalize_s2_prompts]") {
    // from tests/test_personalize_s2_prompts.py::test_every_level_keeps_the_guard_and_the_language
    const CleanupLevel level = GENERATE(from_range(text::STORED_LEVELS));
    const std::string& prompt = text::preset(level);
    CHECK_THAT(prompt, StartsWith(config::cleanup_sentences().intro));
    CHECK(contains(prompt, config::cleanup_sentences().basics));
    CHECK(contains(prompt, config::cleanup_sentences().language));
    CHECK(contains(prompt, "never translate"));
    CHECK_THAT(prompt, EndsWith(config::cleanup_sentences().guard));
    for (const char* phrase : {"Do not invent content", "preserve meaning, tone, and proper nouns",
                               "no preamble or quotes"}) {
        CHECK(contains(prompt, phrase));
    }
}

TEST_CASE("test_each_level_adds_only_its_own_sentences", "[test_personalize_s2_prompts]") {
    // from tests/test_personalize_s2_prompts.py::test_each_level_adds_only_its_own_sentences
    struct Row {
        CleanupLevel level;
        std::vector<std::string> included;
        std::vector<std::string> left_out;
    };
    const std::vector<Row> rows{
        {CleanupLevel::Light, {light_only()}, {corrections(), lists(), paragraphs(), tighten()}},
        {CleanupLevel::Medium, {corrections(), lists(), paragraphs()}, {light_only(), tighten()}},
        {CleanupLevel::High, {corrections(), lists(), paragraphs(), tighten()}, {light_only()}},
    };
    for (const Row& row : rows) {
        INFO(text::to_string(row.level));
        const std::string& prompt = text::preset(row.level);
        for (const std::string& sentence : row.included) {
            CHECK(contains(prompt, sentence));
        }
        for (const std::string& sentence : row.left_out) {
            CHECK_FALSE(contains(prompt, sentence));
        }
    }
}

TEST_CASE("test_the_corrections_sentence_has_a_positive_and_a_negative_example",
          "[test_personalize_s2_prompts]") {
    // from
    // tests/test_personalize_s2_prompts.py::test_the_corrections_sentence_has_a_positive_and_a_negative_example
    for (const char* cue :
         {"actually", "no wait", "sorry", "scratch that", "I mean", "make that"}) {
        CHECK(contains(corrections(), "\"" + std::string(cue) + "\""));
    }
    CHECK(contains(corrections(), R"("let's meet at 2, actually 3" becomes "Let's meet at 3.")"));
    CHECK(contains(corrections(), R"("I actually like it.")"));
    for (const char* cue : {"first", "one... two", "bullet", "number one"}) {
        CHECK(contains(lists(), cue));
    }
    CHECK(contains(lists(), "one item per line"));
    CHECK(contains(lists(), "keep it as prose"));
}

TEST_CASE("test_the_presets_are_read_only_and_the_old_default_is_kept_for_comparison",
          "[test_personalize_s2_prompts]") {
    // from
    // tests/test_personalize_s2_prompts.py::test_the_presets_are_read_only_and_the_old_default_is_kept_for_comparison
    // The presets are const data here (Python checks a MappingProxyType refuses assignment).
    CHECK(config::legacy_default_cleanup_prompts() ==
          std::vector<std::string>{config::transcript_cleanup_prompt()});
    CHECK(contains(config::transcript_cleanup_prompt(), "speech-to-text"));
    for (const auto& [level, prompt] : config::level_prompts()) {
        CHECK(prompt != config::transcript_cleanup_prompt());
    }
    CHECK(text::legacy_default_prompt() == config::transcript_cleanup_prompt());
}

TEST_CASE("base_prompt at High is the eight sentences joined by one space") {
    const auto& s = config::cleanup_sentences();
    const std::string expected = s.intro + " " + s.basics + " " + s.tighten + " " +
                                 s.spoken_corrections + " " + s.spoken_lists + " " + s.paragraphs +
                                 " " + s.language + " " + s.guard;
    CHECK(text::base_prompt(on(), CleanupLevel::High) == expected);
    for (const auto& [level, prompt] : config::level_prompts()) {
        std::string joined;
        for (const std::string& name : config::level_sentence_names(level)) {
            joined += (joined.empty() ? "" : " ") + config::sentence(name);
        }
        CHECK(joined == prompt);
    }
}

// --- levels -------------------------------------------------------------------

TEST_CASE("test_resolve_level_follows_the_switch_and_validates_the_level",
          "[test_personalize_s2_prompts]") {
    // from
    // tests/test_personalize_s2_prompts.py::test_resolve_level_follows_the_switch_and_validates_the_level
    const std::vector<std::pair<Json, std::string>> rows{
        {Json::object(), "none"},
        {Json{{"transcript_cleanup_level", "high"}}, "none"},
        {Json{{"transcript_cleanup_enabled", "yes"}}, "none"},
        {on(), "medium"},
        {on({{"transcript_cleanup_level", "light"}}), "light"},
        {on({{"transcript_cleanup_level", "high"}}), "high"},
        {on({{"transcript_cleanup_level", "none"}}), "medium"},
        {on({{"transcript_cleanup_level", "HIGH"}}), "medium"},
        {on({{"transcript_cleanup_level", 3}}), "medium"},
    };
    for (const auto& [settings, level] : rows) {
        INFO(settings.dump());
        CHECK(text::to_string(text::resolve_level(settings)) == level);
    }
}

TEST_CASE("test_level_label", "[test_personalize_s2_prompts]") {
    // from tests/test_personalize_s2_prompts.py::test_level_label
    const std::vector<std::pair<Json, std::string>> rows{
        {Json::object(), "Off"},
        {Json{{"transcript_cleanup_prompt", "Write like a pirate."}}, "Off"},
        {on(), "Medium"},
        {on({{"transcript_cleanup_level", "light"}}), "Light"},
        {on({{"transcript_cleanup_level", "high"}}), "High"},
        {on({{"transcript_cleanup_prompt", "Write like a pirate."}}), "Custom"},
        {on({{"transcript_cleanup_prompt", config::transcript_cleanup_prompt()}}), "Medium"},
    };
    for (const auto& [settings, label] : rows) {
        INFO(settings.dump());
        CHECK(text::level_label(settings) == label);
    }
}

// --- custom prompts -----------------------------------------------------------

TEST_CASE("test_built_in_prompts_are_not_custom", "[test_personalize_s2_prompts]") {
    // from tests/test_personalize_s2_prompts.py::test_built_in_prompts_are_not_custom
    const std::string& old = config::transcript_cleanup_prompt();
    std::string rewrapped = old;
    for (std::size_t at = rewrapped.find(". "); at != std::string::npos;
         at = rewrapped.find(". ", at + 2)) {
        rewrapped.replace(at, 2, ".\n");
    }
    std::vector<Json> stored{nullptr, "", "  \n ", 42, old, "\n  " + old + "  ", rewrapped};
    for (const auto& [level, prompt] : config::level_prompts()) {
        stored.emplace_back(prompt);
    }
    stored.emplace_back("  " + text::preset(CleanupLevel::High) + "\n");
    for (const Json& value : stored) {
        INFO(value.dump());
        const Json settings =
            value.is_null() ? Json::object() : Json{{"transcript_cleanup_prompt", value}};
        CHECK(text::custom_prompt(settings).empty());
        for (const CleanupLevel level : text::STORED_LEVELS) {
            CHECK(text::base_prompt(settings, level) == text::preset(level));
        }
    }
}

TEST_CASE("test_a_custom_prompt_replaces_every_preset", "[test_personalize_s2_prompts]") {
    // from tests/test_personalize_s2_prompts.py::test_a_custom_prompt_replaces_every_preset
    const Json settings = on({{"transcript_cleanup_prompt", "  Make this a Slack message.  "}});
    CHECK(text::custom_prompt(settings) == "Make this a Slack message.");
    for (const CleanupLevel level :
         {CleanupLevel::Light, CleanupLevel::Medium, CleanupLevel::High, CleanupLevel::None}) {
        CHECK(text::base_prompt(settings, level) == "Make this a Slack message.");
    }
    CHECK(text::resolve_transcript_cleanup_prompt(settings) == "Make this a Slack message.");
}

TEST_CASE("test_with_cleanup_off_the_base_prompt_is_the_saved_levels_preset",
          "[test_personalize_s2_prompts]") {
    // from
    // tests/test_personalize_s2_prompts.py::test_with_cleanup_off_the_base_prompt_is_the_saved_levels_preset
    const Json settings{{"transcript_cleanup_level", "light"}};
    CHECK(text::base_prompt(settings, CleanupLevel::None) == text::preset("light"));
    CHECK(text::base_prompt(Json::object(), CleanupLevel::None) == text::preset("medium"));
    CHECK(text::base_prompt(Json::object(), "bogus") == text::preset("medium"));
    CHECK(text::resolve_transcript_cleanup_prompt(settings) == text::preset("light"));
}

TEST_CASE("test_the_saved_prompt_default_is_empty_so_no_install_starts_custom",
          "[test_personalize_s2_prompts]") {
    // from
    // tests/test_personalize_s2_prompts.py::test_the_saved_prompt_default_is_empty_so_no_install_starts_custom
    CHECK(fv::keys::defaults_table().at("transcript_cleanup_prompt") == "");
    CHECK(text::resolve_transcript_cleanup_prompt(Json::object()) == text::preset("medium"));
}

// --- tests/test_settings.py TestTranscriptCleanupRules ---------------------------------------

TEST_CASE("TestTranscriptCleanupRules::test_missing_key_returns_empty_list", "[test_settings]") {
    // from tests/test_settings.py::TestTranscriptCleanupRules::test_missing_key_returns_empty_list
    CHECK(text::resolve_transcript_cleanup_rules(Json::object()).empty());
}

TEST_CASE("TestTranscriptCleanupRules::test_non_list_value_returns_empty_list", "[test_settings]") {
    // from
    // tests/test_settings.py::TestTranscriptCleanupRules::test_non_list_value_returns_empty_list
    CHECK(text::resolve_transcript_cleanup_rules(Json{{"transcript_cleanup_rules", "not a list"}})
              .empty());
    CHECK(text::resolve_transcript_cleanup_rules(Json{{"transcript_cleanup_rules", Json{{"a", 1}}}})
              .empty());
    CHECK(text::resolve_transcript_cleanup_rules(Json{{"transcript_cleanup_rules", nullptr}})
              .empty());
}

TEST_CASE("TestTranscriptCleanupRules::test_entries_are_filtered_and_stripped", "[test_settings]") {
    // from
    // tests/test_settings.py::TestTranscriptCleanupRules::test_entries_are_filtered_and_stripped
    const Json rules = Json::array({"  Rule one  ", "", "   ", 42, nullptr, "Rule two"});
    CHECK(text::resolve_transcript_cleanup_rules(Json{{"transcript_cleanup_rules", rules}}) ==
          std::vector<std::string>{"Rule one", "Rule two"});
}

TEST_CASE("TestTranscriptCleanupRules::test_rules_capped_at_config_limit", "[test_settings]") {
    // from tests/test_settings.py::TestTranscriptCleanupRules::test_rules_capped_at_config_limit
    Json rules = Json::array();
    for (std::size_t i = 0; i < config::MAX_TRANSCRIPT_CLEANUP_RULES + 10; ++i) {
        rules.push_back("Rule " + std::to_string(i));
    }
    CHECK(
        text::resolve_transcript_cleanup_rules(Json{{"transcript_cleanup_rules", rules}}).size() ==
        config::MAX_TRANSCRIPT_CLEANUP_RULES);
}

TEST_CASE("TestTranscriptCleanupRules::test_compose_without_rules_returns_base_unchanged",
          "[test_settings]") {
    // from
    // tests/test_settings.py::TestTranscriptCleanupRules::test_compose_without_rules_returns_base_unchanged
    CHECK(text::compose_transcript_cleanup_prompt("Base prompt.", {}) == "Base prompt.");
}

TEST_CASE("TestTranscriptCleanupRules::test_compose_appends_numbered_rules", "[test_settings]") {
    // from tests/test_settings.py::TestTranscriptCleanupRules::test_compose_appends_numbered_rules
    const std::string result = text::compose_transcript_cleanup_prompt(
        "Base prompt.", {"Spell \"Alex Rivera\".", "Expand SCWA."});
    CHECK_THAT(result, StartsWith("Base prompt.\n\n"));
    CHECK_THAT(result, ContainsSubstring("Additional user-taught rules (always apply):"));
    CHECK_THAT(result, ContainsSubstring("1. Spell \"Alex Rivera\"."));
    CHECK_THAT(result, ContainsSubstring("2. Expand SCWA."));
}

TEST_CASE("TestTranscriptCleanupRules::test_rules_round_trip_through_settings_file",
          "[test_settings]") {
    // from
    // tests/test_settings.py::TestTranscriptCleanupRules::test_rules_round_trip_through_settings_file
    TempDir temp;
    fv::SettingsStore store(temp.path() / "settings.json");
    const std::vector<std::string> rules{"Always spell my name \"Alex Rivera\".",
                                         "Use bullet lists."};
    store.save_setting("transcript_cleanup_rules", rules);
    CHECK(text::resolve_transcript_cleanup_rules(*store.snapshot()) == rules);
}

// --- tests/test_transcript_cleanup.py TestTranscriptCleanupSettings ----------------------------

TEST_CASE("TestTranscriptCleanupSettings::test_resolve_cleanup_prompt_custom_and_fallback",
          "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestTranscriptCleanupSettings::test_resolve_cleanup_prompt_custom_and_fallback
    const std::string custom = "Make this a Slack message.";
    CHECK(text::resolve_transcript_cleanup_prompt(Json{{"transcript_cleanup_prompt", custom}}) ==
          custom);
    const std::string& medium = text::preset("medium");
    CHECK(text::resolve_transcript_cleanup_prompt(Json{{"transcript_cleanup_prompt", "   "}}) ==
          medium);
    CHECK(text::resolve_transcript_cleanup_prompt(Json::object()) == medium);
}

TEST_CASE("TestTranscriptCleanupSettings::test_save_and_load_cleanup_prompt",
          "[test_transcript_cleanup]") {
    // from
    // tests/test_transcript_cleanup.py::TestTranscriptCleanupSettings::test_save_and_load_cleanup_prompt
    TempDir temp;
    fv::SettingsStore store(temp.path() / "settings.json");
    const std::string custom = "Format as meeting notes with bullets.";
    store.save_setting(fv::keys::transcript_cleanup_prompt.key(), custom);
    CHECK(store.value(fv::keys::transcript_cleanup_prompt.key()) == custom);
}

// --- the inline-lists block --------------------------------------------------------------

TEST_CASE("the inline-lists block applies at Medium and High without a custom prompt") {
    fv::focus::FocusSnapshot terminal;
    terminal.identity = fv::focus::AppIdentity();
    terminal.identity->app_id = "windowsterminal.exe";
    terminal.identity->name = "Windows Terminal";
    fv::focus::FocusSnapshot notepad;
    notepad.identity = fv::focus::AppIdentity();
    notepad.identity->app_id = "notepad.exe";
    notepad.identity->name = "Notepad";
    const auto& s = config::cleanup_sentences();
    CHECK(text::inline_lists_block(on(), std::nullopt) == s.unknown_app_lines);
    CHECK(text::inline_lists_block(on(), fv::focus::FocusSnapshot{}) == s.unknown_app_lines);
    CHECK(text::inline_lists_block(on(), terminal) == s.terminal_lines);
    CHECK(text::inline_lists_block(on(), notepad).empty());
    CHECK(text::inline_lists_block(on({{"transcript_cleanup_level", "light"}}), terminal).empty());
    CHECK(text::inline_lists_block(on({{"transcript_cleanup_prompt", "Bullets."}}), std::nullopt)
              .empty());
    CHECK(text::inline_lists_block(Json::object(), std::nullopt).empty());
    // The terminal check is injectable, as Python's tests patch synthetic_keys.is_terminal.
    const auto everything = [](const fv::focus::AppIdentity&) {
        return true;
    };
    CHECK(text::inline_lists_block(on(), notepad, everything) == s.terminal_lines);
}

// --- golden vectors -------------------------------------------------------------------------

TEST_CASE("golden config", "[golden]") {
    golden::for_each("services.cleanup_prompts", "config", [](const golden::Case& c) {
        const std::string name = c.arg<std::string>(0);
        const Json expected = c.result<Json>();
        Json got;
        if (name == "TRANSCRIPT_CLEANUP_ENABLED") {
            got = config::TRANSCRIPT_CLEANUP_ENABLED;
        } else if (name == "TRANSCRIPT_CLEANUP_TIMEOUT_S") {
            got = config::TRANSCRIPT_CLEANUP_TIMEOUT_S;
        } else if (name == "TRANSCRIPT_CLEANUP_MAX_RETRIES") {
            got = config::TRANSCRIPT_CLEANUP_MAX_RETRIES;
        } else if (name == "TRANSCRIPT_CLEANUP_PROVIDER") {
            got = config::TRANSCRIPT_CLEANUP_PROVIDER;
        } else if (name == "TRANSCRIPT_CLEANUP_MODEL") {
            got = config::TRANSCRIPT_CLEANUP_MODEL;
        } else if (name == "TRANSCRIPT_CLEANUP_OPENROUTER_MODEL") {
            got = config::TRANSCRIPT_CLEANUP_OPENROUTER_MODEL;
        } else if (name == "TRANSCRIPT_CLEANUP_MODEL_SORT") {
            got = config::TRANSCRIPT_CLEANUP_MODEL_SORT;
        } else if (name == "TRANSCRIPT_CLEANUP_REASONING") {
            got = config::TRANSCRIPT_CLEANUP_REASONING;
        } else if (name == "OPENROUTER_BASE_URL") {
            got = config::OPENROUTER_BASE_URL;
        } else if (name == "TRANSCRIPT_CLEANUP_PROMPT") {
            got = config::transcript_cleanup_prompt();
        } else if (name == "LEGACY_DEFAULT_CLEANUP_PROMPTS") {
            got = config::legacy_default_cleanup_prompts();
        } else if (name == "TRANSCRIPT_CLEANUP_LEVEL") {
            got = config::TRANSCRIPT_CLEANUP_LEVEL;
        } else if (name == "TRANSCRIPT_CLEANUP_LEVEL_PROMPTS") {
            got = Json::object();
            for (const auto& [level, prompt] : config::level_prompts()) {
                got[level] = prompt;
            }
        } else if (name == "MAX_TRANSCRIPT_CLEANUP_RULES") {
            got = config::MAX_TRANSCRIPT_CLEANUP_RULES;
        } else if (name == "MAX_DICTIONARY_TERMS") {
            got = config::MAX_DICTIONARY_TERMS;
        } else if (name == "MAX_RECOGNITION_PHRASES") {
            got = config::MAX_RECOGNITION_PHRASES;
        } else if (name == "MAX_SNIPPETS") {
            got = config::MAX_SNIPPETS;
        } else if (name == "MAX_TEXT_TRANSFORMS") {
            got = config::MAX_TEXT_TRANSFORMS;
        } else if (name == "CONTEXT_BEFORE_CHARS") {
            got = config::CONTEXT_BEFORE_CHARS;
        } else if (name == "CONTEXT_AFTER_CHARS") {
            got = config::CONTEXT_AFTER_CHARS;
        } else if (name == "CONTEXT_CAPTURE_DEADLINE_S") {
            got = config::CONTEXT_CAPTURE_DEADLINE_S;
        } else if (name == "COMMAND_SELECTION_TIMEOUT_MS") {
            got = config::COMMAND_SELECTION_TIMEOUT_MS;
        } else if (name == "TRANSCRIPT_CLEANUP_RULE_POLISH_PROMPT") {
            got = config::rule_polish_prompt();
        } else if (name == "TRANSCRIPT_BATCH_CLEANUP_TIMEOUT_S") {
            got = config::TRANSCRIPT_BATCH_CLEANUP_TIMEOUT_S;
        } else if (name == "TRANSCRIPT_BATCH_CLEANUP_MAX_RETRIES") {
            got = config::TRANSCRIPT_BATCH_CLEANUP_MAX_RETRIES;
        } else {
            got = config::sentence(name);
        }
        CHECK(golden::equal(expected, got));
    });
}

TEST_CASE("golden is_built_in", "[golden]") {
    golden::for_each("services.cleanup_prompts", "is_built_in", [](const golden::Case& c) {
        CHECK(text::is_built_in(c.arg<std::string>(0)) == c.result<bool>());
    });
}

TEST_CASE("golden resolve_level", "[golden]") {
    golden::for_each("services.cleanup_prompts", "resolve_level", [](const golden::Case& c) {
        CHECK(text::to_string(text::resolve_level(c.args().at(0))) == c.result<std::string>());
    });
}

TEST_CASE("golden custom_prompt", "[golden]") {
    golden::for_each("services.cleanup_prompts", "custom_prompt", [](const golden::Case& c) {
        CHECK(text::custom_prompt(c.args().at(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden preset", "[golden]") {
    golden::for_each("services.cleanup_prompts", "preset", [](const golden::Case& c) {
        CHECK(text::preset(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden base_prompt", "[golden]") {
    golden::for_each("services.cleanup_prompts", "base_prompt", [](const golden::Case& c) {
        CHECK(text::base_prompt(c.args().at(0), c.arg<std::string>(1)) == c.result<std::string>());
    });
}

TEST_CASE("golden inline_lists_block", "[golden]") {
    golden::for_each("services.cleanup_prompts", "inline_lists_block", [](const golden::Case& c) {
        CHECK(text::inline_lists_block(c.args().at(0), snapshot_from(c.args().at(1))) ==
              c.result<std::string>());
    });
}

TEST_CASE("golden level_label", "[golden]") {
    golden::for_each("services.cleanup_prompts", "level_label", [](const golden::Case& c) {
        CHECK(text::level_label(c.args().at(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden resolve_transcript_cleanup_prompt", "[golden]") {
    golden::for_each("services.settings", "resolve_transcript_cleanup_prompt",
                     [](const golden::Case& c) {
                         CHECK(text::resolve_transcript_cleanup_prompt(c.args().at(0)) ==
                               c.result<std::string>());
                     });
}

TEST_CASE("golden resolve_transcript_cleanup_rules", "[golden]") {
    golden::for_each("services.settings", "resolve_transcript_cleanup_rules",
                     [](const golden::Case& c) {
                         CHECK(text::resolve_transcript_cleanup_rules(c.args().at(0)) ==
                               c.result<std::vector<std::string>>());
                     });
}

TEST_CASE("golden compose_transcript_cleanup_prompt", "[golden]") {
    golden::for_each("services.settings", "compose_transcript_cleanup_prompt",
                     [](const golden::Case& c) {
                         CHECK(text::compose_transcript_cleanup_prompt(
                                   c.arg<std::string>(0), c.arg<std::vector<std::string>>(1)) ==
                               c.result<std::string>());
                     });
}

TEST_CASE("golden cleanup_levels tables", "[golden]") {
    namespace levels = fv::text::cleanup_levels;
    golden::for_each("ui_qt.dialogs.cleanup_levels", "SEGMENTS", [](const golden::Case& c) {
        Json got = Json::array();
        for (const auto& segment : levels::SEGMENTS) {
            got.push_back(Json::array({segment.value, segment.title, segment.detail}));
        }
        CHECK(golden::equal(c.result<Json>(), got));
    });
    for (const char* table : {"NOTES", "SHORT_NOTES"}) {
        golden::for_each("ui_qt.dialogs.cleanup_levels", table, [&](const golden::Case& c) {
            const auto& rows = std::string(table) == "NOTES" ? levels::NOTES : levels::SHORT_NOTES;
            Json got = Json::object();
            for (const auto& [value, note] : rows) {
                got[std::string(value)] = note;
                CHECK((std::string(table) == "NOTES" ? levels::note(value)
                                                     : levels::short_note(value)) == note);
            }
            CHECK(golden::equal(c.result<Json>(), got));
        });
    }
}

TEST_CASE("golden segment_index", "[golden]") {
    golden::for_each("ui_qt.dialogs.cleanup_levels", "segment_index", [](const golden::Case& c) {
        CHECK(fv::text::cleanup_levels::segment_index(c.arg<std::string>(0)) ==
              c.result<std::size_t>());
    });
}
