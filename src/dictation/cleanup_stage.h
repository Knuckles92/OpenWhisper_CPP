#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "appcore/overlay_state.h"
#include "base/cancel.h"
#include "base/clock.h"
#include "base/json.h"
#include "dictation/cleaner_port.h"
#include "dictation/cleanup_info.h"
#include "dictation/job.h"
#include "dictation/pipeline.h"
#include "text/cleanup_profiles.h"

namespace fv::llm {
class TranscriptCleanup;
}

// The cleanup stage of a transcript (services/runtime/transcription.py _maybe_cleanup_transcript
// and _finish_without_cleanup), for live dictations, uploads and the files of a batch: the
// dictionary and snippets always, AI cleanup when it is on or a cleanup profile asks for it.
namespace fv::dictation {

// What one pass works from. Affinity: value type; `job` outlives the call.
struct CleanupStageInput {
    // The delivered job; null for uploads, batches and retranscriptions.
    const DictationJob* job = nullptr;
    // The settings the pass reads: a profile recording's snapshot from its start, else fresh.
    Json settings = Json::object();
    // The cleanup profile to format with; nullopt for standard cleanup.
    std::optional<text::CleanupProfile> profile;
    // A multi-file upload's description, appended to the prompt after the learned rules.
    std::optional<std::string> batch_context;
    // A per-request timeout for text far longer than a dictation, seconds.
    std::optional<double> timeout_s;
    // The job's Cancel: canceled text never reaches the provider.
    CancelToken cancel;
    // False for words that skip AI cleanup whatever the settings say (an action shaped "raw"):
    // the dictionary and snippets only.
    bool allow_ai = true;
};

// Where the stage reaches out. Every pointer outlives the call.
struct CleanupStageServices {
    // dictionary_preparer() when null.
    const TextPreparer* preparer = nullptr;
    // null_transcript_cleaner() when null.
    TranscriptCleaner* cleaner = nullptr;
    // system_clock() when null; its steady time measures the provider's call.
    const Clock* clock = nullptr;
    // The overlay state and status a pass that reaches the provider shows first (Cleaning with
    // "Cleaning up..." or "Formatting · {profile}...").
    std::function<void(appcore::OverlayState, const std::string&)> announce;
};

// What the pass gave back, and what the runtime keeps for delivery and History.
// Affinity: value type.
struct CleanupOutcome {
    std::string text;
    // The text before cleanup, only when the delivered text differs from it.
    std::optional<std::string> raw_text;
    // Only for a pass that ran (its text changed or it reported no error).
    std::optional<CleanupInfo> info;
    // _last_cleanup_failure: why the pass fell back to the uncleaned text; nullopt when it did
    // not ("cleanup unavailable", "snippet placeholder lost", "canceled", the provider's reason).
    std::optional<std::string> failure;
    // The rich-text twin of `text` (formatted snippets), or "".
    std::string delivery_html;
    // The transcript before snippets expanded: what Stats count as words said.
    std::string spoken_text;
    // pipeline.h stats_counts.
    Json stats_counts = Json::object();

    bool operator==(const CleanupOutcome&) const = default;
};

// _maybe_cleanup_transcript on `raw`. Blocking (the provider's call): a worker, never the UI
// thread. Never throws for a feature that fails; a cleaner that throws propagates.
CleanupOutcome run_cleanup(std::string_view raw, const CleanupStageInput& input,
                           const CleanupStageServices& services);

// AI cleanup through the user's text-model endpoint (services/transcript_cleanup.py
// TranscriptCleanup as the runtime drives it): each availability check applies the provider,
// model and thinking level the settings resolve now, so a change in Settings takes effect
// without a restart (a provider switch rebuilds the client).
// Affinity: the runtime's jobs strand, one call at a time; blocking (keys and the provider).
class LlmTranscriptCleaner final : public TranscriptCleaner {
public:
    explicit LlmTranscriptCleaner(std::shared_ptr<llm::TranscriptCleanup> cleanup);

    bool is_available(const Json& settings) override;
    // Sends request.system_prompt (the client's default prompt when it is "") with the
    // request's timeout and Cancel.
    CleanupResult cleanup(const CleanupRequest& request) override;

    llm::TranscriptCleanup& client() const { return *cleanup_; }

private:
    std::shared_ptr<llm::TranscriptCleanup> cleanup_;
};

} // namespace fv::dictation
