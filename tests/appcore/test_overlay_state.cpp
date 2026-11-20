// ui_qt/overlay_state.py: OverlayState and its value strings.
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "appcore/overlay_state.h"

using fv::appcore::overlay_state_from;
using fv::appcore::OverlayState;
using fv::appcore::to_string;

TEST_CASE("every overlay state has OpenWhisper's value string and reads back", "[overlay_state]") {
    const std::vector<std::pair<OverlayState, std::string_view>> values{
        {OverlayState::None, "none"},
        {OverlayState::Recording, "recording"},
        {OverlayState::Processing, "processing"},
        {OverlayState::Transcribing, "transcribing"},
        {OverlayState::Cleaning, "cleaning"},
        {OverlayState::Canceling, "canceling"},
        {OverlayState::SttEnabled, "stt_enabled"},
        {OverlayState::SttDisabled, "stt_disabled"},
        {OverlayState::CommandListening, "command_listening"},
        {OverlayState::Rewriting, "rewriting"},
    };
    for (const auto& [state, name] : values) {
        CHECK(to_string(state) == name);
        CHECK(overlay_state_from(name) == state);
    }
}

TEST_CASE("an unknown value string names no overlay state", "[overlay_state]") {
    CHECK(overlay_state_from("") == std::nullopt);
    CHECK(overlay_state_from("Recording") == std::nullopt);
    CHECK(overlay_state_from("RECORDING") == std::nullopt);
    CHECK(overlay_state_from("listening") == std::nullopt);
}
