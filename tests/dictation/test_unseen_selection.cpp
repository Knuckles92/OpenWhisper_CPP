// Command Mode and transforms where the clipboard can't see the copy of the selection
// (tests/test_personalize_fix2_g4_wayland_selection.py's runtime half): they say so instead of
// writing over the selection, and nothing is copied or written. On Windows the clipboard always
// sees other apps' copies (clipboard::foreign_copies_visible); the refusal is the same path as
// any selection the clipboard hands back unread.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include <algorithm>
#include <string>

#include "command_support.h"
#include "dictation/command_selection.h"

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::focus::FocusSnapshot;

namespace {

// h: Notepad has focus, its selection is unknown, and the clipboard hands back no copy.
struct HiddenRig {
    HiddenRig() {
        rig.capture.fake(FocusSnapshot{notepad_app(), std::nullopt});
        rig.clipboard.set_selection(std::nullopt);
    }
    bool wrote() const {
        const auto entries = rig.trace.entries();
        return std::ranges::any_of(entries, [](const std::string& entry) {
            return entry == "send_copy" || entry.starts_with("copy:") ||
                   entry.starts_with("stage:");
        });
    }
    CommandRig rig;
};

} // namespace

// from
// tests/test_personalize_fix2_g4_wayland_selection.py::test_command_mode_says_so_instead_of_writing_over_the_selection
TEST_CASE("command mode says so instead of writing over an unseen selection",
          "[test_personalize_fix2_g4_wayland_selection]") {
    HiddenRig hidden;
    auto& rig = hidden.rig;

    rig.commands->key_pressed(1.0);
    const auto job = rig.recording_job();
    REQUIRE(job);
    rig.commands->key_released(2.0);
    REQUIRE(rig.settle([&] { return resolved(job->selection, {}).has_value(); }));

    CHECK_THROWS_MATCHES(rig.commands->complete_recording("make this more formal", job),
                         CommandRefused, Catch::Matchers::Message(clipboard_hidden_message()));
    CHECK(rig.model->requests().empty());
}

// from tests/test_personalize_fix2_g4_wayland_selection.py::test_a_transform_says_so
// and test_a_transform_end_to_end_keeps_the_users_clipboard (the clipboard's own half, no copy
// sent and nothing written: tests/clipboard/test_selection.cpp)
TEST_CASE("a transform of an unseen selection says so and writes nothing",
          "[test_personalize_fix2_g4_wayland_selection]") {
    HiddenRig hidden;
    auto& rig = hidden.rig;

    rig.commands->run_transform("polish");

    REQUIRE(rig.settle([&] { return rig.last_status() == clipboard_hidden_message(); }));
    CHECK_FALSE(rig.runtime->has_active_job());
    CHECK(rig.clipboard.captures().size() == 1);
    CHECK_FALSE(hidden.wrote());
    CHECK(rig.model->requests().empty());
}
