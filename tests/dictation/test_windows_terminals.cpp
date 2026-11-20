// Terminals the catalogue knows beyond Windows Terminal and the console
// (tests/test_personalize_fix2_g1_linux_terminals.py, ported against Windows terminals): spoken
// lists stay on one line there, and Command Mode never sends a copy shortcut into them, which
// would interrupt the program running.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>
#include <variant>

#include "base/json.h"
#include "command_support.h"
#include "text/cleanup_config.h"
#include "text/cleanup_prompts.h"

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::focus::FocusSnapshot;

namespace {

// The terminals Windows users run other than Windows Terminal and the console.
fv::focus::AppIdentity windows_terminal_app() {
    const auto [exe, name] = GENERATE(table<std::string, std::string>({
        {"wezterm-gui.exe", "WezTerm"},
        {"alacritty.exe", "Alacritty"},
        {"mintty.exe", "Git Bash"},
        {"pwsh.exe", "PowerShell"},
    }));
    return app(exe, name, 9, "0x9");
}

} // namespace

// from
// tests/test_personalize_fix2_g1_linux_terminals.py::test_spoken_lists_stay_on_one_line_in_a_newly_known_terminal
TEST_CASE("spoken lists stay on one line in a terminal the catalogue knows",
          "[test_personalize_fix2_g1_linux_terminals]") {
    const auto identity = windows_terminal_app();
    CAPTURE(identity.app_id);
    const fv::Json settings{{"transcript_cleanup_enabled", true},
                            {"transcript_cleanup_level", "medium"}};

    CHECK(fv::text::inline_lists_block(settings, FocusSnapshot{identity, std::nullopt}) ==
          fv::text::config::cleanup_sentences().terminal_lines);
}

// from
// tests/test_personalize_fix2_g1_linux_terminals.py::test_command_mode_never_copies_from_a_newly_known_terminal
TEST_CASE("command mode never copies from a terminal the catalogue knows",
          "[test_personalize_fix2_g1_linux_terminals]") {
    const auto identity = windows_terminal_app();
    CAPTURE(identity.app_id);
    CommandRig rig;
    rig.capture.fake(FocusSnapshot{identity, std::nullopt});
    rig.clipboard.set_selection("would have interrupted the build");

    rig.commands->key_pressed(1.0);
    const auto job = rig.recording_job();
    REQUIRE(job);
    rig.commands->key_released(2.0);
    REQUIRE(rig.settle([&] { return resolved(job->selection, {}).has_value(); }));

    const auto value = resolved(job->selection, {});
    REQUIRE(value);
    REQUIRE(std::holds_alternative<std::string>(*value));
    CHECK(std::get<std::string>(*value).empty());
    CHECK(rig.clipboard.captures().empty());
    CHECK(rig.keys.waits().empty());
}
