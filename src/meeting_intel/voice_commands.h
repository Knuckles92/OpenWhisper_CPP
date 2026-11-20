#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/cancel.h"
#include "base/executor.h"
#include "base/json.h"
#include "llm/judge.h"
#include "meeting_state/store.h"
#include "pyport/regex.h"

namespace fv::meeting_intel {

std::optional<py::Regex> compile_wake_pattern(const std::vector<std::string>& names);
// mentions_assistant: whether `text` names the assistant; never with no pattern.
bool mentions_assistant(std::string_view text, const std::optional<py::Regex>& pattern);
std::optional<std::string> extract_spoken_topic(std::string_view text);
// The one or two segments just before the command (ending at most 20 s before it starts) that
// "that" most likely names, oldest first, skipping `exclude_ids` and the command itself.
std::vector<Json> referent_rows(const std::vector<Json>& previous, const Json& command,
                                const std::vector<std::string>& exclude_ids = {});
// The store ops for a recognised command; empty when nothing safe applies. Card commands cite
// referent_rows(previous, row, exclude_ids); fix_transcript is voice_actions.correction_op.
Json voice_command_ops(std::string_view command, const Json& row, const std::vector<Json>& previous,
                       const std::vector<std::string>& exclude_ids = {});

// Said when the listener may not send a segment to the judge or has no device-local judge.
// OpenWhisper's NEEDS_CONSENT also offers "AI insights on for this meeting"; in FioriVoce AI
// insights never unlock a hosted Fast judgments route, so only the local model is named.
inline constexpr std::string_view VOICE_NEEDS_LOCAL_JUDGE =
    "Voice commands need Fast judgments on a model that runs on this computer.";

// Records source-backed commands from committed transcript rows. The gate is read by the worker
// before and after judging, and only loopback/local judges can receive text.
class VoiceCommandListener {
public:
    struct Deps {
        std::function<std::shared_ptr<llm::Judge>()> judge;
        std::function<bool()> allowed;
        std::function<void(const Json&)> feedback;
        std::function<void(const std::string&)> recap;
        // _on_voice_command_applied: the command and every op result of an applied card command
        // or spoken correction.
        std::function<void(const std::string&, const std::vector<meeting_state::OpResult>&)>
            applied;
    };

    VoiceCommandListener(std::shared_ptr<meeting_state::MeetingStateStore>,
                         std::vector<std::string> names, Deps);
    ~VoiceCommandListener();
    bool enabled() const { return wake_.has_value(); }
    // Queues committed commands (at most 16 waiting or running) and returns how many it queued.
    int observe(const std::vector<Json>& rows);
    void observe_preview(const Json& row);
    void shutdown();

private:
    struct Preview {
        Json last;
        std::string raw_text;
        std::optional<Json> pending;
        std::uint64_t generation = 0;
        std::uint64_t queue_order = 0;
        double committed_end = -1;
    };

    void run(Json row, const std::vector<Json>& previous, const std::vector<std::string>& excluded);
    void apply_correction(const Json& row, const std::vector<Json>& previous);
    void run_preview();
    void judge_preview(const std::string& channel, std::uint64_t generation, const Json& row,
                       const std::vector<Json>& previous);
    bool allowed() const;
    void feedback(std::string_view phase, std::string_view message, std::string_view command);

    std::shared_ptr<meeting_state::MeetingStateStore> store_;
    std::vector<std::string> names_;
    std::optional<py::Regex> wake_;
    Deps deps_;
    std::mutex mutex_;
    std::map<std::string, Preview> previews_;
    bool preview_running_ = false;
    std::uint64_t preview_sequence_ = 0;
    std::deque<Json> recent_;
    std::deque<std::string> handled_;
    // voice_commands.py _handled (maxlen 32): the prepared rows of committed commands, so a
    // rolling preview repeating one is not acknowledged again.
    std::deque<Json> handled_rows_;
    std::atomic<int> pending_{0};
    CancelSource closed_;
    ThreadPool worker_{"fv-voice-command", 1};
};

} // namespace fv::meeting_intel
