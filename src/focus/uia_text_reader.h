#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include "focus/focus_capture.h"

namespace fv::focus {

// UI Automation's own bounds: how long to reach an app's provider and how long one call may
// take. The capture deadline is the outer bound.
inline constexpr std::chrono::milliseconds UIA_CONNECTION_TIMEOUT{500};
inline constexpr std::chrono::milliseconds UIA_TRANSACTION_TIMEOUT{1000};

// Text around the caret in other apps, through Windows UI Automation (IUIAutomation2 when the
// system has it, for its timeouts). The reader joins the multithreaded apartment for its thread
// (tolerating one already joined in another mode), and no interface pointer ever leaves it.
// The selection anchors the text around it (its first range); only without a selection range,
// and where TextPattern2 exists, the caret range does. No TextPattern means unknown, never
// empty: Chromium builds its tree lazily.
// Affinity: owned thread (the one that made it); create, use and destroy it there.
class UiaTextReader final : public TextReader {
public:
    // Throws when UI Automation is unavailable (std::runtime_error off Windows).
    UiaTextReader();
    ~UiaTextReader() override;
    UiaTextReader(const UiaTextReader&) = delete;
    UiaTextReader& operator=(const UiaTextReader&) = delete;

    // The focused element's text, or nullopt for unknown, our own process, or focus that moved
    // to another process than identity's since it was taken.
    std::optional<TextContext> read(const AppIdentity& identity, bool include_text,
                                    bool include_selection) override;
    // One window's element instead of the focused one (tests: a hidden window of our own).
    std::optional<TextContext> read_window(std::uintptr_t window, bool include_text,
                                           bool include_selection);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace fv::focus
