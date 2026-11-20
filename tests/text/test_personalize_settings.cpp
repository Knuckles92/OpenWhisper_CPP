// tests/test_personalize_foundation_settings.py: the personalization keys, their defaults and
// resolvers, the limits, tones and levels, and new-install seeding (over
// settings/install.h's seed_new_install_settings).

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "base/json.h"
#include "focus/snapshot.h"
#include "hotkeys/hold_machine.h"
#include "prompt_test_support.h"
#include "settings/install.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "text/app_styles.h"
#include "text/cleanup_config.h"
#include "text/cleanup_prompts.h"
#include "text/transforms.h"

using fv::Json;
using fv::prompt_test::TempDir;
namespace text = fv::text;
namespace config = fv::text::config;
namespace styles = fv::text::app_styles;

namespace {

Json read_json(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return Json::parse(std::string(std::istreambuf_iterator<char>(in), {}));
}

Json new_install_tones() {
    Json tones = Json::object();
    for (const auto& [category, tone] : fv::kNewInstallAppStyleTones) {
        tones[std::string(category)] = tone;
    }
    return tones;
}

} // namespace

TEST_CASE("test_every_design_key_has_its_default", "[test_personalize_foundation_settings]") {
    // from tests/test_personalize_foundation_settings.py::test_every_design_key_has_its_default
    const Json design = Json::parse(R"({
        "app_context_enabled": true,
        "app_context_read_text": false,
        "app_context_excluded_apps": [],
        "app_styles_enabled": true,
        "app_style_overrides": [],
        "transcript_cleanup_level": "medium",
        "command_mode_insert_without_selection": true,
        "dictation_dictionary": [],
        "dictionary_learn_enabled": true,
        "dictionary_steer_recognition": true,
        "dictation_snippets": [],
        "snippets_enabled": true,
        "recording_hands_free_latch": true,
        "audio_input_priority": [],
        "dictation_languages": [],
        "dictation_active_language": "",
        "scratchpad_always_on_top": true,
        "whats_new_seen": ""
    })");
    const Json& defaults = fv::keys::defaults_table();
    for (const auto& [key, value] : design.items()) {
        INFO(key);
        CHECK(fv::keys::by_key(key) != nullptr);
        REQUIRE(defaults.contains(key));
        CHECK(defaults.at(key) == value);
    }
    // Absence means something for these two, so they have no default.
    for (const char* key : {"app_style_tones", "text_transforms"}) {
        CHECK(fv::keys::by_key(key) != nullptr);
        CHECK_FALSE(defaults.contains(key));
    }
}

TEST_CASE("test_resolvers_validate_stored_values", "[test_personalize_foundation_settings]") {
    // from tests/test_personalize_foundation_settings.py::test_resolvers_validate_stored_values
    using fv::keys::transcript_cleanup_level;
    CHECK(transcript_cleanup_level.read(Json::object()) == "medium");
    CHECK(transcript_cleanup_level.read(Json{{"transcript_cleanup_level", "high"}}) == "high");
    CHECK(transcript_cleanup_level.read(Json{{"transcript_cleanup_level", "none"}}) == "medium");
    CHECK(fv::keys::app_context_enabled.read(Json::object()));
    CHECK_FALSE(fv::keys::app_context_read_text.read(Json{{"app_context_read_text", "yes"}}));
    CHECK_FALSE(
        fv::keys::recording_hands_free_latch.read(Json{{"recording_hands_free_latch", false}}));
}

TEST_CASE("test_limits_and_scratchpad_path", "[test_personalize_foundation_settings]") {
    // from tests/test_personalize_foundation_settings.py::test_limits_and_scratchpad_path
    // The Scratchpad's file name is fv_ui_record's (tests/ui_record "the notes live in the data
    // folder's scratchpad.txt"); fv_text cannot reach it.
    CHECK(config::MAX_DICTIONARY_TERMS == 500);
    CHECK(config::MAX_RECOGNITION_PHRASES == 50);
    CHECK(config::MAX_SNIPPETS == 200);
    CHECK(config::MAX_TEXT_TRANSFORMS == 50);
    CHECK(text::MAX_TRANSFORMS == 50);
    CHECK(config::CONTEXT_BEFORE_CHARS == 1000);
    CHECK(config::CONTEXT_AFTER_CHARS == 200);
    CHECK(config::CONTEXT_CAPTURE_DEADLINE_S == 0.4);
    CHECK(fv::focus::CONTEXT_CAPTURE_DEADLINE.count() == 400);
    CHECK(config::COMMAND_SELECTION_TIMEOUT_MS == 700);
    CHECK(fv::hotkeys::kRecordLatchWindowMs == 400);
}

// --- styles and levels ------------------------------------------------------

TEST_CASE("test_tones_are_all_formal_until_saved_and_validated",
          "[test_personalize_foundation_settings]") {
    // from
    // tests/test_personalize_foundation_settings.py::test_tones_are_all_formal_until_saved_and_validated
    CHECK(
        styles::resolve_tones(Json::object()) ==
        Json{{"email", "formal"}, {"work", "formal"}, {"personal", "formal"}, {"other", "formal"}});
    CHECK(styles::resolve_tones(Json{{"app_style_tones", new_install_tones()}}) ==
          new_install_tones());
    CHECK(
        styles::resolve_tones(
            Json::parse(R"({"app_style_tones": {"work": "shouty", "email": "casual"}})")) ==
        Json{{"email", "casual"}, {"work", "formal"}, {"personal", "formal"}, {"other", "formal"}});
    CHECK(
        new_install_tones() ==
        Json{{"email", "formal"}, {"work", "casual"}, {"personal", "casual"}, {"other", "formal"}});
}

TEST_CASE("test_level_is_none_while_cleanup_is_off", "[test_personalize_foundation_settings]") {
    // from tests/test_personalize_foundation_settings.py::test_level_is_none_while_cleanup_is_off
    CHECK(text::resolve_level(Json::object()) == text::CleanupLevel::None);
    CHECK(text::resolve_level(Json{{"transcript_cleanup_enabled", true}}) ==
          text::CleanupLevel::Medium);
    CHECK(text::resolve_level(
              Json{{"transcript_cleanup_enabled", true}, {"transcript_cleanup_level", "light"}}) ==
          text::CleanupLevel::Light);
}

TEST_CASE("test_only_a_real_custom_prompt_counts", "[test_personalize_foundation_settings]") {
    // from tests/test_personalize_foundation_settings.py::test_only_a_real_custom_prompt_counts
    const std::string& old = config::transcript_cleanup_prompt();
    const std::vector<std::pair<Json, std::string>> rows{
        {nullptr, ""},
        {"   ", ""},
        {old, ""},
        {"  " + old + "\n", ""},
        {"  Keep my British spelling. ", "Keep my British spelling."},
    };
    for (const auto& [stored, custom] : rows) {
        INFO(stored.dump());
        const Json settings =
            stored.is_null() ? Json::object() : Json{{"transcript_cleanup_prompt", stored}};
        CHECK(text::custom_prompt(settings) == custom);
        const std::string expected = custom.empty() ? text::preset("medium") : custom;
        CHECK(text::base_prompt(settings, "medium") == expected);
    }
}

// --- new-install seeding ----------------------------------------------------

TEST_CASE("test_a_brand_new_install_is_seeded_once", "[test_personalize_foundation_settings]") {
    // from tests/test_personalize_foundation_settings.py::test_a_brand_new_install_is_seeded_once
    TempDir temp;
    const auto path = temp.path() / "openwhisper_cpp_settings.json";
    fv::SettingsStore manager(path);
    CHECK(manager.is_new_install());

    CHECK(fv::seed_new_install_settings(manager));
    CHECK_FALSE(fv::seed_new_install_settings(manager));

    CHECK(read_json(path).at("app_style_tones") == new_install_tones());
    // A tone the user changes afterwards is never overwritten.
    manager.save_setting("app_style_tones", Json{{"work", "very_casual"}});
    CHECK_FALSE(fv::seed_new_install_settings(manager));
    CHECK(manager.value("app_style_tones") == Json{{"work", "very_casual"}});
}

TEST_CASE("test_an_existing_install_keeps_todays_output",
          "[test_personalize_foundation_settings]") {
    // from
    // tests/test_personalize_foundation_settings.py::test_an_existing_install_keeps_todays_output
    TempDir temp;
    const auto path = temp.path() / "openwhisper_cpp_settings.json";
    {
        std::ofstream out(path, std::ios::binary);
        out << R"({"ui_theme": "dark"})";
    }
    fv::SettingsStore manager(path);
    CHECK_FALSE(manager.is_new_install());
    CHECK_FALSE(fv::seed_new_install_settings(manager));
    CHECK_FALSE(manager.snapshot()->contains("app_style_tones"));
}
