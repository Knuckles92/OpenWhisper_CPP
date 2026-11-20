// The Actions page's pure helpers against OpenWhisper's own results (tools/parity/cases/
// ui_qt.widgets.action_panel.py and ui_qt.dialogs.settings_actions.py): unique_name,
// saved_note, the editor's onto_saved and the page's rail value.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "automation/action_model.h"
#include "base/json.h"
#include "golden/golden.h"
#include "hotkeys/testing/vocabulary.h"
#include "ui_automation/action_editor.h"
#include "ui_automation/action_panel.h"
#include "ui_automation/actions_page.h"

using fv::Json;
using fv::automation::Action;

namespace {

// Action(**spec), as the case file builds it: the fields as given, the dataclass's defaults for
// the rest.
Action fromDict(const Json& spec) {
    Action action;
    action.id = spec.value("id", std::string());
    action.name = spec.value("name", std::string());
    action.kind = spec.value("kind", std::string());
    action.params = spec.value("params", Json::object());
    action.hotkey = spec.value("hotkey", std::string());
    action.shaping = spec.value("shaping", std::string("cleanup"));
    action.spoken_names = spec.value("spoken_names", std::vector<std::string>());
    action.description = spec.value("description", std::string());
    action.enabled = spec.value("enabled", true);
    return action;
}

} // namespace

TEST_CASE("golden: action_panel.unique_name", "[golden]") {
    fv::golden::for_each(
        "ui_qt.widgets.action_panel", "unique_name", [](const fv::golden::Case& c) {
            CHECK(fv::action_panel::unique_name(c.arg<std::string>(0), c.arg<Json>(1)) ==
                  c.result<std::string>());
        });
}

TEST_CASE("golden: action_panel.saved_note", "[golden]") {
    // The vectors were recorded with the Windows hook's key names ("*", not "Num *").
    const fv::hotkeys::testing::ScopedVocabulary windows(fv::hotkeys::Vocabulary::Windows);
    fv::golden::for_each("ui_qt.widgets.action_panel", "saved_note", [](const fv::golden::Case& c) {
        CHECK(fv::action_panel::saved_note(fromDict(c.arg<Json>(0))) == c.result<std::string>());
    });
}

TEST_CASE("golden: action_editor.onto_saved", "[golden]") {
    fv::golden::for_each("ui_qt.widgets.action_panel", "onto_saved", [](const fv::golden::Case& c) {
        const Action result = fv::ActionEditor::ontoSaved(
            fromDict(c.arg<Json>(0)), fromDict(c.arg<Json>(1)), fromDict(c.arg<Json>(2)));
        CHECK(fv::golden::equal(c.result<Json>(), result.to_dict()));
    });
}

TEST_CASE("golden: settings_actions.rail_value", "[golden]") {
    fv::golden::for_each(
        "ui_qt.dialogs.settings_actions", "rail_value", [](const fv::golden::Case& c) {
            CHECK(fv::actions_page::rail_value(c.arg<Json>(0)) == c.result<std::string>());
        });
}
