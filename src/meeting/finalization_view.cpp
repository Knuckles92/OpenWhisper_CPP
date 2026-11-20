#include "meeting/finalization_view.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <tuple>

#include "meeting/detail/py_value.h"
#include "pyport/errors.h"
#include "pyport/text.h"

namespace fv::meeting {

namespace {

struct StatusText {
    std::string_view status;
    std::string_view title;
    std::string_view fallback;
};

constexpr std::array<StatusText, 5> kStatusTexts{{
    {"running", "Finalizing Meeting", "Preparing final insights…"},
    {"completed", "Final Insights Ready", "Final insights are ready."},
    {"disabled", "AI Insights Off", "AI insights are off for this meeting."},
    {"unavailable", "Final Insights Unavailable", "Final insights could not run."},
    {"failed", "Final Insights Incomplete", "Final insights failed."},
}};

bool is_optional_rerun_step(std::string_view step_id) {
    return std::ranges::find(kOptionalRerunSteps, step_id) != kOptionalRerunSteps.end();
}

// str(step.get(key) or "")
std::string step_text(const Json& step, std::string_view key) {
    return detail::str_or_empty(detail::get(step, key));
}

// str(step.get("status", "pending")): the raw status, missing as "pending".
std::string raw_status(const Json& step) {
    if (!step.is_object() || !step.contains("status")) {
        return "pending";
    }
    return detail::py_str(step.at("status"));
}

const std::string kStatusKey = "status";

} // namespace

FinalizationView FinalizationView::read(const Json& finalization) {
    FinalizationView view;
    view.status = detail::str_or_empty(detail::get(finalization, "status"));
    view.message = py::strip(detail::str_or_empty(detail::get(finalization, "message")));
    view.current_step = detail::int_or_zero(detail::get(finalization, "current_step"));
    view.total_steps = detail::int_or_zero(detail::get(finalization, "total_steps"));
    view.step_details = py::strip(detail::str_or_empty(detail::get(finalization, "step_details")));
    view.steps = detail::list_or_empty(detail::get(finalization, "steps"));
    const Json& content = detail::get(finalization, "content_summary");
    if (content.is_object()) {
        view.content = content;
    } else if (detail::truthy(content)) {
        throw py::TypeError("dictionary update sequence element #0 is not a sequence");
    }
    view.meeting_failed =
        detail::str_or_empty(detail::get(view.content, "meeting_status")) == "failed";
    view.empty_meeting = detail::truthy(detail::get(view.content, "is_empty"));
    return view;
}

std::pair<std::string, std::string> finalization_heading(std::string_view status,
                                                         std::string_view message,
                                                         bool has_failed_steps, bool empty_meeting,
                                                         bool meeting_failed) {
    std::string title;
    std::string fallback;
    if (has_failed_steps && status == "completed") {
        title = "Meeting Finished With Issues";
        fallback = "Some post-meeting steps failed. The recording was kept.";
    } else {
        const auto it = std::ranges::find(kStatusTexts, status, &StatusText::status);
        title = it != kStatusTexts.end() ? std::string(it->title) : "Final insights";
        fallback = it != kStatusTexts.end() ? std::string(it->fallback) : "";
    }
    if (meeting_failed && empty_meeting) {
        title = "Meeting Failed";
    } else if (empty_meeting) {
        title = "Empty Meeting";
    }
    return {title, message.empty() ? fallback : std::string(message)};
}

StepIcon step_icon(std::string_view step_status) {
    if (step_status == "completed") {
        return {"✓", "@success-text"};
    }
    if (step_status == "running") {
        return {"●", "@accent"};
    }
    if (step_status == "failed") {
        return {"✗", "@danger-text"};
    }
    if (step_status == "skipped") {
        return {"–", "@text-muted"};
    }
    return {"○", "@text-muted"};
}

std::string step_detail_text(std::string_view step_status) {
    if (step_status == "running") {
        return "In progress";
    }
    if (step_status == "completed") {
        return "Done";
    }
    if (step_status == "failed") {
        return "Failed";
    }
    if (step_status == "skipped") {
        return "Skipped";
    }
    return "Queued";
}

std::string step_action_label(std::string_view step_id, std::string_view step_status,
                              bool allow_actions) {
    if (!allow_actions || step_id.empty()) {
        return "";
    }
    if (step_status == "failed" || step_status == "skipped") {
        return "Retry";
    }
    if (step_status == "completed" && is_optional_rerun_step(step_id)) {
        return "Run again";
    }
    return "";
}

int progress_percent(std::int64_t current_step, std::int64_t total_steps) {
    if (total_steps == 0) {
        // Python's float division raises here; the card asks only when there are steps.
        throw py::PyError("ZeroDivisionError", "float division by zero");
    }
    const double middle =
        ((static_cast<double>(current_step) - 0.5) / static_cast<double>(total_steps)) * 100.0;
    // int(max(5, min(95, x))): clamped, then truncated.
    const double capped = middle < 95.0 ? middle : 95.0;
    const double floored = capped > 5.0 ? capped : 5.0;
    return static_cast<int>(std::trunc(floored));
}

std::string elapsed_text(double seconds) {
    // int(max(0, elapsed)): truncated, and NaN reads as 0 as in Python (the comparison fails).
    // Values past int64 cannot occur on a meeting clock; they are held at its range.
    constexpr double kLimit = 9.0e18;
    const double clamped = seconds > 0.0 ? std::min(seconds, kLimit) : 0.0;
    const auto total = static_cast<std::int64_t>(std::trunc(clamped));
    const std::int64_t hours = total / 3600;
    const std::int64_t minutes = (total % 3600) / 60;
    const std::int64_t secs = total % 60;
    if (hours != 0) {
        return std::format("{}:{:02d}:{:02d}", hours, minutes, secs);
    }
    return std::format("{:02d}:{:02d}", minutes, secs);
}

std::pair<std::string, std::string> meeting_audio_support_copy(const PlatformInfo& info) {
    const std::string platform =
        info.platform.empty() ? current_platform().platform : info.platform;
    std::string subtitle = "Capture microphone audio and, when supported, system audio. Follow "
                           "the transcript and insights in the browser dashboard.";
    std::string hint;
    if (platform.starts_with("win")) {
        hint = "System audio uses Windows WASAPI loopback when available.";
    } else if (platform == "darwin" && meeting_mode_supported(info)) {
        hint = "System audio uses macOS ScreenCaptureKit, which needs Screen Recording "
               "permission. Without it, meetings capture microphone audio only.";
    } else if (platform.starts_with("linux")) {
        if (linux_meeting_implementation_ready(info.machine)) {
            hint = "Meeting Mode on Linux is a preview. System audio comes through PulseAudio "
                   "or PipeWire, or you can continue with the microphone only.";
        } else {
            hint = std::format("{} on this architecture is not a supported Meeting Mode "
                               "platform. Meetings here capture microphone audio only and are "
                               "unsupported.",
                               meeting_unsupported_os_name(platform));
        }
    } else {
        hint = std::format("{} is not a supported Meeting Mode platform. System-audio capture "
                           "needs Windows or macOS 13+. Meetings here capture microphone audio "
                           "only and are unsupported.",
                           meeting_unsupported_os_name(platform));
    }
    return {subtitle, hint};
}

std::string meeting_audio_platform_tone(const PlatformInfo& info) {
    const std::string_view platform =
        info.platform.empty() ? std::string_view(current_platform().platform) : info.platform;
    if (platform.starts_with("linux") && linux_meeting_implementation_ready(info.machine)) {
        return "note";
    }
    return "warning";
}

bool meeting_audio_shows_platform_warning(const PlatformInfo& info) {
    const std::string_view platform =
        info.platform.empty() ? std::string_view(current_platform().platform) : info.platform;
    if (platform.starts_with("win")) {
        return false;
    }
    if (platform == "darwin" && meeting_mode_supported(info)) {
        return false;
    }
    // Linux remains publicly unsupported until hardware attestation, so the idle warning stays
    // visible even when the implementation is ready.
    return true;
}

CardDecision decide_card(const FinalizationView& view, bool background_available) {
    CardDecision card;
    const Json& content = view.content;
    const Json& can_rerun_value = detail::get(content, "can_rerun_speakers");
    const bool can_rerun_speakers = detail::truthy(can_rerun_value);
    const bool rerun_offered = !content.contains("speaker_rerun_offered") ||
                               detail::truthy(content.at("speaker_rerun_offered"));
    const std::string rerun_reason =
        detail::str_or_empty(detail::get(content, "speaker_rerun_reason"));
    const bool rerun_allowed = can_rerun_speakers && rerun_reason.empty();
    card.rerun_speakers_tip =
        !can_rerun_speakers
            ? "No system-audio recording is available for speaker identification"
            : (rerun_reason.empty() ? std::string("Re-run speaker identification") : rerun_reason);

    const bool running = view.status == "running";
    const bool no_report = view.empty_meeting || view.meeting_failed || running;
    card.report_enabled = !no_report;
    if (view.empty_meeting || view.meeting_failed) {
        card.report_tip = "No report for this meeting";
    } else if (running) {
        card.report_tip = "The final report is still being prepared";
    } else {
        card.report_tip = "Open the final report in the web dashboard";
    }

    std::string message = view.message;
    if (view.empty_meeting) {
        message = view.meeting_failed
                      ? "No audio or transcript was captured. The meeting failed before it could "
                        "start, so no dashboard was created."
                      : "No audio or transcript was captured for this meeting.";
    }

    bool has_failed_steps = false;
    bool has_speaker_step = false;
    for (const auto& step : view.steps) {
        has_failed_steps = has_failed_steps || step_text(step, kStatusKey) == "failed";
        has_speaker_step = has_speaker_step || step_text(step, "id") == "speaker_id";
    }
    const bool needs_retry =
        has_failed_steps || view.status == "failed" || view.status == "unavailable";
    std::tie(card.title, card.message) = finalization_heading(
        view.status, message, has_failed_steps, view.empty_meeting, view.meeting_failed);
    card.keep_hint = view.empty_meeting
                         ? "This failed start has no audio or transcript."
                         : "This meeting, transcript, and audio stay in Past Meetings.";
    card.show_background = running && background_available;

    // The current step's detail line, unless it repeats the message.
    const auto show_step_details = [&] {
        card.show_detail = !view.step_details.empty() && view.step_details != message;
        card.detail = card.show_detail ? view.step_details : "";
    };
    const auto show_steps = [&](bool allow_actions) {
        card.show_steps = !view.steps.empty();
        if (!card.show_steps) {
            return;
        }
        int index = 0;
        for (const auto& step : view.steps) {
            ++index;
            StepRow row;
            row.id = step_text(step, "id");
            row.status = raw_status(step);
            row.name = step_text(step, "name");
            if (row.name.empty()) {
                row.name = std::format("Step {}", index);
            }
            row.detail = step_text(step, "detail");
            row.icon = step_icon(row.status);
            row.running = row.status == "running";
            row.detail_text = step_detail_text(row.status);
            row.action_label = step_action_label(row.id, row.status, allow_actions);
            if (!row.action_label.empty() && row.id == "speaker_id" && !rerun_allowed) {
                row.action_enabled = false;
                row.action_tooltip = card.rerun_speakers_tip;
            }
            card.steps.push_back(std::move(row));
        }
    };
    const auto offer_speaker_rerun = [&](bool visible) {
        card.show_rerun_speakers = visible;
        card.rerun_speakers_enabled = rerun_allowed;
    };

    if (running) {
        card.tone = "neutral";
        card.show_progress = true;
        card.show_active_box = true;
        if (view.total_steps > 0) {
            card.step_badge = std::format("Step {} of {}", view.current_step, view.total_steps);
            card.progress_minimum = 0;
            card.progress_maximum = 100;
            card.progress_value = progress_percent(view.current_step, view.total_steps);
        } else {
            card.step_badge = "In progress";
            card.progress_minimum = 0;
            card.progress_maximum = 0;
        }
        card.show_step_badge = true;
        show_step_details();
        show_steps(false);
        // Hidden while running; enabled as it would be once shown.
        card.rerun_speakers_enabled = rerun_allowed;
        return card;
    }

    const bool clean_completion = view.status == "completed" && !has_failed_steps;
    if (clean_completion) {
        card.tone = "success";
        offer_speaker_rerun(!has_speaker_step && rerun_offered);
        card.show_done = true;
        // The header and checklist already cover a successful finish; the stats recap lives on
        // the dashboard.
        card.show_active_box = view.empty_meeting;
        show_steps(true);
    } else if (needs_retry) {
        card.tone = "warning";
        card.show_retry = true;
        offer_speaker_rerun(!has_speaker_step && rerun_offered);
        card.show_keep_later = true;
        card.show_active_box = true;
        show_step_details();
        show_steps(true);
    } else {
        card.tone = "info";
        card.show_active_box = true;
        offer_speaker_rerun(rerun_offered);
        card.show_done = true;
    }
    return card;
}

} // namespace fv::meeting
