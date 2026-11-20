#pragma once

#include <optional>
#include <string_view>

namespace fv::appcore {

// Logical overlay states, independent of the status text shown with them. Runtimes publish them
// and the overlay router, the Quick Record tab and the upload progress map them to visuals.
enum class OverlayState {
    None,
    Recording,
    Processing,
    Transcribing,
    Cleaning,
    Canceling,
    SttEnabled,
    SttDisabled,
    CommandListening,
    Rewriting,
};

// The value strings logs and wire payloads carry: "none", "recording", ..., "rewriting".
std::string_view to_string(OverlayState state);
// nullopt for a string that names no state (exact, case-sensitive, as Python's Enum lookup).
std::optional<OverlayState> overlay_state_from(std::string_view value);

} // namespace fv::appcore
