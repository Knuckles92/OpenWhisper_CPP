#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "base/signal.h"

namespace fv {

// What the user's Cancel says when it found nothing to cancel.
inline constexpr std::string_view kNothingToCancel = "Nothing to cancel";

// The user's Cancel (shortcut, buttons, tray). Steps are asked in order until one cancels
// something: the recording or transcription in progress, then an AI write-up, then the actions
// still running. With none, nothing_to_cancel carries kNothingToCancel for the status and the
// notice.
// Affinity: UI actor; a Cancel from another thread is posted to the UI executor first.
class CancelRouter {
public:
    CancelRouter() = default;
    CancelRouter(const CancelRouter&) = delete;
    CancelRouter& operator=(const CancelRouter&) = delete;

    // A step returns true when it canceled something. Lower orders are asked first; equal orders
    // in the order they were added.
    void add(int order, std::function<bool()> step);
    // True when a step canceled something.
    bool cancel();
    std::size_t size() const;

    Signal<std::string> nothing_to_cancel;

private:
    struct Step {
        int order;
        std::function<bool()> step;
    };
    std::vector<Step> steps_;
};

} // namespace fv
