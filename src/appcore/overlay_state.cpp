#include "appcore/overlay_state.h"

#include <array>
#include <utility>

namespace fv::appcore {

namespace {

constexpr std::array<std::pair<OverlayState, std::string_view>, 10> kValues{{
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
}};

} // namespace

std::string_view to_string(OverlayState state) {
    for (const auto& [value, name] : kValues) {
        if (value == state) {
            return name;
        }
    }
    return "none";
}

std::optional<OverlayState> overlay_state_from(std::string_view value) {
    for (const auto& [state, name] : kValues) {
        if (name == value) {
            return state;
        }
    }
    return std::nullopt;
}

} // namespace fv::appcore
