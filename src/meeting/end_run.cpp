#include "meeting/end_run.h"

#include <exception>
#include <format>
#include <stdexcept>
#include <tuple>

#include "base/log.h"
#include "meeting/detail/py_value.h"
#include "meeting/finalization.h"

namespace fv::meeting {

namespace {

const log::Logger logger("meeting.end_run");

// The headline while each step runs; finalize has none of its own.
std::string_view running_message(std::string_view step_id) {
    if (step_id == "redecode") {
        return "Re-transcribing meeting…";
    }
    if (step_id == "speaker_id") {
        return "Identifying speakers…";
    }
    if (step_id == "polish") {
        return "Cleaning transcript…";
    }
    if (step_id == "consolidation") {
        return "Preparing final report…";
    }
    return "Finalizing meeting state…";
}

// getattr(outcome, key, fallback) over an outcome dict.
std::string outcome_field(const Json& outcome, std::string_view key, std::string_view fallback) {
    if (!outcome.is_object() || !outcome.contains(key)) {
        return std::string(fallback);
    }
    return detail::py_str(outcome.at(std::string(key)));
}

} // namespace

Json FinalizationUpdate::to_json() const {
    Json out = Json::object();
    out["status"] = status;
    out["message"] = message;
    out["stage"] = stage;
    out["current_step"] = current_step;
    out["total_steps"] = total_steps;
    out["step_details"] = step_details;
    if (steps) {
        out["steps"] = *steps;
    }
    if (summary_stats) {
        out["summary_stats"] = *summary_stats;
    }
    out["emit"] = emit;
    return out;
}

EndRun::EndRun(Effects effects, EndOptions options)
    : effects_(std::move(effects)), options_(std::move(options)) {}

bool EndRun::set_finalization(FinalizationUpdate update) {
    return effects_.set_finalization(update);
}

void EndRun::update_step(std::string_view step_id, std::string_view status,
                         std::string_view detail_msg, std::string_view message, bool emit) {
    if (status == "failed") {
        logger.warning("Meeting finalization meeting_id={} stage={} status={} detail={}",
                       options_.meeting_id, step_id, status, detail_msg);
    } else {
        logger.info("Meeting finalization meeting_id={} stage={} status={} detail={}",
                    options_.meeting_id, step_id, status, detail_msg);
    }
    int current = 1;
    int index = 0;
    for (auto& step : steps_) {
        ++index;
        const Json& id = detail::get(step, "id");
        if (id.is_string() && id.get_ref<const std::string&>() == step_id) {
            step["status"] = std::string(status);
            if (!detail_msg.empty()) {
                step["detail"] = std::string(detail_msg);
            }
            current = index;
            break;
        }
    }
    FinalizationUpdate update;
    update.status = "running";
    update.message = message.empty() ? std::string(running_message(step_id)) : std::string(message);
    update.stage = std::string(step_id);
    update.current_step = current;
    update.total_steps = total_steps_;
    update.step_details = std::string(detail_msg);
    update.steps = steps_;
    update.emit = emit;
    set_finalization(std::move(update));
}

ProgressCallback EndRun::progress(std::string step_id,
                                  std::function<std::string(int current, int total)> headline) {
    return [this, step_id = std::move(step_id),
            headline = std::move(headline)](const std::string& detail, int current, int total) {
        update_step(step_id, "running", detail, headline(current, total));
    };
}

std::pair<std::string, std::string> EndRun::speaker_outcome() const {
    if (speaker_ok_) {
        if (cloud_enabled_) {
            return {"completed", "Speaker identification finished."};
        }
        return {"completed",
                "Speaker identification finished. AI insights are off for this meeting."};
    }
    if (speaker_skipped_) {
        return {"completed",
                speaker_error_.empty() ? "Speaker identification skipped." : speaker_error_};
    }
    return {"failed", speaker_error_.empty() ? "Speaker identification failed." : speaker_error_};
}

const Json& EndRun::plan(bool cloud_enabled, bool want_polish, bool want_report) {
    cloud_enabled_ = cloud_enabled;
    want_polish_ = want_polish;
    want_report_ = want_report;
    run_cloud_ = cloud_enabled_ && options_.complete && options_.has_scheduler &&
                 effects_.core_is_healthy() && (want_polish_ || want_report_);
    const std::pair<std::string_view, bool> wanted[] = {
        {"redecode", options_.will_offline},
        {"speaker_id", options_.will_speaker_id},
        {"polish", run_cloud_ && want_polish_},
        {"consolidation", run_cloud_ && want_report_},
        {"finalize", true},
    };
    steps_ = Json::array();
    for (const auto& [step_id, enabled] : wanted) {
        if (enabled) {
            steps_.push_back(make_step(step_id));
        }
    }
    total_steps_ = static_cast<int>(steps_.size());
    return steps_;
}

void EndRun::announce() {
    if (options_.will_offline || options_.will_speaker_id || run_cloud_) {
        const Json first = steps_.at(0);
        update_step(first.at("id").get<std::string>(), "running",
                    first.at("detail").get<std::string>(), "", false);
    } else if (!cloud_enabled_) {
        set_finalization(
            {.status = "disabled", .message = std::string(kInsightsOffMessage), .emit = false});
    } else if (!options_.complete) {
        set_finalization({.status = "unavailable",
                          .message = std::string(kRecoveryPendingMessage),
                          .emit = false});
    } else {
        set_finalization(
            {.status = "unavailable", .message = effects_.offline_message(), .emit = false});
    }
}

bool EndRun::redecode() {
    if (!options_.will_offline) {
        return offline_ok_;
    }
    update_step("redecode", "running", "Starting high-accuracy session audio re-decoding...",
                "Re-transcribing meeting…");
    const ProgressCallback report = progress("redecode", [](int current, int total) {
        return std::format("Re-transcribing meeting (window {}/{})…", current, total);
    });
    try {
        offline_ok_ = effects_.redecode(report);
    } catch (const std::exception& error) {
        logger.error_with(error, "Offline clean ASR pass failed");
        offline_ok_ = false;
    }
    if (offline_ok_) {
        update_step("redecode", "completed", "High-accuracy re-decoding complete");
    } else {
        std::string failure = effects_.redecode_failure();
        update_step("redecode", "failed",
                    failure.empty() ? "Re-decoding failed; kept live transcript" : failure);
    }
    return offline_ok_;
}

void EndRun::identify_speakers() {
    if (!options_.will_speaker_id) {
        return;
    }
    const std::function<void()> announce_upload = [this] {
        update_step("speaker_id", "running", "Uploading system audio for speaker labels…",
                    "Identifying speakers…");
    };
    Json result;
    try {
        result = effects_.identify_speakers(
            progress("speaker_id",
                     [](int current, int total) {
                         return std::format("Identifying speakers (window {}/{})…", current, total);
                     }),
            announce_upload);
    } catch (const std::exception& error) {
        logger.error_with(error, "Cloud speaker identification failed");
        result =
            Json{{"ok", false}, {"skipped", false}, {"error", "Speaker identification failed."}};
    }
    speaker_ok_ = detail::truthy(detail::get(result, "ok"));
    speaker_skipped_ = detail::truthy(detail::get(result, "skipped"));
    speaker_error_ = detail::str_or_empty(detail::get(result, "error"));
    const auto [step_status, step_detail] = speaker_step_outcome(result);
    update_step("speaker_id", step_status, step_detail);
}

void EndRun::insights() {
    if (run_cloud_ && options_.has_scheduler) {
        run_cloud_passes();
    } else {
        effects_.revoke_agent_writes();
        report_without_cloud();
    }
}

void EndRun::run_cloud_passes() {
    std::string status;
    std::string message;
    try {
        effects_.allow_agent_writes();
        if (want_polish_) {
            polish();
        }
        if (want_report_) {
            std::tie(status, message) = consolidate();
        } else {
            status = "completed";
            message = "Transcript cleanup is ready.";
        }
    } catch (const std::exception& error) {
        logger.error_with(error, "Post-end consolidation pass failed");
        effects_.revoke_agent_writes();
        const std::string failed = std::format("Final insights failed: {}", error.what());
        set_finalization({.status = "failed",
                          .message = failed,
                          .stage = "failed",
                          .current_step = total_steps_,
                          .total_steps = total_steps_,
                          .step_details = failed,
                          .steps = steps_});
        return;
    }
    effects_.revoke_agent_writes();
    complete_run(status, message);
}

void EndRun::polish() {
    update_step("polish", "running", "Starting AI transcript cleanup and formatting...",
                "Cleaning transcript…");
    const ProgressCallback report = progress("polish", [](int current, int total) {
        return std::format("Cleaning transcript (block {}/{})…", current, total);
    });
    std::string status = "completed";
    std::string detail_text = "Transcript cleanup finished";
    if (effects_.polish) {
        try {
            const Json outcome = effects_.polish(kPolishTimeoutS, report);
            status = outcome_field(outcome, "status", "completed");
            detail_text = outcome_field(outcome, "message", "");
            if (detail_text.empty()) {
                detail_text = status == "completed" ? "Transcript cleanup finished"
                                                    : "Transcript cleanup failed";
            }
        } catch (const std::exception& error) {
            logger.error_with(error, "Post-end transcript polish failed");
            status = "failed";
            detail_text = std::format("Transcript cleanup failed: {}", error.what());
        }
    }
    update_step("polish", status == "completed" ? "completed" : "failed", detail_text);
}

std::pair<std::string, std::string> EndRun::consolidate() {
    update_step("consolidation", "running",
                "Synthesizing executive summary, key points, decisions, and action items...",
                "Preparing final report…");
    if (!effects_.consolidate) {
        throw std::logic_error("the scheduler has no consolidation pass");
    }
    // live_notes are kept for consolidation, so the final report can synthesize the meeting
    // notes and reconcile them against the final transcript.
    const Json outcome = effects_.consolidate([this](const std::string& detail) {
        update_step("consolidation", "running", detail, "Preparing final report…");
    });
    std::string status = outcome_field(outcome, "status", "failed");
    std::string message = outcome_field(outcome, "message", "");
    if (status == "completed") {
        update_step("consolidation", "completed", "Summary & action items ready");
    } else {
        update_step("consolidation", "failed", message);
    }
    return {std::move(status), std::move(message)};
}

void EndRun::complete_run(std::string status, const std::string& message) {
    update_step("finalize", "running", "Saving final transcript and meeting state...",
                "Finalizing meeting state…");
    const Json stats = effects_.summary_stats();
    update_step("finalize", "completed", saved_state_detail(stats));

    bool any_failed = false;
    for (const auto& step : steps_) {
        any_failed = any_failed || detail::get(step, "status") == "failed";
    }
    std::string final_message;
    if (any_failed) {
        status = "failed";
        final_message = failed_steps_message(steps_);
    } else if (status == "completed" && want_report_) {
        final_message = insights_ready_message(stats);
    } else {
        final_message = message;
    }
    set_finalization({.status = status,
                      .message = final_message,
                      .stage = status == "completed" ? "complete" : status,
                      .current_step = total_steps_,
                      .total_steps = total_steps_,
                      .step_details = final_message,
                      .steps = steps_,
                      // In place: GCC finds copy-initialization from a const Json ambiguous
                      // between Json's conversion operator and optional's converting constructor.
                      .summary_stats = std::optional<Json>(std::in_place, stats)});
}

void EndRun::report_without_cloud() {
    if (options_.will_speaker_id) {
        finish_after_speakers();
    } else if (!cloud_enabled_) {
        // Even after a completed re-decode: the steps are not passed, as OpenWhisper does.
        set_finalization({.status = "disabled", .message = std::string(kInsightsOffMessage)});
    } else if (!options_.complete) {
        // Cloud passes wait for recovery even when the re-decode worked; without a terminal
        // state here the dashboard would keep showing the re-decode as running.
        if (!options_.will_offline) {
            set_finalization(
                {.status = "unavailable", .message = std::string(kRecoveryPendingMessage)});
            return;
        }
        update_step("finalize", "completed", "Saved meeting state after re-transcription.", "",
                    false);
        set_finalization({.status = "unavailable",
                          .message = std::string(kRecoveryPendingMessage),
                          .stage = "unavailable",
                          .current_step = total_steps_,
                          .total_steps = total_steps_,
                          .step_details = std::string(kRecoveryPendingMessage),
                          .steps = steps_});
    } else if (!want_polish_ && !want_report_) {
        set_finalization(
            {.status = "completed", .message = "Post-meeting cleanup and report are off."});
    } else if (!options_.has_scheduler || !effects_.core_is_healthy()) {
        set_finalization({.status = "unavailable", .message = effects_.offline_message()});
    }
}

void EndRun::finish_after_speakers() {
    update_step("finalize", "completed", "Saved meeting state after speaker identification.");
    const auto [status, message] = speaker_outcome();
    set_finalization({.status = status,
                      .message = message,
                      .stage = status == "completed" ? "complete" : status,
                      .current_step = total_steps_,
                      .total_steps = total_steps_,
                      .step_details = message,
                      .steps = steps_});
}

} // namespace fv::meeting
