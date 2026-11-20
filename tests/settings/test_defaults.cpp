// tests/test_setting_defaults.py: the single table of setting defaults and the generic
// resolvers, and the golden replay of services.settings (tests/golden/services.settings.json).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "golden/golden.h"
#include "settings/hotkeys.h"
#include "settings/keys.h"
#include "settings/model_selection.h"
#include "settings/resolve.h"
#include "settings/store.h"
#include "settings_test_support.h"

using fv::Json;
using fv::settings_test::same;
using fv::settings_test::untag;

TEST_CASE("every default belongs to a settings key", "[test_setting_defaults]") {
    // from tests/test_setting_defaults.py::test_every_default_belongs_to_a_settings_key
    for (const auto& [key, value] : fv::keys::defaults_table().items()) {
        const fv::SettingBase* setting = fv::keys::by_key(key);
        REQUIRE(setting != nullptr);
        CHECK(setting->in_table());
    }
}

TEST_CASE("the table is read only", "[test_setting_defaults]") {
    // from tests/test_setting_defaults.py::test_the_table_is_read_only
    // The table is handed out as a const reference; a caller that wants to change a default
    // takes a copy, which leaves the table as it was.
    static_assert(std::is_same_v<decltype(fv::keys::defaults_table()), const Json&>);
    Json copy = fv::keys::defaults_table();
    copy["auto_paste"] = false;
    CHECK(fv::keys::defaults_table().at("auto_paste") == true);
    CHECK(&fv::keys::defaults_table() == &fv::keys::defaults_table());
}

TEST_CASE("defaults match the same named config values", "[test_setting_defaults][golden]") {
    // from tests/test_setting_defaults.py::test_defaults_match_the_same_named_config_values
    // The generator evaluates config on Windows x64 into keys.inc; the vectors hold
    // SETTING_DEFAULTS as OpenWhisper built it there, in declaration order.
    fv::settings_test::for_each_case(
        "services.settings", "SETTING_DEFAULTS", [](const fv::golden::Case& c) {
            const Json expected = untag(c.result<Json>());
            CHECK(same(expected, fv::keys::defaults_table()));
            CHECK(fv::keys::defaults_table().at("transcript_cleanup_model") == "openrouter/free");
        });
}

TEST_CASE("setting value returns the stored value unvalidated", "[test_setting_defaults]") {
    // from tests/test_setting_defaults.py::test_setting_value_returns_the_stored_value_unvalidated
    CHECK(same(Json(true), fv::setting_value("auto_paste", Json::object())));
    CHECK(same(Json(0), fv::setting_value("auto_paste", Json{{"auto_paste", 0}})));
    CHECK(fv::setting_value("whisper_device", Json::object()) == "auto");
}

TEST_CASE("setting value reads the file when no mapping is given", "[test_setting_defaults]") {
    // from
    // tests/test_setting_defaults.py::test_setting_value_reads_the_file_when_no_mapping_is_given
    // There is no module-level store; a caller passes the store's snapshot.
    fv::settings_test::TempDir temp;
    fv::SettingsStore store(temp.path() / "settings.json");
    store.save_all(Json{{"local_asr_language", "auto"}});
    CHECK(fv::setting_value("local_asr_language", *store.snapshot()) == "auto");
    CHECK(fv::setting_value("copy_clipboard", *store.snapshot()) == true);
}

TEST_CASE("bool and choice resolvers fall back to the table", "[test_setting_defaults]") {
    // from tests/test_setting_defaults.py::test_bool_and_choice_resolvers_fall_back_to_the_table
    const Json corrupt{{"typesafe_topic_shift_enabled", "yes"},
                       {"meeting_server_bind", "everywhere"}};
    CHECK(fv::resolve_bool_setting("typesafe_topic_shift_enabled", corrupt) == true);
    const std::vector<Json> binds{"localhost", "lan"};
    CHECK(fv::resolve_choice_setting("meeting_server_bind", binds, corrupt) == "localhost");
}

TEST_CASE("generated bool resolvers keep their call shape", "[test_setting_defaults]") {
    // from tests/test_setting_defaults.py::test_generated_bool_resolvers_keep_their_call_shape
    // resolve_<key>(settings) became keys::<key>.read(settings), a bool for each of them.
    const Json empty = Json::object();
    CHECK(fv::keys::developer_mode.read(empty) == false);
    CHECK(fv::keys::meeting_cloud_consent_given.read(empty) == false);
    CHECK(fv::keys::meeting_end_polish.read(empty) == true);
    CHECK(fv::keys::typesafe_enabled.read(empty) == false);
    CHECK(fv::keys::update_check_enabled.read(empty) == true);
    static_assert(std::is_same_v<decltype(fv::keys::update_check_enabled.read(empty)), bool>);
}

TEST_CASE("setting value matches OpenWhisper's for every key", "[test_settings][golden]") {
    fv::settings_test::for_each_case(
        "services.settings", "setting_value", [](const fv::golden::Case& c) {
            const auto key = c.arg<std::string>(0);
            const Json snapshot = untag(c.arg<Json>(1));
            if (c.raises()) {
                CHECK(c.raises_type() == "KeyError");
                CHECK_THROWS_AS(fv::setting_value(key, snapshot), std::out_of_range);
                return;
            }
            CHECK(same(untag(c.result<Json>()), fv::setting_value(key, snapshot)));
        });
}

TEST_CASE("resolve bool setting matches OpenWhisper's for every key",
          "[test_typed_settings][golden]") {
    // from
    // tests/test_typed_settings.py::test_resolve_bool_setting_matches_the_old_one_for_every_on_off_key
    fv::settings_test::for_each_case(
        "services.settings", "resolve_bool_setting", [](const fv::golden::Case& c) {
            const auto key = c.arg<std::string>(0);
            const Json snapshot = untag(c.arg<Json>(1));
            if (c.raises_type() == "TypeError") {
                CHECK_THROWS_AS(fv::resolve_bool_setting(key, snapshot), std::invalid_argument);
            } else if (c.raises_type() == "KeyError") {
                CHECK_THROWS_AS(fv::resolve_bool_setting(key, snapshot), std::out_of_range);
            } else {
                REQUIRE_FALSE(c.raises());
                CHECK(fv::resolve_bool_setting(key, snapshot) == c.result<bool>());
            }
        });
}

TEST_CASE("resolve choice setting matches OpenWhisper's", "[test_settings][golden]") {
    fv::settings_test::for_each_case(
        "services.settings", "resolve_choice_setting", [](const fv::golden::Case& c) {
            const auto key = c.arg<std::string>(0);
            std::vector<Json> choices;
            for (const auto& choice : c.arg<Json>(1)) {
                choices.push_back(choice);
            }
            const Json snapshot = untag(c.arg<Json>(2));
            if (c.raises()) {
                CHECK_THROWS_AS(fv::resolve_choice_setting(key, choices, snapshot),
                                std::out_of_range);
                return;
            }
            CHECK(
                same(untag(c.result<Json>()), fv::resolve_choice_setting(key, choices, snapshot)));
        });
}

TEST_CASE("load hotkey settings matches OpenWhisper's", "[test_settings][golden]") {
    fv::settings_test::for_each_case(
        "services.settings", "load_hotkey_settings", [](const fv::golden::Case& c) {
            fv::settings_test::TempDir temp;
            const auto path = temp.path() / "settings.json";
            const Json stored = c.arg<Json>(0);
            if (!stored.is_null()) {
                fv::settings_test::write_bytes(path, stored.dump());
            }
            const fv::SettingsStore store(path);
            CHECK(same(c.result<Json>(), fv::load_hotkey_settings(store)));
        });
}

TEST_CASE("load model selection matches OpenWhisper's", "[test_settings][golden]") {
    // Saved names of engines FioriVoce does not have read as the default and stay saved.
    static const std::set<std::string> kDropped{
        "local_whisper", "qwen_asr", "nemotron", "moonshine", "parakeet_mlx", "parakeet_unified"};
    fv::settings_test::for_each_case(
        "services.settings", "load_model_selection", [](const fv::golden::Case& c) {
            fv::settings_test::TempDir temp;
            const auto path = temp.path() / "settings.json";
            const Json stored = c.arg<Json>(0);
            if (!stored.is_null()) {
                fv::settings_test::write_bytes(path, stored.dump());
            }
            const std::string iso = c.arg<std::string>(1); // YYYY-MM-DD
            const int year = std::stoi(iso.substr(0, 4));
            const auto month = static_cast<unsigned>(std::stoi(iso.substr(5, 2)));
            const auto day = static_cast<unsigned>(std::stoi(iso.substr(8, 2)));
            fv::SettingsStore store(path);
            const std::string result = fv::load_model_selection(
                store,
                std::chrono::year_month_day{std::chrono::year{year}, std::chrono::month{month},
                                            std::chrono::day{day}});
            std::string expected = c.result<Json>().at("result").get<std::string>();
            if (kDropped.contains(expected)) {
                expected = "parakeet";
            }
            CHECK(result == expected);
            const Json file = c.result<Json>().at("file");
            if (file.is_null()) {
                CHECK_FALSE(std::filesystem::exists(path));
            } else {
                CHECK(same(file, *store.snapshot()));
            }
        });
}
