#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/json.h"
#include "diarize/clustering.h"
#include "meeting/finalization.h"
#include "meeting/intelligence.h"
#include "meeting/model_lease.h"
#include "meeting/recovery.h"
#include "meeting/rerun_options.h"
#include "meeting_capture/offline.h"
#include "meeting_state/repository.h"
#include "meeting_state/store.h"
#include "meeting_state/types.h"

// Headless re-run of the post-meeting pipeline (meeting/refinalize.py). After End the audio, the
// transcript and the dashboard state are durable; this retries any later step (redecode, speaker
// labels, polish, consolidation, finalize) without a live meeting engine. Human pins, edits and
// confirmed cards stay protected because every write goes through the meeting's state store.
// Polish and consolidation use IntelligenceProvider. Uploading saved audio to identify speakers
// requires a fresh, meeting-specific desktop confirmation before running the cloud pass.
// Affinity: functions; they block on the repository, the speech service and the provider, never
// on the GUI thread.
namespace fv::meeting {

// The steps a re-run may start from besides "finalize".
inline constexpr std::array<std::string_view, 4> OPTIONAL_RERUN_STEPS = {"redecode", "speaker_id",
                                                                         "polish", "consolidation"};

// A general retry has no authority to upload saved audio. The dedicated speaker action collects
// consent.
inline constexpr std::string_view kSpeakerPassUnavailable =
    "Speaker identification is unavailable.";

// ---- one run per meeting ----

// Another re-run of the same meeting is in progress: "post-meeting steps are already running for
// this meeting".
class FinalizationBusyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Keeps one re-run of a meeting exclusive (desktop retry, dashboard re-run and recovery share
// one process-wide set) through the caller's cleanup and publication: a caller may lend its claim
// to a pipeline (RerunRequest::claim) and release it only after publishing the outcome. A claim
// is not reusable once released and must not be lent to two pipelines at once. Destroying it
// releases it.
// Affinity: thread-safe.
class FinalizationClaim {
public:
    // Throws FinalizationBusyError while the meeting is claimed.
    explicit FinalizationClaim(std::string meeting_id);
    ~FinalizationClaim();
    FinalizationClaim(const FinalizationClaim&) = delete;
    FinalizationClaim& operator=(const FinalizationClaim&) = delete;

    const std::string& meeting_id() const { return meeting_id_; }
    bool is_active() const;
    // Whether no later claim of the meeting superseded this one's queued events.
    bool is_current() const;
    // Idempotent.
    void release();
    // py::ValueError "inactive or mismatched finalization claim" unless this claim is active and
    // for `meeting_id`.
    void validate(std::string_view meeting_id) const;

private:
    std::string meeting_id_;
    // Identity in the registry of latest claims, which holds it weakly (Python's
    // WeakValueDictionary): a destroyed claim is no one's latest.
    std::shared_ptr<const int> token_;
    bool active_ = false; // guarded by the registry's lock
};

// Whether post-meeting steps of the meeting are re-running now.
bool is_running(std::string_view meeting_id);

// _one_run_per_meeting: runs `fn` under the borrowed `claim` (validated first, kept afterwards)
// or under a claim of its own, released when fn returns or throws. Throws FinalizationBusyError
// when the meeting is claimed elsewhere.
template <class F>
auto run_claimed(const std::string& meeting_id, FinalizationClaim* claim, F&& fn) {
    if (claim != nullptr) {
        claim->validate(meeting_id);
        return std::forward<F>(fn)();
    }
    FinalizationClaim own(meeting_id);
    own.validate(meeting_id);
    return std::forward<F>(fn)();
}

// ---- seams ----

// The injected offline decoder (Python's transcribe_fn): the session audio under `spool_dir` to
// segments, with the meeting's chunk rows for channels whose session WAV is missing. Live End
// passes its own engine's transcribe_offline_session; tests pass fakes. Throws to fail the pass.
using RedecodeFn = std::function<std::vector<meeting_state::TranscriptSegment>(
    const std::filesystem::path& spool_dir, const std::vector<Json>& chunks,
    const OfflineProgress& progress)>;

// A diarizer for a stored meeting whose new speakers `host` creates (the host outlives it), or
// null when diarization is unavailable.
using DiarizerFactory = std::function<std::unique_ptr<diarize::Diarizer>(
    diarize::DiarizerHost& host, const std::string& meeting_id)>;

// The factory of OpenWhisper's _new_offline_diarizer: create_diarizer over the speaker model and
// the ONNX Runtime library, null without either.
DiarizerFactory speaker_model_diarizer(std::optional<std::filesystem::path> model,
                                       std::optional<std::filesystem::path> onnxruntime);

// The deterministic repairs after a redecode (meeting_state::repair_meeting_state; tests count
// the calls).
using RepairFn =
    std::function<int(meeting_state::MeetingStateStore& store, const std::vector<Json>& rows)>;

// ---- redecode ----

struct RedecodeRequest {
    // The meeting's store; null opens a write-through one over the stored row.
    std::shared_ptr<meeting_state::MeetingStateStore> store;
    // What a store opened over the row reads (the report views of a fresh state).
    Json settings = Json::object();
    // The model the engine loads when `transcribe` is not set.
    std::string asr_model_name = "auto";
    // An ISO-639-1 pin; nullopt (or "auto", "", "None") detects.
    std::optional<std::string> language;
    // Replaces the engine (live End's own decoder, tests): no model is loaded and no lease taken.
    RedecodeFn transcribe;
    // Builds the engine that decodes on the speech service's Background lane.
    AsrFactory asr;
    OfflineProgress progress;
    // Taken around the engine's lifetime when it is built here.
    ModelLease* model_lease = nullptr;
    // Keeps the draft when the new pass has fewer than 80% of its words.
    bool redecode_coverage_guard = false;
    // "off" leaves system audio channel-labelled, as a live meeting with speaker identification
    // off does; any other backend re-diarizes.
    std::string speaker_id_backend = "local";
    // Labels system audio: nullopt builds a fresh one from `diarizer_factory` when the pass has
    // system audio; a null pointer (live End without a diarizer) labels nothing.
    std::optional<diarize::Diarizer*> diarizer;
    DiarizerFactory diarizer_factory;
    RepairFn repair;
    const Clock* clock = nullptr; // nullptr: system_clock()
    CancelToken cancel;
};

// Re-decodes the session audio and replaces the stored draft transcript, in this order:
// transcribe, refuse an empty pass ("Re-decoding produced no transcript."), the coverage guard,
// the diarizer choice, speaker assignment (Me on the mic, the diarizer on system audio), then the
// replace with derived state rebuilt (chunks done, store reloaded, unevidenced proposed cards
// stripped, repairs). The engine and the lease are released before anything is stored. Returns
// {ok, error}, plus "rows" and "removed_ids" on success and "kept_draft" when the guard refused;
// failures are reported, not thrown, except py::ValueError "unknown meeting".
Json rerun_redecode(const std::shared_ptr<meeting_state::MeetingRepository>& repository,
                    const std::string& meeting_id, const RedecodeRequest& request);

// ---- the retry pipeline ----

struct SpeakerUploadOffer;
struct CloudSpeakerOptions;

struct RerunRequest {
    std::shared_ptr<meeting_state::MeetingRepository> repository;
    RerunOptions options;
    // The step to start from, or "failed" for the earliest failed or skipped step (else
    // consolidation, else the first).
    std::string from_step = "failed";
    // The meeting's live store; null opens a headless one over the stored row.
    std::shared_ptr<meeting_state::MeetingStateStore> store;
    Json settings = Json::object();
    // Polish and consolidation; null reads as null_intelligence().
    std::shared_ptr<IntelligenceProvider> intelligence = null_intelligence();
    // The redecode's decoder: `transcribe` when set, else an engine from `asr`.
    RedecodeFn transcribe;
    AsrFactory asr;
    // Builds the redecode's diarizer: the app passes speaker_model_diarizer (OpenWhisper always
    // builds one from the speaker model); empty leaves system audio channel-labelled.
    DiarizerFactory diarizer;
    ModelLease* model_lease = nullptr;
    // Every persisted finalization snapshot (FinalizationState::to_json()).
    std::function<void(const Json& finalization)> progress;
    // A claim the caller holds and releases after publishing; null claims the meeting here.
    FinalizationClaim* claim = nullptr;
    RepairFn repair;
    const Clock* clock = nullptr; // nullptr: system_clock()
    CancelToken cancel;
    std::optional<meeting_agent::MeetingAgentRoute> approved_route;
    std::function<bool()> saved_destination_allowed;
};

// Retries the post-meeting steps from request.from_step through its dependents, persisting each
// step's progress in the meeting's finalization. Returns {ok, state, applied, error,
// finalization}. Throws py::ValueError "unknown meeting" and FinalizationBusyError.
Json rerun_finalization(const std::string& meeting_id, const RerunRequest& request);

// The speaker pass of every caller (live End, the dashboard's re-run, the retry): nothing is
// uploaded unless `gate` is ok. Returns {ok, skipped, applied, created, windows, error}; a refused
// gate is skipped with its reason. Never throws.
Json run_speaker_pass(meeting_state::MeetingRepository& repository, const std::string& meeting_id,
                      meeting_state::MeetingStateStore& store, const std::string& spool_dir,
                      const SpeakerPassGate& gate,
                      const std::function<void(const std::string&, int, int)>& progress = {},
                      const std::function<void()>& on_start = {});

// Relabels a finished meeting's speakers from its system-audio recording: run_speaker_pass's
// result plus the post-pass "state". The gate is built from the request's options (consent and
// backend; no key is held in this build) unless one is given. Throws py::ValueError "unknown
// meeting" and FinalizationBusyError.
Json rerun_speakers(const std::string& meeting_id, const RerunRequest& request);
Json rerun_speakers(const std::string& meeting_id, const RerunRequest& request,
                    const SpeakerPassGate& gate);

// The explicit desktop-only path, scoped to the exact meeting and audio described before consent.
Json rerun_approved_speakers(
    const SpeakerUploadOffer& offer, const std::filesystem::path& recordings_root,
    const RerunRequest& request, const CloudSpeakerOptions& options,
    const std::function<void(const std::string&, int, int)>& progress = {});

// ---- the pieces ----

// Reloads the store's document after an out-of-band SQLite write (a transcript replace rewrites
// evidence ids in state_json directly). A missing, corrupt or unloadable snapshot is logged and
// leaves the store as it is.
void reload_store(meeting_state::MeetingStateStore& store,
                  meeting_state::MeetingRepository& repository, const std::string& meeting_id,
                  const Clock& clock = system_clock());

// The deterministic repair after a consolidation pass that ran, whatever its outcome
// (scheduler.py _repair_after_consolidation, reinsight.py rerun_insights): a model often ships key
// points and a summary but leaves the timeline empty, so evidenced key points are promoted (or
// the transcript sampled) and the durable record always has story beats. `outcome` is the
// provider's; a pass that never ran ("unavailable") repairs nothing. Returns the ops applied; a
// repair failure is logged and never replaces the pass's outcome.
int repair_after_consolidation(meeting_state::MeetingStateStore& store,
                               const std::vector<Json>& segments, const Json& outcome);

// Drops ghost-anchored proposed cards after a transcript replace: proposed, unprotected items
// without evidence leave every card but user_notes and live_notes (the notes page gives the final
// consolidation its context). Applied as system/finalization; a rejected batch is logged.
void strip_unevidenced_proposed(meeting_state::MeetingStateStore& store);

// Labels re-decoded segments in place: Me (`me_id`, source "channel") on the mic, `diarizer`
// over the whole loopback session recording on system audio (`chunks` stitch a missing session
// WAV). A null diarizer leaves system audio as it is. Never throws.
void assign_session_speakers(std::vector<meeting_state::TranscriptSegment>& segments,
                             const std::optional<std::string>& me_id, diarize::Diarizer* diarizer,
                             const std::filesystem::path& spool_dir,
                             const std::vector<Json>& chunks);

// _steps_from_state: the stored steps (`existing`, a list of step objects), or the default plan
// when there are none (redecode with audio, speaker_id when asked for, polish and consolidation
// with AI insights, finalize), with a requested step that is missing inserted in kStepOrder's
// position.
Json steps_from_state(const Json& existing, std::string_view from_step, bool cloud_enabled,
                      bool have_audio);

// _run_ids: the step ids to run, from `from_step` ("failed": the first failed or skipped step,
// else consolidation, else the first) to the end; a known step that is not in the list runs
// alone.
std::vector<std::string> run_ids(const Json& steps, std::string_view from_step);

// _overall_from_steps: ("failed", failed_steps_message), ("disabled", "AI insights are off for
// this meeting.") without AI insights or their steps, else ("completed", "Final insights are
// ready.").
std::pair<std::string, std::string> overall_from_steps(const Json& steps, bool cloud_enabled);

// _coverage_refusal: {ok: false, error: sparse_redecode_detail, kept_draft: true} when the guard
// is on and the new pass has fewer than 80% of the draft's words (an unreadable draft skips the
// guard), else nullopt.
std::optional<Json> coverage_refusal(meeting_state::MeetingRepository& repository,
                                     const std::string& meeting_id,
                                     const std::vector<meeting_state::TranscriptSegment>& decoded,
                                     bool coverage_guard);

} // namespace fv::meeting
