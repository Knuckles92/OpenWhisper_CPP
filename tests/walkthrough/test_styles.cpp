// from tests/test_walkthrough_styles.py (the library's tests; the dialogs and menus are the
// storyboard's and the tab's)
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <format>
#include <functional>

#include "base/cancel.h"
#include "library_support.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "settings/keys.h"
#include "walkthrough/library.h"
#include "walkthrough/styles.h"
#include "walkthrough/write_up.h"

using namespace fv::walkthrough;
using namespace fv::walkthrough::test;
using fv::Json;

namespace {

const std::string kStyles(fv::keys::walkthrough_write_up_styles.key());

std::vector<WriteUpStyle> default_and_starters() {
    std::vector<WriteUpStyle> styles{default_style()};
    styles.insert(styles.end(), starter_styles().begin(), starter_styles().end());
    return styles;
}

std::string value_error(const std::function<void()>& call) {
    try {
        call();
    } catch (const fv::py::ValueError& error) {
        return error.what();
    }
    return "<no ValueError>";
}

} // namespace

TEST_CASE("the default comes first then the starters until any is saved",
          "[test_walkthrough_styles]") {
    const auto styles = load_styles(Json::object());
    CHECK(styles == default_and_starters());
    CHECK(styles[0].name == "Walkthrough");
    CHECK(styles[1].name == "Step-by-step guide");
    CHECK(styles[2].name == "Bug report");
}

TEST_CASE("deleting every style leaves only the default", "[test_walkthrough_styles]") {
    Json settings = Json::object();
    for (const auto& style : starter_styles()) {
        delete_style(settings, style.id);
    }
    CHECK(settings[kStyles] == Json::array());
    CHECK(load_styles(settings) == std::vector<WriteUpStyle>{default_style()});
}

TEST_CASE("a saved style is added and one by the same name replaced", "[test_walkthrough_styles]") {
    Json settings = Json::object();
    const auto added = save_style(settings, "  Release notes ", "  Bullets of what changed. ");
    CHECK(load_styles(settings).back() == added);
    CHECK(added.name == "Release notes");
    CHECK(added.instructions == "Bullets of what changed.");
    CHECK(added.id.size() == 12);

    const auto again = save_style(settings, "release NOTES", "Shorter bullets.");
    const auto styles = load_styles(settings);
    CHECK(again.id == added.id);
    CHECK(again.name == "release NOTES");
    std::vector<std::string> instructions;
    for (const auto& style : styles) {
        if (style.id == added.id) {
            instructions.push_back(style.instructions);
        }
    }
    CHECK(instructions == std::vector<std::string>{"Shorter bullets."});
    CHECK(styles.size() == 1 + starter_styles().size() + 1);
}

TEST_CASE("a starter can be rewritten by its name", "[test_walkthrough_styles]") {
    Json settings = Json::object();
    save_style(settings, "Bug report", "Just the steps.");
    const auto bug = style_named(settings, "Bug report");
    REQUIRE(bug);
    CHECK(bug->id == "bug-report");
    CHECK(bug->instructions == "Just the steps.");
}

TEST_CASE("a style that cant be saved says why", "[test_walkthrough_styles]") {
    using Row = std::tuple<std::string, std::string, std::string>;
    const auto [name, words, says] =
        GENERATE(Row{"", "Words.", "Give the style a name."},
                 Row{std::string(61, 'x'), "Words.", "Use a name with 60 characters or fewer."},
                 Row{"Checklist", "  ", "Add the instructions for the write-up you want."},
                 Row{"walkthrough", "Words.",
                     "\"Walkthrough\" is the built-in style. Choose another name."});
    Json settings = Json::object();
    CHECK(value_error([&] { save_style(settings, name, words); }) == says);
    CHECK_FALSE(settings.contains(kStyles));
}

TEST_CASE("there is room for only so many styles", "[test_walkthrough_styles]") {
    Json settings = Json::object();
    settings[kStyles] = Json::array();
    for (std::size_t number = 0; number < MAX_STYLES; ++number) {
        save_style(settings, std::format("Style {}", number), "Words.");
    }
    CHECK(value_error([&] { save_style(settings, "One more", "Words."); }) ==
          "You can keep up to 30 styles. Delete one first.");
    CHECK_NOTHROW(save_style(settings, "Style 3", "Replacing one still works."));
}

TEST_CASE("broken saved styles are skipped", "[test_walkthrough_styles]") {
    Json settings = Json::object();
    settings[kStyles] = fv::py::loads(R"([
        {"id": "a", "name": "Fine", "instructions": "Words."},
        {"id": "a", "name": "Same id", "instructions": "Words."},
        {"id": "walkthrough", "name": "Posing as the default", "instructions": "Words."},
        {"id": "b", "name": "", "instructions": "Words."},
        "not a style"])");
    std::vector<std::string> names;
    for (const auto& style : load_styles(settings)) {
        names.push_back(style.name);
    }
    CHECK(names == std::vector<std::string>{"Walkthrough", "Fine"});
    Json nonsense = Json::object();
    nonsense[kStyles] = "nonsense";
    CHECK(load_styles(nonsense) == std::vector<WriteUpStyle>{default_style()});
}

TEST_CASE("the default words and an empty prompt are the default style",
          "[test_walkthrough_styles]") {
    const auto styles = load_styles(Json::object());
    CHECK(style_for("", styles) == default_style());
    CHECK(style_for(starter_styles()[1].instructions + "\n", styles) == starter_styles()[1]);
    CHECK_FALSE(style_for("Something else.", styles));
}

TEST_CASE("a redo with instructions uses them instead of settings", "[test_walkthrough_styles]") {
    Capture capture;
    Json settings = Json::object();
    settings[std::string(fv::keys::walkthrough_write_up_instructions.key())] = "From Settings.";
    std::vector<std::string> called;
    const write_up::AgentTaskRunner run = [&](const write_up::AgentTaskRequest& request) {
        // The instructions open the message, up to its first line break.
        called.push_back(request.user_prompt.substr(0, request.user_prompt.find('\n')));
        fv::agents::PassOutcome outcome;
        outcome.ok = true;
        outcome.text = "# Done";
        return outcome;
    };

    retry_write_up(capture.folder, "  Just a checklist. ", settings, fv::CancelToken{}, run);
    retry_write_up(capture.folder, std::nullopt, settings, fv::CancelToken{}, run);

    CHECK(called == std::vector<std::string>{"Just a checklist.", "From Settings."});
}
