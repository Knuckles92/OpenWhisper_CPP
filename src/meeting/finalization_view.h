#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "meeting/platform_policy.h"

// The pure logic of the Meeting Mode tab (ui_qt/widgets/meeting_mode_tab.py): how a finalization
// payload reads, what the finalization card shows for it, the step rows, the elapsed clock and
// the platform sentences. The widgets apply these; nothing here touches Qt.
namespace fv::meeting {

// Steps that may run again after they completed.
inline constexpr std::array<std::string_view, 4> kOptionalRerunSteps{"redecode", "speaker_id",
                                                                     "polish", "consolidation"};

// A finalization payload, read once for the card (_FinalizationView.read).
struct FinalizationView {
    std::string status;
    std::string message;
    std::int64_t current_step = 0;
    std::int64_t total_steps = 0;
    std::string step_details;
    // The step dicts, as listed.
    Json steps = Json::array();
    // content_summary.
    Json content = Json::object();
    bool meeting_failed = false;
    bool empty_meeting = false;

    // Reads the payload as Python does: str(x or ""), stripped message and details, int(x or 0)
    // (py::ValueError or py::TypeError for a value int() refuses), list(...) of the steps,
    // dict(...) of content_summary (py::TypeError for anything but a dict).
    static FinalizationView read(const Json& finalization);
};

// The card's title and message (_finalization_heading): the message, or the status's default.
std::pair<std::string, std::string> finalization_heading(std::string_view status,
                                                         std::string_view message,
                                                         bool has_failed_steps, bool empty_meeting,
                                                         bool meeting_failed);

// A step's status glyph and its colour token ("✓", "@success-text").
struct StepIcon {
    std::string glyph;
    std::string color;

    bool operator==(const StepIcon&) const = default;
};
StepIcon step_icon(std::string_view step_status);
// "In progress", "Done", "Failed", "Skipped" or "Queued".
std::string step_detail_text(std::string_view step_status);
// "Retry", "Run again" or "" when the row offers no action.
std::string step_action_label(std::string_view step_id, std::string_view step_status,
                              bool allow_actions);

// The running card's progress, in percent: the middle of the current step, kept in [5, 95].
// Python's ZeroDivisionError (py::PyError) when `total_steps` is 0.
int progress_percent(std::int64_t current_step, std::int64_t total_steps);

// The on-air clock: "MM:SS" under an hour, "H:MM:SS" from one; negative reads as 0.
std::string elapsed_text(double seconds);

// The idle tab's capture sentences: (subtitle, hint) for the platform.
std::pair<std::string, std::string>
meeting_audio_support_copy(const PlatformInfo& info = current_platform());
// "note" for Linux's preview line, "warning" for unsupported platforms.
std::string meeting_audio_platform_tone(const PlatformInfo& info = current_platform());
// The idle platform sentence stays visible as a warning.
bool meeting_audio_shows_platform_warning(const PlatformInfo& info = current_platform());

// One row of the step list (_add_step_row).
struct StepRow {
    std::string id;
    // The name, or "Step <n>" for a step without one.
    std::string name;
    // str() of the raw status ("pending" when the step has none).
    std::string status;
    std::string detail;
    StepIcon icon;
    // The name is drawn DemiBold while the step runs.
    bool running = false;
    std::string detail_text;
    // Empty: no action button.
    std::string action_label;
    bool action_enabled = true;
    std::string action_tooltip;

    bool operator==(const StepRow&) const = default;
};

// What the finalization card shows for a payload (_render_finalization, _update_report_button
// and the background button of _apply_layout_state).
struct CardDecision {
    std::string title;
    std::string message;
    // finalizationTone: "neutral", "success", "warning" or "info".
    std::string tone;
    std::string keep_hint;

    bool report_enabled = false;
    std::string report_tip;

    bool show_step_badge = false;
    std::string step_badge;
    bool show_progress = false;
    // The bar's range and value; (0, 0) is the indeterminate bar, whose value is left as it is.
    int progress_minimum = 0;
    int progress_maximum = 1;
    int progress_value = 0;

    bool show_detail = false;
    std::string detail;

    bool show_steps = false;
    std::vector<StepRow> steps;

    bool show_active_box = false;
    bool show_retry = false;
    bool show_background = false;
    // "Keep for later" and the hint above it.
    bool show_keep_later = false;
    bool show_done = false;
    bool show_open_report = true;
    bool show_rerun_speakers = false;
    bool rerun_speakers_enabled = false;
    std::string rerun_speakers_tip;

    bool operator==(const CardDecision&) const = default;
};

// The card for `view` while no meeting is live; `background_available` is whether the running
// finalization may continue in the background.
CardDecision decide_card(const FinalizationView& view, bool background_available);

} // namespace fv::meeting
