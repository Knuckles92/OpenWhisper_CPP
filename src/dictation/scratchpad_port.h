#pragma once

#include <string>

// Where a finished dictation goes instead of the paste when the user dictates into the
// Scratchpad (ui_qt/widgets/scratchpad.py insert). The Scratchpad window (M2.25) implements it.
namespace fv::dictation {

// Affinity: UI thread.
class ScratchpadPort {
public:
    virtual ~ScratchpadPort() = default;
    // The Scratchpad is visible and the active window, so a dictation belongs in it.
    virtual bool is_focused() const = 0;
    // Adds `text` at the Scratchpad's caret as a dictation; true when it took it.
    virtual bool insert(std::string text) = 0;
};

} // namespace fv::dictation
