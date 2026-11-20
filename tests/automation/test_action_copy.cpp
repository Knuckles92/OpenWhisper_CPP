#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>
#include <vector>

#include "automation/action_copy.h"
#include "golden/golden.h"

// from tests/test_automation_ui_actions_copy.py (also replayed as vectors in test_golden.cpp)

using namespace fv::automation;
using namespace fv::automation::action_copy;
using fv::Json;

namespace {

Action action_of(std::string_view kind, Json params = Json::object()) {
    Action action;
    action.id = "x";
    action.name = "X";
    action.kind = std::string(kind);
    action.params = std::move(params);
    return action;
}

#ifdef _WIN32
const std::string kJournal = "tools\\journal.exe";
#else
const std::string kJournal = "tools/journal.exe";
#endif

} // namespace

TEST_CASE("test_summaries_say_what_happens_to_your_words", "[test_automation_ui_actions_copy]") {
    const auto [kind, params, expected] = GENERATE(table<std::string, Json, std::string>({
        {"open_link", Json{{"template", "claude://claude.ai/new?q={text}"}},
         "Opens a new Claude chat with your words"},
        {"open_link", Json{{"template", "codex://new?prompt={text}"}},
         "Opens a new ChatGPT chat with your words"},
        {"open_link", Json{{"template", "https://chatgpt.com/?q={text}"}},
         "Opens ChatGPT in your browser with your words"},
        {"open_link", Json{{"template", "https://www.perplexity.ai/search?q={text}"}},
         "Opens perplexity.ai with your words"},
        {"open_link", Json{{"template", "obsidian://new?content={text}"}},
         "Opens Obsidian with your words"},
        {"agent", Json{{"agent", "codex"}, {"mode", "handoff"}},
         "Opens a Codex session with your words"},
        {"agent", Json{{"agent", "codex"}, {"mode", "handoff"}, {"new_folder", true}},
         "Opens a Codex session in a new folder with your words"},
        {"agent", Json{{"agent", "claude_code"}, {"mode", "ask"}, {"output", "scratchpad"}},
         "Asks Claude Code and shows the answer in the Scratchpad"},
        {"agent", Json{{"agent", "opencode_cli"}, {"mode", "ask"}, {"output", "paste"}},
         "Asks OpenCode and pastes the answer where you were"},
        {"channel", Json::object(), "Sends your words to your Claude Code session"},
        {"webhook", Json{{"url", "http://homeassistant.local/api/webhook/x"}, {"output", "notice"}},
         "Sends your words to homeassistant.local, then shows the reply in a notice"},
        {"webhook", Json{{"url", "https://n8n.example.com/hook"}, {"output", "none"}},
         "Sends your words to n8n.example.com"},
        // A path as this system writes it: summaries name the program by its file name.
        {"program", Json{{"program", kJournal}, {"output", "clipboard"}},
         "Runs journal.exe with your words, then copies its output"},
        {"clipboard", Json::object(), "Copies your words without pasting them"},
        {"scripts", Json::object(), "Tells the programs listening to OpenWhisper"},
    }));
    CAPTURE(kind, params);
    CHECK(fv::golden::equal_rebranded(expected, summary(action_of(kind, params))));
}

TEST_CASE("test_every_kind_and_preset_has_an_icon_and_a_line",
          "[test_automation_ui_actions_copy]") {
    for (const auto kind : ActionKind::ALL) {
        CAPTURE(kind);
        CHECK(label_for(KIND_ICONS(), kind).ends_with(".svg"));
        CHECK_FALSE(label_for(KIND_BLURBS(), kind).empty());
        CHECK_FALSE(summary(action_of(kind)).empty());
    }
    for (const Preset& preset : PRESETS()) {
        CAPTURE(preset.id);
        Action action = action_of(preset.kind, preset.params);
        action.name = preset.name;
        CHECK_FALSE(summary(action).empty());
    }
}

TEST_CASE("test_spoken_names_and_arguments_are_tidied", "[test_automation_ui_actions_copy]") {
    CHECK(parse_spoken_names(" Claude ,  ask   Claude,, ") ==
          std::vector<std::string>{"Claude", "ask Claude"});
    CHECK(parse_arguments("--append\n\n  --title My day \n") ==
          std::vector<std::string>{"--append", "--title My day"});
    Action house = action_of(ActionKind::CLIPBOARD);
    house.spoken_names = {"House"};
    CHECK(spoken_hint(house) == "Say \xE2\x80\x9CHouse\xE2\x80\x9D");
    CHECK(spoken_hint(action_of(ActionKind::CLIPBOARD)).empty());
}
