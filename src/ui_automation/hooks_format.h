#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "automation/programs.h"
#include "automation/webhooks.h"
#include "base/clock.h"

// The words the Hooks page uses for events, deliveries, runs and addresses
// (ui_qt/widgets/hooks_panel.py event_label … unencrypted_note, and the constants beside them).
// Qt-free and pure over its arguments.
namespace fv::hooks_format {

// Recent deliveries or runs shown in an editor.
inline constexpr std::size_t RECENT_ROWS = 5;
// How often a visible section reads deliveries, runs and leases again (memory only), in ms.
inline constexpr int LIVE_INTERVAL_MS = 1000;

// EVENT_LABELS: friendly names for event types, in the order the chooser offers them.
const std::vector<std::pair<std::string_view, std::string_view>>& event_labels();
// DEFAULT_EVENTS: what a new webhook or program sends until the user picks something else.
const std::vector<std::string>& default_events();
// STAGE_LABELS[stage], or the stage itself for one without a label.
std::string stage_label(std::string_view stage);

// "Test" for a type ending in ".test", else the friendly name, else the type itself.
std::string event_label(std::string_view event_type);
// Every public event type: the friendly ones first, any newer ones after.
std::vector<std::string> event_types();
// "No events", the one label, or "<first> +<others>".
std::string events_summary(const std::vector<std::string>& events);

// "HH:MM:SS" in the clock's zone for an ISO-8601 instant (a trailing "Z" counts as UTC; a naive
// value is local time already). Text that is not an instant comes back as it was given.
std::string clock(std::string_view at, const Clock& now = system_clock());

// A delivery or run as one short line, and whether it went well.
struct Outcome {
    std::string text;
    bool ok = false;
    friend bool operator==(const Outcome&, const Outcome&) = default;
};
// "200 · 120 ms" (with " · try N" past the first attempt), or the error, or "No answer".
Outcome delivery_result(const automation::DeliveryRecord& record);
// "Exit 0 · 340 ms", or the error, or "Didn't run".
Outcome run_result(const automation::RunRecord& record);

// A gentle note for plain http to a host outside this computer and network; "" for anything
// else, an address urlsplit refuses included.
std::string unencrypted_note(std::string_view url);
// urlsplit(url).netloc (the port kept), or the address itself when it has none or cannot be split.
std::string host_of(std::string_view url);

} // namespace fv::hooks_format
