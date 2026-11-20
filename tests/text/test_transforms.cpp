// tests/test_personalize_s3_transforms_storage.py: tolerant loading, validation, shortcuts and the
// starters of saved transforms; plus the services.text_transforms golden vectors.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <functional>
#include <string>
#include <vector>

#include "base/json.h"
#include "golden/golden.h"
#include "hotkeys/conflicts.h"
#include "prompt_test_support.h"
#include "pyport/errors.h"
#include "settings/store.h"
#include "text/cleanup_profiles.h"
#include "text/transforms.h"

using Catch::Matchers::ContainsSubstring;
using fv::Json;
using fv::prompt_test::TempDir;
using fv::prompt_test::transform_from;
using fv::prompt_test::windows_context;
using fv::text::Transform;
namespace text = fv::text;
namespace golden = fv::golden;
namespace hotkeys = fv::hotkeys;

namespace {

const std::string kKey = "text_transforms";

Json row(const std::string& id, const std::string& name, const std::string& instruction = "Do it.",
         const Json& hotkey = "") {
    return Json{{"id", id}, {"name", name}, {"instruction", instruction}, {"hotkey", hotkey}};
}

std::vector<std::string> ids(const std::vector<Transform>& transforms) {
    std::vector<std::string> out;
    for (const Transform& transform : transforms) {
        out.push_back(transform.id);
    }
    return out;
}

std::string error_of(const std::function<void()>& run) {
    try {
        run();
    } catch (const fv::py::ValueError& error) {
        return error.what();
    }
    return "";
}

Json plain(const std::vector<Transform>& transforms) {
    Json out = Json::array();
    for (const Transform& transform : transforms) {
        out.push_back(text::transform_to_json(transform));
    }
    return out;
}

} // namespace

TEST_CASE("test_starters_show_until_a_library_is_saved",
          "[test_personalize_s3_transforms_storage]") {
    // from
    // tests/test_personalize_s3_transforms_storage.py::test_starters_show_until_a_library_is_saved
    CHECK(text::load_transforms(Json::object()) == text::starter_transforms());
    std::vector<std::string> names;
    for (const Transform& transform : text::starter_transforms()) {
        names.push_back(transform.name);
    }
    CHECK(names ==
          std::vector<std::string>{"Polish", "Make concise", "Fix grammar", "Prompt engineer"});
    CHECK(text::load_transforms(Json{{kKey, Json::array()}}).empty());

    Json settings = Json::object();
    text::delete_transform(settings, "polish");
    CHECK(ids(text::load_transforms(settings)) ==
          std::vector<std::string>{"make-concise", "fix-grammar", "prompt-engineer"});
}

TEST_CASE("test_loading_skips_malformed_rows_and_normalises_shortcuts",
          "[test_personalize_s3_transforms_storage]") {
    // from
    // tests/test_personalize_s3_transforms_storage.py::test_loading_skips_malformed_rows_and_normalises_shortcuts
    Json rows = Json::array();
    rows.push_back("nonsense");
    rows.push_back(Json{{"id", "x"}, {"name", ""}, {"instruction", "Do it."}});
    rows.push_back(Json{{"id", "y"}, {"name", "No instruction"}, {"instruction", "  "}});
    rows.push_back(Json{{"id", 3}, {"name", "Bad id"}, {"instruction", "Do it."}});
    rows.push_back(row(" tidy ", " Tidy ", " Tidy it up. ", "Alt+Ctrl+T"));
    rows.push_back(row("tidy", "Duplicate id"));
    rows.push_back(row("loose", "Loose", "Do it.", 42));
    const Json settings{{kKey, rows}};
    CHECK(text::load_transforms(settings) ==
          std::vector<Transform>{{"tidy", "Tidy", "Tidy it up.", "ctrl+alt+t"},
                                 {"loose", "Loose", "Do it.", ""}});
    CHECK(text::load_transforms(Json{{kKey, "oops"}}).empty());
    CHECK(text::find_transform(settings, "tidy")->name == "Tidy");
    CHECK_FALSE(text::find_transform(settings, "missing").has_value());
}

TEST_CASE("test_loading_stops_at_the_cap", "[test_personalize_s3_transforms_storage]") {
    // from tests/test_personalize_s3_transforms_storage.py::test_loading_stops_at_the_cap
    // Python lowers config.MAX_TEXT_TRANSFORMS to 3 and saves 5; the cap is a constant here, so
    // five more than it are saved.
    Json rows = Json::array();
    for (std::size_t i = 0; i < text::MAX_TRANSFORMS + 5; ++i) {
        rows.push_back(row(std::to_string(i), "T" + std::to_string(i)));
    }
    const auto loaded = text::load_transforms(Json{{kKey, rows}});
    REQUIRE(loaded.size() == text::MAX_TRANSFORMS);
    CHECK(loaded.front().id == "0");
    CHECK(loaded.back().id == std::to_string(text::MAX_TRANSFORMS - 1));
}

TEST_CASE("test_save_adds_replaces_and_keeps_order", "[test_personalize_s3_transforms_storage]") {
    // from tests/test_personalize_s3_transforms_storage.py::test_save_adds_replaces_and_keeps_order
    Json settings = Json::object();
    text::save_transform(settings, {"mine", " Mine ", " Shorter. ", "ctrl+alt+j"});
    CHECK(ids(text::load_transforms(settings)).back() == "mine");
    CHECK(settings.at(kKey).back() == row("mine", "Mine", "Shorter.", "ctrl+alt+j"));

    text::save_transform(settings, {"polish", "Polish more", "Polish harder.", ""});
    std::vector<std::string> names;
    for (const Transform& transform : text::load_transforms(settings)) {
        names.push_back(transform.name);
    }
    CHECK(names == std::vector<std::string>{"Polish more", "Make concise", "Fix grammar",
                                            "Prompt engineer", "Mine"});
}

TEST_CASE("test_invalid_transforms_are_refused_without_changes",
          "[test_personalize_s3_transforms_storage]") {
    // from
    // tests/test_personalize_s3_transforms_storage.py::test_invalid_transforms_are_refused_without_changes
    const std::vector<std::pair<Transform, std::string>> rows{
        {{"a", "  ", "Do it.", ""}, "Give the transform a name."},
        {{"a", std::string(81, 'x'), "Do it.", ""}, "80 characters or fewer"},
        {{"a", "New", "   ", ""}, "Add an instruction"},
        {{"a", "polish", "Do it.", ""}, "already exists"},
        {{"", "New", "Do it.", ""}, "no id"},
    };
    for (const auto& [transform, message] : rows) {
        INFO(message);
        Json settings = Json::object();
        CHECK_THAT(error_of([&] { text::save_transform(settings, transform); }),
                   ContainsSubstring(message));
        CHECK(settings == Json::object());
    }
}

TEST_CASE("test_the_cap_refuses_only_new_transforms", "[test_personalize_s3_transforms_storage]") {
    // from
    // tests/test_personalize_s3_transforms_storage.py::test_the_cap_refuses_only_new_transforms
    // Python lowers the cap to the four starters; here the library is filled to the cap.
    Json rows = Json::array();
    rows.push_back(row("polish", "Polish"));
    for (std::size_t i = 1; i < text::MAX_TRANSFORMS; ++i) {
        rows.push_back(row(std::to_string(i), "T" + std::to_string(i)));
    }
    Json settings{{kKey, rows}};
    CHECK_THAT(error_of([&] { text::save_transform(settings, {"fifth", "Fifth", "Do it.", ""}); }),
               ContainsSubstring("up to 50 transforms"));
    text::save_transform(settings, {"polish", "Polish", "Edited.", ""});
    CHECK(text::find_transform(settings, "polish")->instruction == "Edited.");
}

TEST_CASE("test_shortcuts_conflict_with_actions_profiles_and_other_transforms",
          "[test_personalize_s3_transforms_storage]") {
    // from
    // tests/test_personalize_s3_transforms_storage.py::test_shortcuts_conflict_with_actions_profiles_and_other_transforms
    Json settings{
        {"hotkeys", Json{{"record_toggle", "ctrl+alt+r"}, {"command_mode", "ctrl+alt+k"}}}};
    text::save_cleanup_profile(
        settings, fv::prompt_test::profile("ticket", "Ticket", "Format.", "ctrl+alt+t"));
    text::save_transform(settings, {"tidy", "Tidy", "Tidy.", "ctrl+alt+y"});

    for (const auto& [hotkey, owner] : std::vector<std::pair<std::string, std::string>>{
             {"Alt+Ctrl+R", "Recording"},
             {"ctrl+alt+k", "Command Mode"},
             {"ctrl+alt+t", "Ticket"},
             {"alt+ctrl+y", "Tidy"},
         }) {
        INFO(hotkey);
        CHECK_THAT(
            error_of([&] { text::save_transform(settings, {"new", "New", "Do it.", hotkey}); }),
            ContainsSubstring("already used by " + owner));
    }

    // Its own shortcut never conflicts with itself.
    text::save_transform(settings, {"tidy", "Tidy", "Tidier.", "ctrl+alt+y"});
    hotkeys::ConflictOptions shared;
    shared.bindings = text::hotkey_bindings(settings);
    CHECK(hotkeys::hotkey_conflict("ctrl+alt+y", settings, shared) == "Tidy");
    CHECK(text::transform_hotkey_conflict("ctrl+alt+y", settings, "tidy").empty());
}

TEST_CASE("test_delete_removes_only_that_transform", "[test_personalize_s3_transforms_storage]") {
    // from tests/test_personalize_s3_transforms_storage.py::test_delete_removes_only_that_transform
    Json settings{{kKey, Json::array({row("a", "A"), row("b", "B")})}};
    text::delete_transform(settings, "a");
    text::delete_transform(settings, "missing");
    CHECK(settings.at(kKey) == Json::array({row("b", "B")}));
}

TEST_CASE("test_mutators_work_through_the_settings_manager",
          "[test_personalize_s3_transforms_storage]") {
    // from
    // tests/test_personalize_s3_transforms_storage.py::test_mutators_work_through_the_settings_manager
    TempDir temp;
    fv::SettingsStore store(temp.path() / "settings.json");
    store.mutate([](Json& s) { text::save_transform(s, {"mine", "Mine", "Do it.", ""}); });
    CHECK_THROWS_AS(
        store.mutate([](Json& s) { text::save_transform(s, {"other", "mine", "Clash.", ""}); }),
        fv::py::ValueError);
    std::vector<std::string> expected = ids(text::starter_transforms());
    expected.push_back("mine");
    CHECK(ids(text::load_transforms(*store.snapshot())) == expected);
}

// --- golden vectors -------------------------------------------------------------------------

TEST_CASE("golden STARTER_TRANSFORMS", "[golden]") {
    golden::for_each("services.text_transforms", "STARTER_TRANSFORMS", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), plain(text::starter_transforms())));
    });
}

TEST_CASE("golden load_transforms", "[golden]") {
    golden::for_each("services.text_transforms", "load_transforms", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), plain(text::load_transforms(
                                                  c.args().at(0), hotkeys::Vocabulary::Windows))));
    });
}

TEST_CASE("golden find_transform", "[golden]") {
    golden::for_each("services.text_transforms", "find_transform", [](const golden::Case& c) {
        const auto found = text::find_transform(c.args().at(0), c.arg<std::string>(1),
                                                hotkeys::Vocabulary::Windows);
        CHECK(golden::equal(c.result<Json>(), found ? text::transform_to_json(*found) : Json()));
    });
}

TEST_CASE("golden transform_hotkey_conflict", "[golden]") {
    golden::for_each(
        "services.text_transforms", "transform_hotkey_conflict", [](const golden::Case& c) {
            CHECK(text::transform_hotkey_conflict(c.arg<std::string>(0), c.args().at(1),
                                                  c.arg<std::string>(2),
                                                  windows_context()) == c.result<std::string>());
        });
}

TEST_CASE("golden validate_transform", "[golden]") {
    golden::for_each("services.text_transforms", "validate_transform", [](const golden::Case& c) {
        Json got{{"ok", true}};
        try {
            text::validate_transform(transform_from(c.args().at(1)), c.args().at(0),
                                     windows_context());
        } catch (const fv::py::ValueError& error) {
            got = Json{{"error", error.what()}};
        }
        CHECK(golden::equal(c.result<Json>(), got));
    });
}

TEST_CASE("golden save_transform", "[golden]") {
    golden::for_each("services.text_transforms", "save_transform", [](const golden::Case& c) {
        Json settings = c.args().at(0);
        Json got;
        try {
            text::save_transform(settings, transform_from(c.args().at(1)), windows_context());
            got = Json{{"settings", settings}};
        } catch (const fv::py::ValueError& error) {
            got = Json{{"error", error.what()}};
            CHECK(settings == c.args().at(0));
        }
        CHECK(golden::equal(c.result<Json>(), got));
    });
}

TEST_CASE("golden delete_transform", "[golden]") {
    golden::for_each("services.text_transforms", "delete_transform", [](const golden::Case& c) {
        Json settings = c.args().at(0);
        text::delete_transform(settings, c.arg<std::string>(1), hotkeys::Vocabulary::Windows);
        CHECK(golden::equal(c.result<Json>(), Json{{"settings", settings}}));
    });
}
