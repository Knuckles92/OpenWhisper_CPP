#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/json.h"

// The shared presentation of live and retried finalization steps (meeting/finalization.py):
// the step table, the card's counts and sentences, the speaker-pass gate, the polish windows
// and the bounded agent call every post-meeting pass goes through.
namespace fv::meeting {

// ---- the bounded agent call ----

// How long a cancelled agent call gets to unwind before it is abandoned.
inline constexpr std::chrono::seconds kCancelGrace{5};

// Why run_agent_call has no result: the call threw (`message` is what() it threw), or it ran
// past the timeout and was cancelled (`message` is empty), as Python's None.
struct AgentCallError {
    bool timed_out = false;
    std::string message;

    bool operator==(const AgentCallError&) const = default;
};

namespace finalization_detail {
void log_call_raised(std::string_view name, const std::exception* error);
void log_call_timed_out(std::string_view name, std::chrono::milliseconds timeout);
void log_timeout_handler_raised(std::string_view name, const std::exception* error);
void log_cancel_raised(std::string_view name, const std::exception* error);
} // namespace finalization_detail

// Runs one blocking agent call on a thread of its own, bounded by `timeout`. On timeout
// `on_timeout` runs first (to revoke agent writes, so a call that ignores the cancel cannot land
// them), then the call's token is cancelled, then the call gets kCancelGrace to unwind and is
// abandoned: it keeps running detached, so it must own everything it touches. `name` is used in
// log lines.
template <class T>
std::expected<T, AgentCallError>
run_agent_call(std::function<T(const CancelToken&)> call, std::chrono::milliseconds timeout,
               std::string_view name, std::function<void()> on_timeout = {}) {
    struct Shared {
        std::mutex mutex;
        std::condition_variable done;
        std::optional<std::expected<T, AgentCallError>> outcome;
    };
    auto shared = std::make_shared<Shared>();
    CancelSource source;
    std::thread([shared, call = std::move(call), token = source.token(),
                 label = std::string(name)]() mutable {
        std::optional<std::expected<T, AgentCallError>> outcome;
        try {
            outcome.emplace(call(token));
        } catch (const std::exception& error) {
            finalization_detail::log_call_raised(label, &error);
            outcome.emplace(std::unexpected(AgentCallError{false, error.what()}));
        } catch (...) {
            finalization_detail::log_call_raised(label, nullptr);
            outcome.emplace(std::unexpected(AgentCallError{false, "unknown error"}));
        }
        {
            const std::lock_guard lock(shared->mutex);
            shared->outcome = std::move(outcome);
        }
        shared->done.notify_all();
    }).detach();

    std::unique_lock lock(shared->mutex);
    if (shared->done.wait_for(lock, timeout, [&] { return shared->outcome.has_value(); })) {
        return std::move(*shared->outcome);
    }
    lock.unlock();
    finalization_detail::log_call_timed_out(name, timeout);
    if (on_timeout) {
        try {
            on_timeout();
        } catch (const std::exception& error) {
            finalization_detail::log_timeout_handler_raised(name, &error);
        } catch (...) {
            finalization_detail::log_timeout_handler_raised(name, nullptr);
        }
    }
    try {
        source.cancel();
    } catch (const std::exception& error) {
        finalization_detail::log_cancel_raised(name, &error);
    }
    lock.lock();
    shared->done.wait_for(lock, kCancelGrace, [&] { return shared->outcome.has_value(); });
    return std::unexpected(AgentCallError{true, {}});
}

// ---- transcript cleanup windows ----

// The budget of one cleanup block, provider tool rounds included, in seconds.
inline constexpr double kPolishTimeoutS = 180.0;
// Cleanup emits a revision per changed segment, so large batches can spend the whole budget
// reasoning before one edit lands: both the segment count and the text (in code points) of a
// block are bounded, a little boundary context included.
inline constexpr std::size_t kPolishMaxSegments = 80;
inline constexpr std::size_t kPolishMaxTextChars = 8'000;
inline constexpr std::size_t kPolishOverlapSegments = 8;

// Covers the transcript with bounded, overlapping cleanup windows, as [start, end) indices into
// `segments`. A single oversized segment stays whole (splitting it would change its identity),
// and the overlap shrinks when needed so every block advances into unseen segments. A segment's
// size is the code points of str(segment.get("text") or "").
std::vector<std::pair<std::size_t, std::size_t>> polish_blocks(const std::vector<Json>& segments);

// ---- the card's counts and sentences ----

// The counts on the finalization card, shared by live End and retry: {segments, words,
// key_points, action_items, decisions, risks, questions, duration_s}. Items whose status is
// "removed" and questions "dismissed" are left out, so the card matches the dashboard and the
// exports. `cards` is {card_key: [item, ...]} (anything else reads as {}), `duration_s` the
// recorded seconds with pauses excluded.
Json summary_stats(const Json& cards, const Json& questions, const std::vector<Json>& transcript,
                   double duration_s);
// The same over a meeting state's "cards" and "questions".
Json summary_stats(const Json& state, const std::vector<Json>& segments, double duration_s);

// "Saved {segments} segments ({words} words)": the finalize step's detail once state is saved.
std::string saved_state_detail(const Json& stats);
// "Final insights ready — 32 segments, 4 key points, 1 decisions.": only the counts found.
std::string insights_ready_message(const Json& stats);
std::string sparse_redecode_detail(std::int64_t new_words, std::int64_t old_words);

// ---- the steps ----

inline constexpr std::array<std::string_view, 5> kStepOrder{"redecode", "speaker_id", "polish",
                                                            "consolidation", "finalize"};

// STEP_NAMES and STEP_DETAILS: nullopt for an id that is not a step.
std::optional<std::string_view> step_name(std::string_view step_id);
std::optional<std::string_view> step_detail(std::string_view step_id);

// {"id", "name", "status", "detail"}; Python's KeyError (py::PyError) for an unknown id.
Json make_step(std::string_view step_id, std::string_view status = "pending");

// "Transcript Cleanup, Summary & Action Items failed. The recording and transcript were kept.
// Transcript Cleanup: <detail> ..." over the failed steps of `steps` (a list of step dicts), or
// "" when none failed. A step without a name is named by its id.
std::string failed_steps_message(const Json& steps);

// (status, detail) of the speaker step from a speaker pass's {ok, skipped, error, applied}: a
// skip is completed, not failed (meeting/refinalize.py:speaker_step_outcome).
std::pair<std::string, std::string> speaker_step_outcome(const Json& result);

// ---- the OpenAI speaker pass gate ----

inline constexpr std::string_view kNotOpenaiReason = "Speaker identification is not set to OpenAI.";
inline constexpr std::string_view kNoConsentReason = "Audio-upload consent has not been given.";
inline constexpr std::string_view kNoKeyReason = "No OpenAI API key is configured.";

// Whether the OpenAI speaker pass may upload a meeting's system audio. The key itself is never
// part of the gate: the pass reads it when it runs.
struct SpeakerPassGate {
    // Every condition holds.
    bool ok = false;
    // Why the pass is refused; empty when ok.
    std::string reason;
    // The user chose OpenAI labels and OpenAI still serves them, so a refusal is one they can fix
    // (consent or key).
    bool offered = false;

    bool operator==(const SpeakerPassGate&) const = default;
};

// The one eligibility check for every OpenAI speaker pass (live End, the dashboard's re-run, the
// desktop retry). Checked in order: the backend is "openai", OpenAI has not retired the model
// on `today`, consent, then a key, which `key_present` reports and is asked only when every
// other check passed. An empty `key_present` means no key is needed (an injected decoder).
SpeakerPassGate speaker_pass_gate(std::string_view backend, bool consent,
                                  const std::function<bool()>& key_present,
                                  std::chrono::year_month_day today);
// The same, judged on `clock`'s local date.
SpeakerPassGate speaker_pass_gate(std::string_view backend, bool consent,
                                  const std::function<bool()>& key_present, const Clock& clock);

} // namespace fv::meeting
