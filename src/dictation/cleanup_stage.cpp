#include "dictation/cleanup_stage.h"

#include <format>
#include <stdexcept>
#include <utility>
#include <vector>

#include "base/log.h"
#include "dictation/cleanup_prompt.h"
#include "dictation/text_preparer.h"
#include "history/dictation_stats.h"
#include "llm/cleanup_settings.h"
#include "llm/transcript_cleanup.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "text/cleanup_prompts.h"

namespace fv::dictation {

namespace {

constexpr log::Logger kLog{"services.runtime.transcription"};

WordCounter stats_words() {
    return WordCounter{[](std::string_view text) { return dictation_stats::count_words(text); },
                       [](std::string_view before, std::string_view after) {
                           const auto edited = dictation_stats::words_edited(before, after);
                           return edited > 0 ? static_cast<std::size_t>(edited) : std::size_t{0};
                       }};
}

std::string pass_level(const Json& settings, bool has_profile) {
    return cleanup_level(settings, has_profile,
                         [](const Json& values) { return text::custom_prompt(values); });
}

} // namespace

CleanupOutcome run_cleanup(std::string_view raw, const CleanupStageInput& input,
                           const CleanupStageServices& services) {
    CleanupOutcome outcome;
    if (raw.empty() || py::strip(raw).empty()) {
        outcome.text = std::string(raw);
        return outcome;
    }
    const TextPreparer& preparer =
        services.preparer != nullptr ? *services.preparer : dictionary_preparer();
    const Json& settings = input.settings;
    const text::CleanupProfile* profile = input.profile ? &*input.profile : nullptr;

    // Before the switch below: the dictionary applies with cleanup off, and to uploads and
    // batches too.
    const PreparedText prepared = prepare_text(raw, input.job, settings, preparer);
    outcome.spoken_text = prepared.text;
    outcome.stats_counts = stats_counts(prepared, stats_words());
    const auto finish_without_cleanup = [&] {
        const FinishedText finished = finish_text(prepared.cleanup_input, prepared, preparer);
        outcome.delivery_html = finished.html;
        outcome.text = finished.text;
        return outcome;
    };
    // A profile formats whatever the switch says; the switch counts only as a real bool.
    const bool enabled = profile != nullptr || keys::transcript_cleanup_enabled.read(settings);
    // Respect the saved recipient even when an old or malformed model field makes the
    // reference settings resolver fall back to its endpoint default.
    const std::string provider = keys::transcript_cleanup_provider.read(settings);
    const bool agent = llm::is_installed_cleanup_agent(provider);
    const bool local_dictation = input.job && input.job->mode == JobMode::Dictation &&
                                 (input.job->source == "ui" || input.job->source == "hotkey") &&
                                 !input.job->changed_by_program && !input.batch_context;
    if (!input.allow_ai || !enabled || prepared.skip_cleanup || (agent && !local_dictation)) {
        return finish_without_cleanup();
    }
    TranscriptCleaner& cleaner =
        services.cleaner != nullptr ? *services.cleaner : null_transcript_cleaner();
    if ((agent && !llm::cleanup_agent_authorized(settings, provider)) ||
        !cleaner.is_available(settings)) {
        kLog.warning("Transcript cleanup enabled but unavailable; using raw text");
        outcome.failure = std::string(CLEANUP_UNAVAILABLE);
        return finish_without_cleanup();
    }

    // Private dictionary spellings stay on this computer. Plan placeholders from the original
    // words if a replacement changed the text, and apply the dictionary to the returned words.
    std::optional<PreparedText> spoken;
    if (agent && prepared.text != raw) {
        spoken = prepare_text(raw, input.job, settings, preparer, false);
        if (spoken->skip_cleanup) {
            return finish_without_cleanup();
        }
    }
    const auto& submitted = spoken ? *spoken : prepared;
    if (services.announce) {
        services.announce(appcore::OverlayState::Cleaning,
                          profile != nullptr ? std::format("Formatting · {}...", profile->name)
                                             : std::string("Cleaning up..."));
    }
    const std::vector<std::string> rules = text::resolve_transcript_cleanup_rules(settings);
    CleanupRequest request;
    request.text = submitted.cleanup_input;
    request.prepared = &submitted;
    request.job = input.job;
    request.settings = settings;
    request.batch_context = input.batch_context;
    request.timeout_s = input.timeout_s;
    request.cancel = input.cancel;
    request.system_prompt = compose_cleanup_prompt(
        settings, input.job, profile, rules, submitted,
        input.batch_context ? std::optional<std::string_view>(*input.batch_context) : std::nullopt);
    request.profile_name = profile != nullptr ? profile->name : std::string();

    const Clock& clock = services.clock != nullptr ? *services.clock : system_clock();
    const auto started = clock.steady();
    CleanupResult result;
    if (input.cancel.cancelled()) {
        // Canceled text never reaches a cleanup provider.
        result.text = request.text;
        result.error = std::string(CLEANUP_CANCELED_REASON);
    } else {
        result = cleaner.cleanup(request);
    }
    const double elapsed = std::chrono::duration<double>(clock.steady() - started).count();
    // A changed transcript also proves cleanup ran, whatever error the pass reported.
    const bool cleaned = !result.error || result.text != request.text;
    if (!cleaned) {
        outcome.failure = result.error->empty() ? std::string("cleanup failed") : *result.error;
    }
    if (agent && !cleaned) {
        return finish_without_cleanup();
    }
    const FinishedText finished =
        finish_text(agent ? apply_dictionary(result.text, settings, preparer) : result.text,
                    submitted, preparer);
    outcome.delivery_html = finished.html;
    outcome.text = finished.text;
    if (!finished.ok) {
        // The cleanup dropped a snippet, so its output is not used.
        outcome.failure = "snippet placeholder lost";
        return finish_without_cleanup();
    }
    if (cleaned) {
        outcome.info = CleanupInfo{result.provider, result.model, elapsed,
                                   pass_level(settings, profile != nullptr)};
    }
    if (finished.text != finished.raw_text) {
        outcome.raw_text = finished.raw_text;
    }
    return outcome;
}

LlmTranscriptCleaner::LlmTranscriptCleaner(std::shared_ptr<llm::TranscriptCleanup> cleanup)
    : cleanup_(std::move(cleanup)) {
    if (!cleanup_) {
        throw std::invalid_argument("LlmTranscriptCleaner needs a TranscriptCleanup");
    }
}

bool LlmTranscriptCleaner::is_available(const Json& settings) {
    // Re-applied on every pass so Settings changes take effect without a restart.
    cleanup_->configure(llm::resolve_transcript_cleanup_provider(settings),
                        llm::resolve_transcript_cleanup_model(settings),
                        llm::resolve_transcript_cleanup_reasoning(settings));
    return cleanup_->is_available();
}

CleanupResult LlmTranscriptCleaner::cleanup(const CleanupRequest& request) {
    CleanupResult result;
    result.text = cleanup_->cleanup(request.text, request.system_prompt, request.timeout_s,
                                    std::nullopt, request.cancel);
    result.error = cleanup_->last_error();
    result.provider = cleanup_->provider();
    result.model = cleanup_->model();
    result.level = pass_level(request.settings, !request.profile_name.empty());
    return result;
}

} // namespace fv::dictation
