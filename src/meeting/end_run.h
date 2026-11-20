#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/json.h"

// The finalization steps of one meeting End (meeting/end_run.py). After the engine has stopped
// capture, drained ASR, saved the terminal status and announced `ended`, what remains is a short
// sequence of optional passes: re-decode, speaker identification, transcript polish, the final
// report and a closing summary. EndRun plans which of those run, reports each one's progress as
// a finalization state and decides the outcome. It holds no engine state: every effect is a
// function the engine hands it.
namespace fv::meeting {

inline constexpr std::string_view kInsightsOffMessage = "AI insights are off for this meeting.";
inline constexpr std::string_view kRecoveryPendingMessage =
    "Final insights are unavailable until transcription recovery finishes.";

// (detail, current, total) of a pass working through windows or blocks.
using ProgressCallback = std::function<void(const std::string& detail, int current, int total)>;

// The arguments of the engine's _set_finalization: a status with its message, the step progress,
// and whether to push the update to listeners. `steps` and `summary_stats` are absent when the
// call did not pass them.
struct FinalizationUpdate {
    std::string status;
    std::string message;
    std::string stage;
    int current_step = 0;
    int total_steps = 0;
    std::string step_details;
    std::optional<Json> steps;
    std::optional<Json> summary_stats;
    bool emit = true;

    // {"status", "message", "stage", "current_step", "total_steps", "step_details", "steps"?,
    // "summary_stats"?, "emit"}
    Json to_json() const;
    bool operator==(const FinalizationUpdate&) const = default;
};

struct EndOptions {
    // The meeting being finalized, for logging.
    std::string meeting_id;
    // Every chunk was transcribed and stored.
    bool complete = false;
    // Chunks still waiting for transcription.
    std::int64_t unfinished = 0;
    // The intelligence scheduler exists (meeting intelligence is online).
    bool has_scheduler = false;
    // The offline re-decode pass will run.
    bool will_offline = false;
    // The cloud speaker identification will run.
    bool will_speaker_id = false;
};

// Affinity: one thread (the engine's End worker) for its whole life.
class EndRun {
public:
    struct Effects {
        // Persists and announces a finalization state; true when it was persisted.
        std::function<bool(const FinalizationUpdate&)> set_finalization;
        // Runs the offline re-decode pass; true when a new transcript was committed.
        std::function<bool(const ProgressCallback&)> redecode;
        // Why the last re-decode kept the draft, or "".
        std::function<std::string()> redecode_failure;
        // Runs the cloud speaker pass; `on_start` runs once the upload begins. Returns
        // {ok, skipped, error, applied}.
        std::function<Json(const ProgressCallback& progress, const std::function<void()>& on_start)>
            identify_speakers;
        // Let and stop the agent changing the meeting state.
        std::function<void()> allow_agent_writes;
        std::function<void()> revoke_agent_writes;
        // The agent core can take a pass.
        std::function<bool()> core_is_healthy;
        // Why meeting intelligence is offline.
        std::function<std::string()> offline_message;
        // Final statistics for the closing summary (summary_stats' shape).
        std::function<Json()> summary_stats;
        // The scheduler's final polish over the stored transcript with a per-block budget in
        // seconds; {status, message}. Empty when the scheduler has none, which counts as done.
        std::function<Json(double timeout_s, const ProgressCallback&)> polish;
        // The scheduler's consolidation (the final report); {status, message}.
        std::function<Json(const std::function<void(const std::string&)>& progress)> consolidate;
    };

    EndRun(Effects effects, EndOptions options);

    // Chooses the steps this End runs and returns them (make_step dicts, all pending).
    const Json& plan(bool cloud_enabled, bool want_polish, bool want_report);
    // Publishes the opening finalization state, without emitting, before `ended` goes out.
    void announce();
    // The offline re-decode, when planned; returns whether it committed a new transcript.
    bool redecode();
    void identify_speakers();
    // The cloud passes when they run, else the outcome without them.
    void insights();

    // Marks one step and publishes a running state: `message` is the headline (by default the
    // step's running message), `detail` replaces the step's detail when not empty. An unknown
    // step id counts as step 1.
    void update_step(std::string_view step_id, std::string_view status,
                     std::string_view detail = "", std::string_view message = "", bool emit = true);
    // Final status and message when speaker identification ends the run.
    std::pair<std::string, std::string> speaker_outcome() const;

    const std::string& meeting_id() const { return options_.meeting_id; }
    bool complete() const { return options_.complete; }
    std::int64_t unfinished() const { return options_.unfinished; }
    bool has_scheduler() const { return options_.has_scheduler; }
    bool will_offline() const { return options_.will_offline; }
    bool will_speaker_id() const { return options_.will_speaker_id; }
    bool cloud_enabled() const { return cloud_enabled_; }
    bool want_polish() const { return want_polish_; }
    bool want_report() const { return want_report_; }
    bool run_cloud() const { return run_cloud_; }
    const Json& steps() const { return steps_; }
    int total_steps() const { return total_steps_; }
    bool offline_ok() const { return offline_ok_; }
    bool speaker_ok() const { return speaker_ok_; }
    bool speaker_skipped() const { return speaker_skipped_; }
    const std::string& speaker_error() const { return speaker_error_; }

private:
    ProgressCallback progress(std::string step_id,
                              std::function<std::string(int current, int total)> headline);
    bool set_finalization(FinalizationUpdate update);
    void run_cloud_passes();
    void polish();
    std::pair<std::string, std::string> consolidate();
    void complete_run(std::string status, const std::string& message);
    void report_without_cloud();
    void finish_after_speakers();

    Effects effects_;
    EndOptions options_;
    bool cloud_enabled_ = false;
    bool want_polish_ = false;
    bool want_report_ = false;
    bool run_cloud_ = false;
    Json steps_ = Json::array();
    int total_steps_ = 0;
    bool offline_ok_ = false;
    bool speaker_ok_ = false;
    bool speaker_skipped_ = false;
    std::string speaker_error_;
};

} // namespace fv::meeting
