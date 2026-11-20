#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>

#include "hotkeys/conflicts.h"
#include "hotkeys/defaults.h"

using namespace fv::hotkeys;
using fv::Json;

namespace {

std::string default_of(std::string_view action) {
    for (const auto& [name, hotkey] : default_hotkeys()) {
        if (name == action) {
            return hotkey;
        }
    }
    return "";
}

// The test's settings fixture: two profiles saved in the settings, two transforms from
// text_transforms.load_transforms, as their loaders hand them over.
struct Fixture {
    Json settings = Json::parse(R"({"transcript_cleanup_profiles": [
        {"id": "notes", "name": "Meeting notes", "instructions": "Bullets.",
         "hotkey": "ctrl+shift+f9"},
        {"id": "ticket", "name": "Ticket", "instructions": "A ticket.", "hotkey": ""}]})");
    Bindings bindings{
        .profiles = {{"notes", "Meeting notes", "ctrl+shift+f9"}, {"ticket", "Ticket", ""}},
        .transforms = {{"polish", "Polish", "ctrl+alt+p"}, {"short", "Make concise", ""}},
    };

    ConflictOptions options() const { return {.bindings = bindings}; }
};

} // namespace

// from tests/test_personalize_foundation_hotkeys.py::test_new_actions_have_no_default_shortcut
TEST_CASE("test_new_actions_have_no_default_shortcut", "[test_personalize_foundation_hotkeys]") {
    for (const char* action :
         {"command_mode", "scratchpad_toggle", "cycle_language", "paste_last_original"}) {
        CHECK(default_of(action).empty());
    }
}

// from
// tests/test_personalize_foundation_hotkeys.py::test_free_and_empty_shortcuts_conflict_with_nothing
TEST_CASE("test_free_and_empty_shortcuts_conflict_with_nothing",
          "[test_personalize_foundation_hotkeys]") {
    const Fixture f;
    CHECK(hotkey_conflict("", f.settings, f.options()).empty());
    CHECK(hotkey_conflict("ctrl+shift+f11", f.settings, f.options()).empty());
}

// An unset standard shortcut ("" saved or by default) is skipped, as Python skips falsy values:
// a whitespace-only shortcut, which parses to no key, collides with none of them.
TEST_CASE("an unset standard shortcut conflicts with nothing", "[conflicts]") {
    Fixture f;
    CHECK(hotkey_conflict("   ", f.settings, f.options()).empty());
    f.settings["hotkeys"] = Json{{"record_toggle", ""}, {"command_mode", ""}};
    CHECK(hotkey_conflict("   ", f.settings, f.options()).empty());
    ConflictOptions unsaved = f.options();
    unsaved.standard_hotkeys = Json{{"scratchpad_toggle", ""}};
    CHECK(hotkey_conflict("   ", f.settings, unsaved).empty());
}

// from
// tests/test_personalize_foundation_hotkeys.py::test_standard_actions_conflict_by_their_saved_or_default_shortcut
TEST_CASE("test_standard_actions_conflict_by_their_saved_or_default_shortcut",
          "[test_personalize_foundation_hotkeys]") {
    Fixture f;
    const std::string record = default_of("record_toggle");
    CHECK(hotkey_conflict(record, f.settings, f.options()) == "Recording");

    f.settings["hotkeys"] = Json{{"command_mode", "ctrl+alt+c"}};
    // Modifier order does not matter.
    CHECK(hotkey_conflict("alt+ctrl+c", f.settings, f.options()) == "Command Mode");
    // Unsaved edits replace the saved shortcuts.
    ConflictOptions unsaved = f.options();
    unsaved.standard_hotkeys = Json::object();
    CHECK(hotkey_conflict("ctrl+alt+c", f.settings, unsaved).empty());
    unsaved.standard_hotkeys = Json{{"scratchpad_toggle", "ctrl+alt+s"}};
    CHECK(hotkey_conflict("ctrl+alt+s", f.settings, unsaved) == "Scratchpad");
}

// from tests/test_personalize_foundation_hotkeys.py::test_profiles_and_transforms_conflict_by_name
TEST_CASE("test_profiles_and_transforms_conflict_by_name",
          "[test_personalize_foundation_hotkeys]") {
    const Fixture f;
    CHECK(hotkey_conflict("ctrl+shift+f9", f.settings, f.options()) == "Meeting notes");
    CHECK(hotkey_conflict("ctrl+alt+p", f.settings, f.options()) == "Polish");
}

// from
// tests/test_personalize_foundation_hotkeys.py::test_the_binding_being_edited_does_not_conflict_with_itself
TEST_CASE("test_the_binding_being_edited_does_not_conflict_with_itself",
          "[test_personalize_foundation_hotkeys]") {
    const auto [hotkey, kind, id] = GENERATE(std::tuple{"ctrl+shift+f9", kProfile, "notes"},
                                             std::tuple{"ctrl+alt+p", kTransform, "polish"});
    const Fixture f;
    ConflictOptions options = f.options();
    options.exclude = {std::string(kind), id};
    CHECK(hotkey_conflict(hotkey, f.settings, options).empty());
}

// from
// tests/test_personalize_foundation_hotkeys.py::test_an_excluded_standard_action_is_free_to_keep_its_shortcut
TEST_CASE("test_an_excluded_standard_action_is_free_to_keep_its_shortcut",
          "[test_personalize_foundation_hotkeys]") {
    const Fixture f;
    const std::string record = default_of("record_toggle");
    ConflictOptions options = f.options();
    options.exclude = {std::string(kStandard), "record_toggle"};
    CHECK(hotkey_conflict(record, f.settings, options).empty());
    // Excluding one kind never hides another kind with the same id.
    options.exclude = {std::string(kProfile), "record_toggle"};
    CHECK(hotkey_conflict(record, f.settings, options) == "Recording");
}
