#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "base/cancel.h"
#include "base/json.h"
#include "dictation/job.h"
#include "dictation/pipeline.h"

// What the transcription runtime asks of AI transcript cleanup (services/transcript_cleanup.py
// TranscriptCleanup, as _maybe_cleanup_transcript drives it). The provider clients, the prompt
// composition and the profiles are fv_llm's and fv_text's (M4); until they exist the runtime
// uses the null cleaner, which is never available, so a transcript with cleanup on falls back to
// its raw text with "cleanup unavailable", as OpenWhisper does without a provider.
namespace fv::dictation {

// config.TRANSCRIPT_BATCH_CLEANUP_TIMEOUT_S: the per-request timeout of the one cleanup pass of a
// combined batch, whose text is far longer than a dictation. Seconds.
inline constexpr double TRANSCRIPT_BATCH_CLEANUP_TIMEOUT_S = 120.0;
// transcript_cleanup.CANCELED_REASON: last_error when the job's Cancel ended the cleanup.
inline constexpr std::string_view CLEANUP_CANCELED_REASON = "canceled";
// last_error of a cleaner that is not available.
inline constexpr std::string_view CLEANUP_UNAVAILABLE = "cleanup unavailable";

// One cleanup pass. Affinity: value type; the pointers outlive the call.
struct CleanupRequest {
    // What cleanup is given: PreparedText::cleanup_input.
    std::string text;
    // The prepared transcript `text` came from.
    const PreparedText* prepared = nullptr;
    // The delivered job; null for uploads, batches and retranscriptions.
    const DictationJob* job = nullptr;
    // The settings snapshot the pass reads (provider, model, rules, level).
    Json settings = Json::object();
    // The user's description of how a multi-file upload fits together, appended to the prompt
    // after the learned rules (batch.h compose_batch_cleanup_prompt); nullopt cleans the text
    // exactly as a single file's.
    std::optional<std::string> batch_context;
    // A per-request timeout overriding the client's (TRANSCRIPT_BATCH_CLEANUP_TIMEOUT_S for a
    // combined batch), seconds; nullopt keeps the client's own.
    std::optional<double> timeout_s;
    // The job's Cancel: the pass returns at once with CLEANUP_CANCELED_REASON once it is set.
    CancelToken cancel;
    // The system prompt run_cleanup composed (cleanup_prompt.h compose_cleanup_prompt), with the
    // batch context already in it; "" leaves the prompt to the cleaner.
    std::string system_prompt;
    // The name of the cleanup profile the pass formats with; "" for standard cleanup.
    std::string profile_name;
};

// What a pass gave back. Affinity: value type.
struct CleanupResult {
    // The cleaned text, or the request's text when the pass failed.
    std::string text;
    // last_error: nullopt only when the pass succeeded; CLEANUP_CANCELED_REASON after a Cancel.
    std::optional<std::string> error;
    // Who cleaned it, for CleanupInfo and History.
    std::string provider;
    std::string model;
    // pipeline.h cleanup_level for this pass ("custom", "light", "medium", "high", "profile").
    std::string level;
};

// The cleanup pass the runtime calls on its jobs strand, one call at a time.
// Affinity: the runtime's jobs strand.
class TranscriptCleaner {
public:
    virtual ~TranscriptCleaner() = default;
    // configure() with the provider, model and reasoning `settings` resolve, then is_available().
    virtual bool is_available(const Json& settings) = 0;
    // cleanup(): never throws; a failure gives the request's text with `error` set.
    virtual CleanupResult cleanup(const CleanupRequest& request) = 0;
};

// No provider: never available, and a pass returns its text with CLEANUP_UNAVAILABLE.
class NullTranscriptCleaner final : public TranscriptCleaner {
public:
    bool is_available(const Json& settings) override;
    CleanupResult cleanup(const CleanupRequest& request) override;
};

// The process's null cleaner. Thread-safe (it keeps no state).
TranscriptCleaner& null_transcript_cleaner();

// What a status line says after a transcript whose cleanup fell back to raw text
// (_batch_status's tail): " without AI cleanup" when a Cancel cut it short, " — AI cleanup
// failed ({error}); showing raw text" for any other failure, "" when it did not fail.
std::string cleanup_status_suffix(const std::optional<std::string>& cleanup_error);

} // namespace fv::dictation
