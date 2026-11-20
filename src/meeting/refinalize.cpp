#include "meeting/refinalize.h"

#include "meeting/cloud_pass.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <map>
#include <set>
#include <utility>

#include "base/log.h"
#include "base/utf8.h"
#include "diarize/assign.h"
#include "meeting/detail/py_value.h"
#include "meeting_capture/session_audio.h"
#include "meeting_state/repair.h"
#include "meeting_state/schema.h"
#include "meeting_state/stored.h"
#include "meeting_state/time_utils.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "speech/api_models.h"

namespace fv::meeting {

namespace {

constexpr log::Logger logger("meeting.refinalize");

using meeting_state::MeetingRepository;
using meeting_state::MeetingStateStore;
using meeting_state::TranscriptSegment;

const Clock& clock_or_system(const Clock* clock) {
    return clock != nullptr ? *clock : system_clock();
}

// ---- claims ----

struct ClaimRegistry {
    std::mutex mutex;
    std::set<std::string, std::less<>> running;
    std::map<std::string, std::weak_ptr<const int>, std::less<>> latest;
};

ClaimRegistry& claims() {
    static ClaimRegistry registry;
    return registry;
}

// ---- steps ----

const Json& step_id_of(const Json& step) {
    return detail::get(step, "id");
}

bool step_id_is(const Json& step, std::string_view id) {
    const Json& value = step_id_of(step);
    return value.is_string() && value.get_ref<const std::string&>() == id;
}

std::size_t order_of(const Json& step) {
    const Json& id = step_id_of(step);
    const std::string text = id.is_string() ? id.get<std::string>() : detail::py_str(id);
    for (std::size_t i = 0; i < kStepOrder.size(); ++i) {
        if (kStepOrder[i] == text) {
            return i;
        }
    }
    return 99;
}

Json ensure_step(const Json& steps, std::string_view step_id) {
    for (const Json& step : steps) {
        if (step_id_is(step, step_id)) {
            return steps;
        }
    }
    Json merged = steps;
    merged.push_back(make_step(step_id));
    std::vector<Json> sorted(merged.begin(), merged.end());
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Json& a, const Json& b) { return order_of(a) < order_of(b); });
    return Json(sorted);
}

// _set_step: the step's status, and its detail when one is given; a step that is missing is
// appended with the detail or the step's default. Returns its 1-based position.
int set_step(Json& steps, std::string_view step_id, std::string_view status,
             std::string_view detail_text = "") {
    int index = 0;
    for (Json& step : steps) {
        ++index;
        if (step_id_is(step, step_id)) {
            step["status"] = std::string(status);
            if (!detail_text.empty()) {
                step["detail"] = std::string(detail_text);
            }
            return index;
        }
    }
    Json step = make_step(step_id, status);
    step["detail"] = !detail_text.empty() ? std::string(detail_text)
                                          : std::string(step_detail(step_id).value_or(""));
    steps.push_back(std::move(step));
    return static_cast<int>(steps.size());
}

std::int64_t word_count(const std::vector<TranscriptSegment>& segments) {
    std::int64_t total = 0;
    for (const TranscriptSegment& segment : segments) {
        total += static_cast<std::int64_t>(py::split(segment.text).size());
    }
    return total;
}

std::int64_t word_count(const std::vector<Json>& rows) {
    std::int64_t total = 0;
    for (const Json& row : rows) {
        total += static_cast<std::int64_t>(
            py::split(detail::str_or_empty(detail::get(row, "text"))).size());
    }
    return total;
}

std::string format_template(std::string_view text, int current, int total) {
    std::string out(text);
    const auto replace = [&out](std::string_view key, int value) {
        const std::size_t at = out.find(key);
        if (at != std::string::npos) {
            out.replace(at, key.size(), std::to_string(value));
        }
    };
    replace("{curr}", current);
    replace("{total}", total);
    return out;
}

std::shared_ptr<MeetingStateStore>
open_meeting_store(const std::shared_ptr<MeetingRepository>& repository,
                   const std::string& meeting_id, const Json& meeting, const Json& settings,
                   const Clock& clock) {
    return meeting_state::open_store(repository, meeting_id, meeting, false, settings, clock);
}

std::optional<std::string> me_participant_id(MeetingStateStore& store) {
    try {
        return store.with_state([](const meeting_state::MeetingState& state) {
            for (const auto& [id, participant] : state.participants) {
                if (participant.kind == "me") {
                    return std::optional<std::string>(id);
                }
            }
            return std::optional<std::string>();
        });
    } catch (const std::exception&) {
        logger.warning("Could not read meeting participants for speaker assignment");
        return std::nullopt;
    }
}

// The diarizer's view of a stored meeting (Python hands it the store): new speakers are
// provisional "Speaker N" participants created as system/diarizer. An offline diarizer has no
// relabel callback, so re-cluster relabels of rows not yet stored are dropped, as in Python.
class StoreDiarizerHost final : public diarize::DiarizerHost {
public:
    explicit StoreDiarizerHost(MeetingStateStore& store) : store_(store) {}

    std::expected<std::string, std::string> create_participant(std::string display_name) override {
        Json op = Json::object();
        op["op"] = "upsert_participant";
        op["display_name"] = std::move(display_name);
        op["kind"] = "others_cluster";
        op["is_provisional"] = true;
        const std::vector<meeting_state::OpResult> results =
            store_.apply("system", std::string("diarizer"), Json::array({op}));
        if (results.empty()) {
            return std::unexpected("no result");
        }
        if (!results.front().ok) {
            return std::unexpected(results.front().reason.value_or("None"));
        }
        const Json& participant = detail::get(results.front().effect, "participant");
        const std::string id = detail::str_or_empty(detail::get(participant, "id"));
        if (!id.empty()) {
            return id;
        }
        return results.front().target_id.value_or("");
    }

    void relabel(const diarize::RelabelOps&) override {}

    std::map<std::string, std::string> participant_names() override {
        return store_.with_state([](const meeting_state::MeetingState& state) {
            std::map<std::string, std::string> names;
            for (const auto& [id, participant] : state.participants) {
                names.emplace(id, participant.display_name);
            }
            return names;
        });
    }

private:
    MeetingStateStore& store_;
};

std::vector<Json> redecode_chunks(MeetingRepository& repository, const std::string& meeting_id) {
    try {
        return repository.get_audio_chunks(meeting_id);
    } catch (const std::exception& error) {
        logger.error_with(error, "Could not load audio chunks for redecode retry");
        return {};
    }
}

// "auto" for no pin. rerun_options reads a remote route's null language as "None", which Python
// passes on as no pin.
std::string decode_language(const std::optional<std::string>& language) {
    if (!language) {
        return "auto";
    }
    const std::string text = py::strip(*language);
    if (text.empty() || text == "None") {
        return "auto";
    }
    return *language;
}

// The engine and the model lease one redecode holds until it is destroyed (_RedecodeBackend).
class RedecodeEngine {
public:
    explicit RedecodeEngine(ModelLease* lease) : lease_(lease) {}
    RedecodeEngine(const RedecodeEngine&) = delete;
    RedecodeEngine& operator=(const RedecodeEngine&) = delete;

    ~RedecodeEngine() {
        // Freed here rather than at each exit: a retained model keeps a second copy of the weights
        // resident beside the dictation engine.
        if (engine_) {
            try {
                engine_->stop();
            } catch (const std::exception& error) {
                logger.error_with(error, "Error releasing the redecode speech model");
            }
            engine_.reset();
        }
        if (leased_) {
            release_model_lease(lease_);
        }
    }

    // Builds and loads the engine; the error to report when it is not available.
    std::optional<std::string> load(const std::string& meeting_id, const RedecodeRequest& request,
                                    const std::string& language) {
        leased_ = acquire_model_lease(lease_);
        const std::string model =
            request.asr_model_name.empty() ? std::string("auto") : request.asr_model_name;
        if (request.asr) {
            engine_ = request.asr(meeting_id, model, language);
        }
        if (engine_ && engine_->load_backend(request.cancel)) {
            return std::nullopt;
        }
        std::string error = engine_ ? engine_->last_error() : std::string();
        if (error.empty()) {
            error = "The speech model failed to load.";
        }
        return error;
    }

    MeetingAsrEngine& engine() { return *engine_; }

private:
    ModelLease* lease_;
    std::unique_ptr<MeetingAsrEngine> engine_;
    bool leased_ = false;
};

Json failure(std::string_view error) {
    Json result = Json::object();
    result["ok"] = false;
    result["error"] = std::string(error);
    return result;
}

Json store_redecoded(MeetingRepository& repository, MeetingStateStore& store,
                     const std::string& meeting_id, const std::vector<TranscriptSegment>& decoded,
                     const RepairFn& repair, const Clock& clock) {
    meeting_state::TranscriptReplacement replacement;
    try {
        replacement = repository.replace_final_transcript(meeting_id, decoded);
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Final transcript replace failed for {}", meeting_id));
        return failure(error.what());
    }
    try {
        repository.mark_chunks_done(meeting_id);
    } catch (const std::exception& error) {
        logger.error_with(error, "Could not mark chunks done after a redecode");
    }
    reload_store(store, repository, meeting_id, clock);
    strip_unevidenced_proposed(store);
    // Repair ran against the draft at End; timeline coverage and summary fallbacks are rebuilt
    // from the final transcript.
    try {
        if (repair) {
            repair(store, replacement.rows);
        } else {
            meeting_state::repair_meeting_state(store, replacement.rows);
        }
    } catch (const std::exception& error) {
        logger.error_with(error, "State repair after a redecode failed");
    }
    Json result = Json::object();
    result["ok"] = true;
    result["error"] = nullptr;
    result["rows"] = Json(replacement.rows);
    result["removed_ids"] = Json(replacement.deleted_ids);
    return result;
}

// The outcome of a provider pass as Python's {ok, applied, error}.
Json provider_result(const Json& outcome) {
    const std::string status = detail::str_or_empty(detail::get(outcome, "status"));
    Json result = Json::object();
    result["ok"] = status == "completed";
    result["applied"] = detail::truthy(detail::get(outcome, "applied"))
                            ? detail::int_or_zero(detail::get(outcome, "applied"))
                            : 0;
    const std::string message = detail::str_or_empty(detail::get(outcome, "message"));
    result["error"] = status == "completed" || message.empty() ? Json() : Json(message);
    return result;
}

Json persist_finalization(MeetingStateStore& store, std::string_view status,
                          std::string_view message, const Json& steps, std::string_view stage,
                          int current_step, std::string_view step_details,
                          const Json& summary_stats,
                          const std::function<void(const Json&)>& progress) {
    meeting_state::FinalizationState payload;
    payload.status = std::string(status);
    payload.message = std::string(message);
    payload.stage = std::string(stage);
    payload.current_step = current_step;
    payload.total_steps = static_cast<std::int64_t>(steps.size());
    payload.step_details = std::string(step_details);
    payload.steps = steps;
    payload.summary_stats = summary_stats.is_object() ? summary_stats : Json::object();
    payload.card_deferred = store.with_state(
        [](const meeting_state::MeetingState& state) { return state.finalization.card_deferred; });
    const std::string detail_text(step_details.empty() ? message : step_details);
    if (status == "failed") {
        logger.warning("Meeting re-finalization meeting_id={} stage={} status={} detail={}",
                       store.meeting_id(), stage, status, detail_text);
    } else {
        logger.info("Meeting re-finalization meeting_id={} stage={} status={} detail={}",
                    store.meeting_id(), stage, status, detail_text);
    }
    Json data = payload.to_json();
    store.update_runtime_fields(Json{{"finalization", data}});
    if (progress) {
        try {
            progress(data);
        } catch (const std::exception& error) {
            logger.error_with(error, "Finalization progress callback failed");
        }
    }
    return data;
}

Json collect_summary_stats(MeetingStateStore& store, MeetingRepository& repository,
                           const std::string& meeting_id, const Json& meeting, const Clock& clock) {
    const double duration_s =
        meeting_state::meeting_duration_s(meeting, false, clock).value_or(0.0);
    Json state = Json::object();
    try {
        state = store.snapshot();
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Could not collect card stats for {}", meeting_id));
    }
    std::vector<Json> segments;
    try {
        segments = repository.get_segments(meeting_id, -1.0, std::nullopt);
    } catch (const std::exception& error) {
        logger.error_with(error,
                          std::format("Could not collect transcript stats for {}", meeting_id));
    }
    // Python hands summary_stats list(state.questions), the question ids: every question counts.
    return summary_stats(detail::get(state, "cards"), detail::get(state, "questions"), segments,
                         duration_s);
}

// What the steps of one rerun_finalization share: progress and outcome (_RetryRun).
class RetryRun {
public:
    RetryRun(MeetingRepository& repository, std::string meeting_id, Json meeting,
             MeetingStateStore& store, Json steps, bool cloud_enabled,
             std::function<void(const Json&)> progress, const Clock& clock)
        : repository(repository), meeting_id(std::move(meeting_id)), meeting(std::move(meeting)),
          store(store), steps(std::move(steps)), cloud_enabled(cloud_enabled),
          progress_cb(std::move(progress)), clock(clock) {}

    void running(std::string_view step_id, std::string_view detail_text, std::string_view message) {
        const int current = set_step(steps, step_id, "running", detail_text);
        persist_finalization(store, "running", message, steps, step_id, current, detail_text,
                             Json::object(), progress_cb);
    }

    // A (detail, current, total) callback; `text` takes {curr} and {total}.
    std::function<void(const std::string&, int, int)> progress(std::string step_id,
                                                               std::string text) {
        return [this, step_id = std::move(step_id),
                text = std::move(text)](const std::string& detail_text, int current, int total) {
            running(step_id, detail_text, format_template(text, current, total));
        };
    }

    void record(std::string_view step_id, const Json& result, std::string_view done_detail,
                std::string_view default_error, bool count_applied = true) {
        if (detail::truthy(detail::get(result, "ok"))) {
            if (count_applied) {
                applied += detail::int_or_zero(detail::get(result, "applied"));
            }
            set_step(steps, step_id, "completed", done_detail);
            return;
        }
        const Json& error = detail::get(result, "error");
        last_error = detail::truthy(error) ? detail::py_str(error) : std::string(default_error);
        set_step(steps, step_id, "failed", *last_error);
    }

    Json finish(std::string_view status, std::string_view message, const Json& stats) {
        const Json finalization = persist_finalization(
            store, status, message, steps, status == "completed" ? "complete" : status,
            static_cast<int>(steps.size()), message, stats, progress_cb);
        Json result = Json::object();
        result["ok"] = status != "failed";
        result["state"] = store.snapshot();
        result["applied"] = applied;
        result["error"] = status == "failed" && last_error ? Json(*last_error) : Json();
        result["finalization"] = finalization;
        return result;
    }

    MeetingRepository& repository;
    const std::string meeting_id;
    const Json meeting;
    MeetingStateStore& store;
    Json steps;
    const bool cloud_enabled;
    const std::function<void(const Json&)> progress_cb;
    const Clock& clock;
    std::int64_t applied = 0;
    std::optional<std::string> last_error;
};

RerunContext rerun_context(RetryRun& run, const RerunRequest& request,
                           const std::shared_ptr<MeetingStateStore>& store,
                           std::function<void(const std::string&, int, int)> progress) {
    RerunContext context;
    context.meeting_id = run.meeting_id;
    context.meeting = run.meeting;
    context.store = store;
    context.repository = request.repository;
    context.state = run.store.snapshot();
    context.segments = run.repository.get_segments(run.meeting_id, -1.0, std::nullopt);
    context.options = request.options;
    context.progress = std::move(progress);
    context.approved_route = request.approved_route;
    context.saved_destination_allowed = request.saved_destination_allowed;
    return context;
}

void retry_redecode(RetryRun& run, const RerunRequest& request,
                    const std::shared_ptr<MeetingStateStore>& store) {
    run.running("redecode", "Starting high-accuracy session audio re-decoding...",
                "Re-transcribing meeting…");
    RedecodeRequest redecode;
    redecode.store = store;
    redecode.settings = request.settings;
    redecode.asr_model_name = request.options.asr_model_name;
    redecode.language = request.options.language;
    redecode.transcribe = request.transcribe;
    redecode.asr = request.asr;
    redecode.progress =
        run.progress("redecode", "Re-transcribing meeting (window {curr}/{total})…");
    redecode.model_lease = request.model_lease;
    redecode.redecode_coverage_guard = request.options.redecode_coverage_guard;
    redecode.speaker_id_backend = request.options.speaker_id_backend;
    redecode.diarizer_factory = request.diarizer;
    redecode.repair = request.repair;
    redecode.clock = &run.clock;
    redecode.cancel = request.cancel;
    const Json result = rerun_redecode(request.repository, run.meeting_id, redecode);
    // The redecode applies no state ops of its own.
    run.record("redecode", result, "High-accuracy re-decoding complete", "Re-decoding failed",
               false);
}

void retry_speaker_id(RetryRun& run, const SpeakerPassGate& gate) {
    const Json result = run_speaker_pass(
        run.repository, run.meeting_id, run.store,
        detail::str_or_empty(detail::get(run.meeting, "spool_dir")), gate,
        run.progress("speaker_id", "Identifying speakers (window {curr}/{total})…"), [&run] {
            run.running("speaker_id", "Uploading system audio for speaker labels…",
                        "Identifying speakers…");
        });
    const auto [status, detail_text] = speaker_step_outcome(result);
    // Counted even for a skipped pass, whose applied is 0.
    run.applied += detail::int_or_zero(detail::get(result, "applied"));
    if (status == "failed") {
        run.last_error = detail_text;
    }
    set_step(run.steps, "speaker_id", status, detail_text);
}

// The request's provider; a null one is the build without meeting intelligence.
IntelligenceProvider& intelligence_of(const RerunRequest& request) {
    return request.intelligence ? *request.intelligence : *null_intelligence();
}

void retry_polish(RetryRun& run, const RerunRequest& request,
                  const std::shared_ptr<MeetingStateStore>& store) {
    run.running("polish", "Starting AI transcript cleanup and formatting...",
                "Cleaning transcript…");
    // rerun_polish reads the segments before its try: a failed read ends the retry.
    RerunContext context = rerun_context(
        run, request, store, run.progress("polish", "Cleaning transcript (block {curr}/{total})…"));
    Json result;
    try {
        if (context.segments.empty()) {
            result = Json{{"ok", true}, {"applied", 0}, {"error", nullptr}};
        } else {
            result = provider_result(intelligence_of(request).polish(context, request.cancel));
        }
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Polish retry failed for meeting {}", run.meeting_id));
        result = Json{{"ok", false}, {"applied", 0}, {"error", error.what()}};
    }
    run.record("polish", result, "Transcript cleanup finished", "Transcript cleanup failed");
}

void retry_consolidation(RetryRun& run, const RerunRequest& request,
                         const std::shared_ptr<MeetingStateStore>& store) {
    run.running("consolidation",
                "Synthesizing executive summary, key points, decisions, and action items...",
                "Preparing final report…");
    Json result;
    try {
        const RerunContext context = rerun_context(run, request, store, {});
        if (context.segments.empty()) {
            // reinsight.py rerun_insights refuses a meeting with nothing to read before any
            // agent is built.
            result = Json{{"ok", false}, {"applied", 0}, {"error", "meeting has no transcript"}};
        } else {
            const Json outcome = intelligence_of(request).consolidate(context, request.cancel);
            result = provider_result(outcome);
            // The repair's ops count with the agent's.
            result["applied"] = detail::int_or_zero(detail::get(result, "applied")) +
                                repair_after_consolidation(run.store, context.segments, outcome);
        }
    } catch (const py::ValueError& error) {
        // _retry_consolidation catches only ValueError (rerun_insights' refusals); anything
        // else ends the retry.
        result = Json{{"ok", false}, {"applied", 0}, {"error", error.what()}};
    }
    run.record("consolidation", result, "Summary & action items ready", "consolidation failed");
}

Json retry_finalize(RetryRun& run) {
    run.running("finalize", "Saving final transcript and meeting state...",
                "Finalizing meeting state…");
    Json stats = Json::object();
    try {
        stats = collect_summary_stats(run.store, run.repository, run.meeting_id, run.meeting,
                                      run.clock);
        set_step(run.steps, "finalize", "completed", saved_state_detail(stats));
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Finalize retry failed for {}", run.meeting_id));
        run.last_error = error.what();
        set_step(run.steps, "finalize", "failed", *run.last_error);
        stats = Json::object();
    }
    auto [status, message] = overall_from_steps(run.steps, run.cloud_enabled);
    if (status == "completed" && !stats.empty()) {
        message = insights_ready_message(stats);
    }
    return run.finish(status, message, stats);
}

// Finishes a retry whose steps did not include finalize.
Json complete_retry(RetryRun& run) {
    const Json stats =
        collect_summary_stats(run.store, run.repository, run.meeting_id, run.meeting, run.clock);
    bool finalized = false;
    for (const Json& step : run.steps) {
        finalized = finalized || (step_id_is(step, "finalize") &&
                                  detail::get(step, "status") == Json("completed"));
    }
    if (!finalized) {
        set_step(run.steps, "finalize", "completed", saved_state_detail(stats));
    }
    const auto [status, message] = overall_from_steps(run.steps, run.cloud_enabled);
    return run.finish(status, message, stats);
}

Json stored_meeting(MeetingRepository& repository, const std::string& meeting_id) {
    std::optional<Json> meeting = repository.get_meeting(meeting_id);
    if (!meeting) {
        throw py::ValueError("unknown meeting");
    }
    return std::move(*meeting);
}

} // namespace

// ---- claims ----

FinalizationClaim::FinalizationClaim(std::string meeting_id)
    : meeting_id_(std::move(meeting_id)), token_(std::make_shared<const int>(0)) {
    ClaimRegistry& registry = claims();
    const std::lock_guard lock(registry.mutex);
    if (registry.running.contains(meeting_id_)) {
        throw FinalizationBusyError("post-meeting steps are already running for this meeting");
    }
    registry.running.insert(meeting_id_);
    registry.latest[meeting_id_] = token_;
    active_ = true;
}

FinalizationClaim::~FinalizationClaim() {
    release();
    ClaimRegistry& registry = claims();
    const std::lock_guard lock(registry.mutex);
    const auto found = registry.latest.find(meeting_id_);
    if (found != registry.latest.end() && found->second.lock() == token_) {
        registry.latest.erase(found);
    }
}

bool FinalizationClaim::is_active() const {
    const std::lock_guard lock(claims().mutex);
    return active_;
}

bool FinalizationClaim::is_current() const {
    ClaimRegistry& registry = claims();
    const std::lock_guard lock(registry.mutex);
    const auto found = registry.latest.find(meeting_id_);
    return found != registry.latest.end() && found->second.lock() == token_;
}

void FinalizationClaim::release() {
    ClaimRegistry& registry = claims();
    const std::lock_guard lock(registry.mutex);
    if (active_) {
        active_ = false;
        registry.running.erase(meeting_id_);
    }
}

void FinalizationClaim::validate(std::string_view meeting_id) const {
    const std::lock_guard lock(claims().mutex);
    if (!active_ || meeting_id != meeting_id_) {
        throw py::ValueError("inactive or mismatched finalization claim");
    }
}

bool is_running(std::string_view meeting_id) {
    ClaimRegistry& registry = claims();
    const std::lock_guard lock(registry.mutex);
    return registry.running.contains(meeting_id);
}

// ---- seams ----

DiarizerFactory speaker_model_diarizer(std::optional<std::filesystem::path> model,
                                       std::optional<std::filesystem::path> onnxruntime) {
    return [model = std::move(model), onnxruntime = std::move(onnxruntime)](
               diarize::DiarizerHost& host,
               const std::string& meeting_id) -> std::unique_ptr<diarize::Diarizer> {
        return diarize::create_diarizer(model.value_or(std::filesystem::path()),
                                        onnxruntime.value_or(std::filesystem::path()), host,
                                        meeting_id);
    };
}

// ---- the pieces ----

void reload_store(MeetingStateStore& store, MeetingRepository& repository,
                  const std::string& meeting_id, const Clock& clock) {
    std::optional<Json> meeting;
    try {
        meeting = repository.get_meeting(meeting_id);
    } catch (const std::exception& error) {
        logger.error_with(
            error, std::format("Could not reload meeting {} after a pipeline write", meeting_id));
        return;
    }
    const Json raw = meeting ? detail::get(*meeting, "state_json") : Json();
    const std::optional<Json> data =
        detail::truthy(raw) ? meeting_state::parse_state_json(raw) : std::nullopt;
    if (!data) {
        if (detail::truthy(raw)) {
            logger.warning("Corrupt state_json for {} after a pipeline write", meeting_id);
        }
        return;
    }
    try {
        store.replace_document(meeting_state::MeetingState::from_json(*data, clock));
    } catch (const std::exception& error) {
        logger.error_with(error,
                          std::format("Could not replace stored meeting state for {}", meeting_id));
    }
}

int repair_after_consolidation(MeetingStateStore& store, const std::vector<Json>& segments,
                               const Json& outcome) {
    if (detail::str_or_empty(detail::get(outcome, "status")) == "unavailable") {
        return 0;
    }
    try {
        return meeting_state::repair_meeting_state(store, segments);
    } catch (const std::exception& error) {
        logger.error_with(error, "State repair after consolidation failed");
        return 0;
    }
}

void strip_unevidenced_proposed(MeetingStateStore& store) {
    const Json snapshot = store.snapshot();
    const Json& cards = detail::get(snapshot, "cards");
    Json ops = Json::array();
    for (const std::string_view key : meeting_state::CARD_KEYS) {
        if (key == "user_notes" || key == "live_notes") {
            continue;
        }
        const Json& items = detail::get(cards, key);
        if (!items.is_array()) {
            continue;
        }
        for (const Json& item : items) {
            if (!item.is_object()) {
                continue;
            }
            if (detail::get(item, "status") != Json("proposed") ||
                meeting_state::CardItem::from_json(item).is_protected()) {
                continue;
            }
            if (detail::truthy(detail::get(item, "evidence"))) {
                continue;
            }
            Json op = Json::object();
            op["op"] = "remove_item";
            op["id"] = detail::get(item, "id");
            op["base_revision"] = item.contains("revision") ? item.at("revision") : Json(1);
            ops.push_back(std::move(op));
        }
    }
    if (ops.empty()) {
        return;
    }
    try {
        store.apply("system", std::string("finalization"), ops);
    } catch (const std::exception& error) {
        logger.error_with(error, "Could not strip unevidenced proposed cards");
    }
}

void assign_session_speakers(std::vector<TranscriptSegment>& segments,
                             const std::optional<std::string>& me_id, diarize::Diarizer* diarizer,
                             const std::filesystem::path& spool_dir,
                             const std::vector<Json>& chunks) {
    std::vector<std::size_t> loopback;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        TranscriptSegment& segment = segments[i];
        if (segment.channel == meeting_state::kChannelMic && me_id && !me_id->empty()) {
            segment.speaker_participant_id = me_id;
            segment.speaker_source = "channel";
        } else if (segment.channel == meeting_state::kChannelLoopback) {
            loopback.push_back(i);
        }
    }
    if (diarizer == nullptr || loopback.empty()) {
        return;
    }
    try {
        // System audio is labelled from the whole session recording, not chunk by chunk.
        auto [frames, rate, origin] =
            load_channel_session(spool_dir, meeting_state::kChannelLoopback, chunks);
        if (!frames || frames->empty()) {
            return;
        }
        std::vector<diarize::Segment> views;
        views.reserve(loopback.size());
        for (const std::size_t index : loopback) {
            const TranscriptSegment& segment = segments[index];
            diarize::Segment view;
            view.id = segment.segment_id;
            view.start_s = segment.start_s;
            view.end_s = segment.end_s;
            view.speaker_participant_id = segment.speaker_participant_id;
            view.speaker_source = segment.speaker_source;
            views.push_back(std::move(view));
        }
        const std::vector<std::string> labeled =
            diarize::assign_from_frames(*diarizer, views, *frames, rate, origin);
        const std::set<std::string> labeled_ids(labeled.begin(), labeled.end());
        std::vector<diarize::Segment> labeled_views;
        std::vector<std::size_t> labeled_index;
        for (std::size_t i = 0; i < views.size(); ++i) {
            if (labeled_ids.contains(views[i].id)) {
                labeled_views.push_back(views[i]);
                labeled_index.push_back(i);
            }
        }
        diarize::refresh_labels(*diarizer, labeled_views);
        for (std::size_t k = 0; k < labeled_views.size(); ++k) {
            views[labeled_index[k]] = std::move(labeled_views[k]);
        }
        for (std::size_t i = 0; i < views.size(); ++i) {
            TranscriptSegment& segment = segments[loopback[i]];
            segment.speaker_participant_id = views[i].speaker_participant_id;
            segment.speaker_source = views[i].speaker_source;
            if (!views[i].embedding.empty()) {
                std::vector<std::uint8_t> bytes(views[i].embedding.size());
                std::transform(views[i].embedding.begin(), views[i].embedding.end(), bytes.begin(),
                               [](std::byte value) { return static_cast<std::uint8_t>(value); });
                segment.embedding = std::move(bytes);
            }
        }
    } catch (const std::exception& error) {
        logger.error_with(error, "Offline speaker assignment failed");
    }
}

Json steps_from_state(const Json& existing, std::string_view from_step, bool cloud_enabled,
                      bool have_audio) {
    Json steps = existing.is_array() ? existing : Json::array();
    if (steps.empty()) {
        if (have_audio) {
            steps.push_back(make_step("redecode"));
        }
        if (from_step == "speaker_id") {
            steps.push_back(make_step("speaker_id"));
        }
        if (cloud_enabled) {
            steps.push_back(make_step("polish"));
            steps.push_back(make_step("consolidation"));
        }
        steps.push_back(make_step("finalize"));
    }
    if (step_name(from_step) && from_step != "failed") {
        steps = ensure_step(steps, from_step);
    }
    return steps;
}

std::vector<std::string> run_ids(const Json& steps, std::string_view from_step) {
    std::vector<std::string> ids;
    for (const Json& step : steps) {
        const Json& id = step_id_of(step);
        if (detail::truthy(id)) {
            ids.push_back(detail::py_str(id));
        }
    }
    std::string start(from_step);
    if (from_step == "failed") {
        std::optional<std::string> first;
        for (const Json& step : steps) {
            const Json& status = detail::get(step, "status");
            if (status == Json("failed") || status == Json("skipped")) {
                first = detail::py_str(step_id_of(step));
                break;
            }
        }
        if (!first) {
            if (std::find(ids.begin(), ids.end(), "consolidation") != ids.end()) {
                first = "consolidation";
            } else if (!ids.empty()) {
                first = ids.front();
            } else {
                return {};
            }
        }
        start = *first;
    }
    const auto found = std::find(ids.begin(), ids.end(), start);
    if (found == ids.end()) {
        if (step_name(start)) {
            return {start};
        }
        return {};
    }
    return std::vector<std::string>(found, ids.end());
}

std::pair<std::string, std::string> overall_from_steps(const Json& steps, bool cloud_enabled) {
    const std::string failed = failed_steps_message(steps);
    if (!failed.empty()) {
        return {"failed", failed};
    }
    bool has_ai_step = false;
    for (const Json& step : steps) {
        has_ai_step =
            has_ai_step || step_id_is(step, "polish") || step_id_is(step, "consolidation");
    }
    if (!cloud_enabled && !has_ai_step) {
        return {"disabled", std::string(meeting_state::kInsightsOffMessage)};
    }
    return {"completed", "Final insights are ready."};
}

std::optional<Json> coverage_refusal(MeetingRepository& repository, const std::string& meeting_id,
                                     const std::vector<TranscriptSegment>& decoded,
                                     bool coverage_guard) {
    std::vector<Json> existing;
    try {
        existing = repository.get_segments(meeting_id, -1.0, std::nullopt);
    } catch (const std::exception& error) {
        logger.error_with(
            error, std::format("Could not read the draft transcript for {}; coverage guard skipped",
                               meeting_id));
    }
    const std::int64_t new_words = word_count(decoded);
    const std::int64_t old_words = word_count(existing);
    if (!(coverage_guard && old_words != 0 &&
          static_cast<double>(new_words) < 0.8 * static_cast<double>(old_words))) {
        return std::nullopt;
    }
    logger.warning("Keeping live draft transcript for {}: offline pass has {} words vs draft {} "
                   "(AMI IN1009 guard: do not replace a sparser decode)",
                   meeting_id, new_words, old_words);
    Json refusal = Json::object();
    refusal["ok"] = false;
    refusal["error"] = sparse_redecode_detail(new_words, old_words);
    refusal["kept_draft"] = true;
    return std::optional<Json>(std::in_place, std::move(refusal));
}

// ---- redecode ----

Json rerun_redecode(const std::shared_ptr<MeetingRepository>& repository,
                    const std::string& meeting_id, const RedecodeRequest& request) {
    const Clock& clock = clock_or_system(request.clock);
    const Json meeting = stored_meeting(*repository, meeting_id);
    std::shared_ptr<MeetingStateStore> store = request.store;
    if (!store) {
        store = open_meeting_store(repository, meeting_id, meeting, request.settings, clock);
    }
    const std::string spool_text = detail::str_or_empty(detail::get(meeting, "spool_dir"));
    const std::filesystem::path spool_dir = path_from_utf8(spool_text);
    const std::vector<Json> chunks = redecode_chunks(*repository, meeting_id);

    std::vector<TranscriptSegment> decoded;
    {
        RedecodeEngine speech(request.model_lease);
        try {
            if (request.transcribe) {
                decoded = request.transcribe(spool_dir, chunks, request.progress);
            } else {
                const std::string language = decode_language(request.language);
                if (const auto unavailable = speech.load(meeting_id, request, language)) {
                    return failure(*unavailable);
                }
                decoded = speech.engine().transcribe_offline_session(
                    spool_dir, chunks, request.progress, request.cancel);
            }
        } catch (const std::exception& error) {
            logger.error_with(error,
                              std::format("Redecode transcription failed for {}", meeting_id));
            return failure(error.what());
        }
    }
    if (decoded.empty()) {
        return failure("Re-decoding produced no transcript.");
    }
    if (auto refusal =
            coverage_refusal(*repository, meeting_id, decoded, request.redecode_coverage_guard)) {
        return std::move(*refusal);
    }

    StoreDiarizerHost host(*store);
    std::unique_ptr<diarize::Diarizer> fresh;
    diarize::Diarizer* diarizer = nullptr;
    if (request.speaker_id_backend == "off") {
        diarizer = nullptr;
    } else if (request.diarizer) {
        diarizer = *request.diarizer;
    } else if (std::any_of(decoded.begin(), decoded.end(), [](const TranscriptSegment& segment) {
                   return segment.channel == meeting_state::kChannelLoopback;
               })) {
        try {
            if (request.diarizer_factory) {
                fresh = request.diarizer_factory(host, meeting_id);
            }
        } catch (const std::exception& error) {
            logger.error_with(error, "Could not create a diarizer for redecode retry");
        }
        diarizer = fresh.get();
    }
    assign_session_speakers(decoded, me_participant_id(*store), diarizer, spool_dir, chunks);
    return store_redecoded(*repository, *store, meeting_id, decoded, request.repair, clock);
}

// ---- the speaker pass ----

Json run_speaker_pass(MeetingRepository& /*repository*/, const std::string& meeting_id,
                      MeetingStateStore& /*store*/, const std::string& /*spool_dir*/,
                      const SpeakerPassGate& gate,
                      const std::function<void(const std::string&, int, int)>& /*progress*/,
                      const std::function<void()>& /*on_start*/) {
    if (gate.ok) {
        // Uploads require the saved-meeting desktop action and its fresh confirmation.
        logger.warning("Cloud speaker pass unavailable");
        // Python logs this after any pass the gate let through, a failed one included.
        logger.info("Speaker pass for meeting {} finished: ok=False applied=None", meeting_id);
    }
    Json result = Json::object();
    result["ok"] = false;
    result["skipped"] = !gate.ok;
    result["applied"] = 0;
    result["created"] = 0;
    result["windows"] = 0;
    result["error"] = gate.ok ? std::string(kSpeakerPassUnavailable) : gate.reason;
    return result;
}

Json rerun_speakers(const std::string& meeting_id, const RerunRequest& request) {
    const Clock& clock = clock_or_system(request.clock);
    return rerun_speakers(meeting_id, request,
                          speaker_pass_gate(
                              request.options.speaker_id_backend,
                              request.options.speaker_audio_consent, [] { return false; }, clock));
}

Json rerun_speakers(const std::string& meeting_id, const RerunRequest& request,
                    const SpeakerPassGate& gate) {
    return run_claimed(meeting_id, request.claim, [&] {
        const Clock& clock = clock_or_system(request.clock);
        const Json meeting = stored_meeting(*request.repository, meeting_id);
        std::shared_ptr<MeetingStateStore> store = request.store;
        if (!store) {
            store = open_meeting_store(request.repository, meeting_id, meeting, request.settings,
                                       clock);
        }
        Json result =
            run_speaker_pass(*request.repository, meeting_id, *store,
                             detail::str_or_empty(detail::get(meeting, "spool_dir")), gate);
        result["state"] = store->snapshot();
        return result;
    });
}

Json rerun_approved_speakers(const SpeakerUploadOffer& offer,
                             const std::filesystem::path& recordings_root,
                             const RerunRequest& request, const CloudSpeakerOptions& options,
                             const std::function<void(const std::string&, int, int)>& progress) {
    return run_claimed(offer.meeting_id, request.claim, [&] {
        const auto clock = &clock_or_system(request.clock);
        if (request.cancel.cancelled()) {
            throw std::runtime_error("Speaker identification was canceled.");
        }
        if (speech::retired(speech::local_today(*clock))) {
            throw std::runtime_error(std::string(speech::kSpeakerModelRetiredMessage));
        }
        const auto still_approved = [&] {
            if (request.cancel.cancelled() || speech::retired(speech::local_today(*clock))) {
                return false;
            }
            try {
                return describe_speaker_upload(*request.repository, offer.meeting_id,
                                               recordings_root, offer.from_chunks) == offer;
            } catch (const std::exception&) {
                return false;
            }
        };
        if (!still_approved()) {
            throw std::runtime_error("The meeting changed. Review and approve the upload again.");
        }
        const auto meeting = stored_meeting(*request.repository, offer.meeting_id);
        auto store = request.store;
        if (!store) {
            store = open_meeting_store(request.repository, offer.meeting_id, meeting,
                                       request.settings, *clock);
        }
        const auto active = store->with_state([](const meeting_state::MeetingState& state) {
            return state.status != "ended" || state.finalization.status == "pending" ||
                   state.finalization.status == "running" ||
                   detail::get(state.insight_review, "status") == "running";
        });
        if (active) {
            throw FinalizationBusyError("Wait for this meeting's post-processing to finish.");
        }
        auto approved_options = options;
        approved_options.from_chunks = offer.from_chunks;
        approved_options.still_approved = still_approved;
        return run_cloud_speaker_pass(*request.repository, offer.meeting_id, *store,
                                      offer.spool_dir, approved_options, progress, request.cancel);
    });
}

// ---- the retry pipeline ----

Json rerun_finalization(const std::string& meeting_id, const RerunRequest& request) {
    return run_claimed(meeting_id, request.claim, [&] {
        const Clock& clock = clock_or_system(request.clock);
        const Json meeting = stored_meeting(*request.repository, meeting_id);
        std::shared_ptr<MeetingStateStore> store = request.store;
        if (!store) {
            store = open_meeting_store(request.repository, meeting_id, meeting, request.settings,
                                       clock);
        }
        const auto [cloud_enabled, existing] =
            store->with_state([](const meeting_state::MeetingState& state) {
                return std::pair<bool, Json>(state.cloud_enabled, state.finalization.steps);
            });
        const bool have_audio = detail::truthy(detail::get(meeting, "spool_dir"));
        Json steps = steps_from_state(existing, request.from_step, cloud_enabled, have_audio);
        const std::vector<std::string> ids = run_ids(steps, request.from_step);
        RetryRun run(*request.repository, meeting_id, meeting, *store, std::move(steps),
                     cloud_enabled, request.progress, clock);

        for (const std::string& step_id : ids) {
            if (step_id == "redecode") {
                retry_redecode(run, request, store);
            } else if (step_id == "speaker_id") {
                // A key is never held in this build, so an OpenAI backend with consent is refused
                // for the missing key, as live End would.
                const SpeakerPassGate gate = speaker_pass_gate(
                    request.options.speaker_id_backend, request.options.speaker_audio_consent,
                    [] { return false; }, clock);
                retry_speaker_id(run, gate);
            } else if (step_id == "polish") {
                retry_polish(run, request, store);
            } else if (step_id == "consolidation") {
                retry_consolidation(run, request, store);
            } else if (step_id == "finalize") {
                return retry_finalize(run);
            }
        }
        return complete_retry(run);
    });
}

} // namespace fv::meeting
