// tests/test_cleanup_profiles.py (storage, validation and the profile prompt; the panel, Quick
// Record and controller tests are later packages'), two tests of
// tests/test_personalize_foundation_hotkeys.py and test_cleanup_may_run of
// tests/test_openai_lazy_import.py; plus the services.cleanup_profiles golden vectors.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "golden/golden.h"
#include "hotkeys/conflicts.h"
#include "hotkeys/defaults.h"
#include "prompt_test_support.h"
#include "pyport/errors.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "text/cleanup_profiles.h"
#include "text/transforms.h"

using Catch::Matchers::ContainsSubstring;
using fv::Json;
using fv::prompt_test::profile_from;
using fv::prompt_test::TempDir;
using fv::prompt_test::windows_context;
using fv::text::CleanupProfile;
namespace text = fv::text;
namespace golden = fv::golden;
namespace hotkeys = fv::hotkeys;

namespace {

std::string default_hotkey(std::string_view action) {
    for (const auto& [name, hotkey] : hotkeys::default_hotkeys()) {
        if (name == action) {
            return hotkey;
        }
    }
    return "";
}

std::string error_of(const std::function<void()>& run) {
    try {
        run();
    } catch (const fv::py::ValueError& error) {
        return error.what();
    }
    return "";
}

Json plain(const std::vector<CleanupProfile>& profiles) {
    Json out = Json::array();
    for (const CleanupProfile& profile : profiles) {
        out.push_back(text::profile_to_json(profile));
    }
    return out;
}

// The profiles and transforms of tests/test_personalize_foundation_hotkeys.py's fixture. Python
// patches text_transforms.load_transforms; here the transforms are saved in the settings.
Json foundation_settings() {
    Json profiles = Json::array();
    profiles.push_back(Json{{"id", "notes"},
                            {"name", "Meeting notes"},
                            {"instructions", "Bullets."},
                            {"hotkey", "ctrl+shift+f9"}});
    profiles.push_back(
        Json{{"id", "ticket"}, {"name", "Ticket"}, {"instructions", "A ticket."}, {"hotkey", ""}});
    Json transforms = Json::array();
    transforms.push_back(text::transform_to_json({"polish", "Polish", "Polish it.", "ctrl+alt+p"}));
    transforms.push_back(text::transform_to_json({"short", "Make concise", "Shorter.", ""}));
    return Json{{"transcript_cleanup_profiles", profiles}, {"text_transforms", transforms}};
}

} // namespace

TEST_CASE("test_starters_do_not_reappear_after_deleting_library", "[test_cleanup_profiles]") {
    // from tests/test_cleanup_profiles.py::test_starters_do_not_reappear_after_deleting_library
    Json settings{{"unrelated", "preserve"}};
    std::vector<std::string> names;
    for (const CleanupProfile& profile : text::load_cleanup_profiles(settings)) {
        names.push_back(profile.name);
    }
    CHECK(names == std::vector<std::string>{"Support ticket", "Email"});
    for (const CleanupProfile& profile : text::starter_profiles()) {
        text::delete_cleanup_profile(settings, profile.id);
    }
    CHECK(text::load_cleanup_profiles(settings).empty());
    CHECK(settings.at("unrelated") == "preserve");
}

TEST_CASE("test_profile_round_trip_and_delete_selected_profile", "[test_cleanup_profiles]") {
    // from tests/test_cleanup_profiles.py::test_profile_round_trip_and_delete_selected_profile
    TempDir temp;
    const auto path = temp.path() / "profiles.json";
    const CleanupProfile profile{"custom", "Release notes", "Group changes into bullets.",
                                 "ctrl+shift+f9", false};
    {
        fv::SettingsStore manager(path);
        manager.mutate([&](Json& s) { text::save_cleanup_profile(s, profile); });
        manager.save_setting(fv::keys::quick_record_profile.key(), profile.id);
    }
    fv::SettingsStore reloaded(path);
    CHECK(text::load_cleanup_profiles(*reloaded.snapshot()).back() == profile);
    reloaded.mutate([&](Json& s) { text::delete_cleanup_profile(s, profile.id); });
    CHECK(reloaded.value(fv::keys::quick_record_profile.key()) == "");
    for (const CleanupProfile& item : text::load_cleanup_profiles(*reloaded.snapshot())) {
        CHECK(item.id != profile.id);
    }
}

TEST_CASE("test_invalid_profile_never_changes_settings", "[test_cleanup_profiles]") {
    // from tests/test_cleanup_profiles.py::test_invalid_profile_never_changes_settings
    struct Row {
        std::string name, instructions, hotkey, reason;
    };
    const std::vector<Row> rows{
        {"", "Make bullets", "", "name"},
        {"Custom", " ", "", "instructions"},
        {"EMAIL", "Make bullets", "", "name already"},
        {"Custom", "Make bullets", default_hotkey("record_toggle"), "already used"},
    };
    for (const Row& row : rows) {
        INFO(row.reason);
        Json settings = Json::object();
        const std::string error = error_of([&] {
            text::save_cleanup_profile(
                settings, fv::prompt_test::profile("new", row.name, row.instructions, row.hotkey));
        });
        CHECK_THAT(error, ContainsSubstring(row.reason));
        CHECK(settings == Json::object());
    }
}

TEST_CASE("test_hotkey_conflicts_include_other_profiles_and_modifier_aliases",
          "[test_cleanup_profiles]") {
    // from
    // tests/test_cleanup_profiles.py::test_hotkey_conflicts_include_other_profiles_and_modifier_aliases
    Json settings = Json::object();
    CleanupProfile first = text::starter_profiles()[0];
    first.hotkey = "ctrl+alt+t";
    text::save_cleanup_profile(settings, first);
    CHECK(text::profile_hotkey_conflict("alt+control+t", settings) == "Support ticket");
    CHECK(text::profile_hotkey_conflict("ctrl+alt+t", settings, "support-ticket").empty());
    CleanupProfile second = text::starter_profiles()[1];
    second.hotkey = "ctrl+alt+t";
    CHECK_THAT(error_of([&] { text::save_cleanup_profile(settings, second); }),
               ContainsSubstring("Support ticket"));
}

TEST_CASE("test_malformed_profiles_are_skipped_without_crashing", "[test_cleanup_profiles]") {
    // from tests/test_cleanup_profiles.py::test_malformed_profiles_are_skipped_without_crashing
    const Json settings = Json::parse(R"({"transcript_cleanup_profiles": [
        null, "bad", {}, {"id": 4, "name": "Bad", "instructions": "Bad"},
        {"id": "ok", "name": " Good ", "instructions": " Do this ", "hotkey": 9},
        {"id": "ok", "name": "Duplicate", "instructions": "Skip"}]})");
    CHECK(text::load_cleanup_profiles(settings) ==
          std::vector<CleanupProfile>{fv::prompt_test::profile("ok", "Good", "Do this")});
}

TEST_CASE("test_profile_prompt_uses_format_and_optional_shared_rules", "[test_cleanup_profiles]") {
    // from
    // tests/test_cleanup_profiles.py::test_profile_prompt_uses_format_and_optional_shared_rules
    const CleanupProfile& profile = text::starter_profiles()[1];
    const std::string prompt = text::compose_profile_prompt(profile, {"Spell Acme correctly."});
    CHECK_THAT(prompt, ContainsSubstring(profile.instructions));
    CHECK_THAT(prompt, ContainsSubstring("Do not invent"));
    CHECK_THAT(prompt, ContainsSubstring("Spell Acme correctly."));
    CleanupProfile without = profile;
    without.use_learned_rules = false;
    CHECK_FALSE(text::compose_profile_prompt(without, {"Spell Acme correctly."})
                    .find("Spell Acme correctly.") != std::string::npos);
}

TEST_CASE("test_profile_wrapper_matches_the_shared_check",
          "[test_personalize_foundation_hotkeys]") {
    // from
    // tests/test_personalize_foundation_hotkeys.py::test_profile_wrapper_matches_the_shared_check
    const Json settings = foundation_settings();
    hotkeys::ConflictOptions shared;
    shared.bindings = text::hotkey_bindings(settings);
    for (const std::string& hotkey : {std::string("ctrl+shift+f9"), std::string("ctrl+alt+p"),
                                      default_hotkey("cancel"), std::string("f13")}) {
        INFO(hotkey);
        CHECK(text::profile_hotkey_conflict(hotkey, settings) ==
              hotkeys::hotkey_conflict(hotkey, settings, shared));
    }
    CHECK(text::profile_hotkey_conflict("ctrl+shift+f9", settings, "notes").empty());
    text::ShortcutContext unsaved;
    unsaved.standard_hotkeys = Json{{"command_mode", "ctrl+alt+c"}};
    CHECK(text::profile_hotkey_conflict("ctrl+alt+c", settings, "", unsaved) == "Command Mode");
}

TEST_CASE("test_transforms_are_read_through_their_module",
          "[test_personalize_foundation_hotkeys]") {
    // from
    // tests/test_personalize_foundation_hotkeys.py::test_transforms_are_read_through_their_module
    // Python counts calls to a patched load_transforms; here the bindings the shared check gets
    // are exactly what load_transforms reads from the same settings.
    Json settings{{"transcript_cleanup_profiles", Json::array()}};
    CHECK(text::profile_hotkey_conflict("ctrl+alt+q", settings).empty());
    const hotkeys::Bindings starters = text::hotkey_bindings(settings);
    REQUIRE(starters.transforms.size() == text::starter_transforms().size());
    settings["text_transforms"] =
        Json::array({text::transform_to_json({"q", "Quick", "Do it.", "ctrl+alt+q"})});
    const hotkeys::Bindings saved = text::hotkey_bindings(settings);
    REQUIRE(saved.transforms.size() == 1);
    CHECK(saved.transforms[0].name == "Quick");
    CHECK(text::profile_hotkey_conflict("ctrl+alt+q", settings) == "Quick");
}

TEST_CASE("test_cleanup_may_run", "[test_openai_lazy_import]") {
    // from tests/test_openai_lazy_import.py::test_cleanup_may_run
    const std::vector<std::pair<Json, bool>> rows{
        {Json::object(), false},
        {Json{{"transcript_cleanup_enabled", true}}, true},
        {Json{{"quick_record_profile", "email"}}, true},
        {Json::parse(R"({"transcript_cleanup_profiles": [
            {"id": "memo", "name": "Memo", "instructions": "Tidy.", "hotkey": "ctrl+alt+m"}]})"),
         true},
        {Json::parse(R"({"transcript_cleanup_profiles": [
            {"id": "memo", "name": "Memo", "instructions": "Tidy."}]})"),
         false},
    };
    for (const auto& [settings, expected] : rows) {
        INFO(settings.dump());
        CHECK(text::cleanup_may_run(settings) == expected);
    }
}

TEST_CASE("saved actions count as taken shortcuts when the caller passes them") {
    text::ShortcutContext context;
    context.actions.push_back({"a1", "Send to Slack", "ctrl+alt+s"});
    CHECK(text::profile_hotkey_conflict("alt+ctrl+s", Json::object(), "", context) ==
          "Send to Slack");
    CHECK_THAT(error_of([&] {
                   Json settings = Json::object();
                   text::save_cleanup_profile(
                       settings, fv::prompt_test::profile("n", "New", "Do.", "ctrl+alt+s"),
                       context);
               }),
               ContainsSubstring("Send to Slack"));
}

// --- golden vectors -------------------------------------------------------------------------

TEST_CASE("golden STARTER_PROFILES", "[golden]") {
    golden::for_each("services.cleanup_profiles", "STARTER_PROFILES", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), plain(text::starter_profiles())));
    });
}

TEST_CASE("golden load_cleanup_profiles", "[golden]") {
    golden::for_each(
        "services.cleanup_profiles", "load_cleanup_profiles", [](const golden::Case& c) {
            CHECK(golden::equal(
                c.result<Json>(),
                plain(text::load_cleanup_profiles(c.args().at(0), hotkeys::Vocabulary::Windows))));
        });
}

TEST_CASE("golden find_cleanup_profile", "[golden]") {
    golden::for_each(
        "services.cleanup_profiles", "find_cleanup_profile", [](const golden::Case& c) {
            const auto found = text::find_cleanup_profile(c.args().at(0), c.arg<std::string>(1),
                                                          hotkeys::Vocabulary::Windows);
            CHECK(golden::equal(c.result<Json>(), found ? text::profile_to_json(*found) : Json()));
        });
}

TEST_CASE("golden cleanup_may_run", "[golden]") {
    golden::for_each("services.cleanup_profiles", "cleanup_may_run", [](const golden::Case& c) {
        CHECK(text::cleanup_may_run(c.args().at(0)) == c.result<bool>());
    });
}

TEST_CASE("golden normalize_hotkey", "[golden]") {
    golden::for_each("services.cleanup_profiles", "normalize_hotkey", [](const golden::Case& c) {
        CHECK(text::normalize_hotkey(c.arg<std::string>(0), hotkeys::Vocabulary::Windows) ==
              c.result<std::string>());
    });
}

TEST_CASE("golden profile_hotkey_conflict", "[golden]") {
    golden::for_each("services.cleanup_profiles", "profile_hotkey_conflict",
                     [](const golden::Case& c) {
                         text::ShortcutContext context = windows_context();
                         if (!c.args().at(3).is_null()) {
                             context.standard_hotkeys = c.args().at(3);
                         }
                         CHECK(text::profile_hotkey_conflict(c.arg<std::string>(0), c.args().at(1),
                                                             c.arg<std::string>(2),
                                                             context) == c.result<std::string>());
                     });
}

TEST_CASE("golden validate_profile", "[golden]") {
    golden::for_each("services.cleanup_profiles", "validate_profile", [](const golden::Case& c) {
        Json got{{"ok", true}};
        try {
            text::validate_profile(profile_from(c.args().at(1)), c.args().at(0), windows_context());
        } catch (const fv::py::ValueError& error) {
            got = Json{{"error", error.what()}};
        }
        CHECK(golden::equal(c.result<Json>(), got));
    });
}

TEST_CASE("golden save_cleanup_profile", "[golden]") {
    golden::for_each("services.cleanup_profiles", "save_cleanup_profile",
                     [](const golden::Case& c) {
                         Json settings = c.args().at(0);
                         Json got;
                         try {
                             text::save_cleanup_profile(settings, profile_from(c.args().at(1)),
                                                        windows_context());
                             got = Json{{"settings", settings}};
                         } catch (const fv::py::ValueError& error) {
                             got = Json{{"error", error.what()}};
                             CHECK(settings == c.args().at(0));
                         }
                         CHECK(golden::equal(c.result<Json>(), got));
                     });
}

TEST_CASE("golden delete_cleanup_profile", "[golden]") {
    golden::for_each("services.cleanup_profiles", "delete_cleanup_profile",
                     [](const golden::Case& c) {
                         Json settings = c.args().at(0);
                         text::delete_cleanup_profile(settings, c.arg<std::string>(1),
                                                      hotkeys::Vocabulary::Windows);
                         CHECK(golden::equal(c.result<Json>(), Json{{"settings", settings}}));
                     });
}

TEST_CASE("golden compose_profile_prompt", "[golden]") {
    golden::for_each("services.cleanup_profiles", "compose_profile_prompt",
                     [](const golden::Case& c) {
                         CHECK(text::compose_profile_prompt(profile_from(c.args().at(0)),
                                                            c.arg<std::vector<std::string>>(1)) ==
                               c.result<std::string>());
                     });
}
