#include "meeting/engine.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "base/log.h"
#include "meeting/detail/engine_internal.h"

#include "meeting_intel/citation_verifier.h"
#include "meeting_intel/insight_review.h"
#include "meeting_intel/intel_settings.h"
#include "meeting_intel/live_highlights.h"
#include "meeting_intel/voice_commands.h"
#include "meeting_state/fast_ops.h"
#include "pyport/text.h"

namespace fv::meeting {

void MeetingEngine::start_fast_features() {
    if (!deps_.local_fast_judge || !deps_.settings || processing_stop_.load()) {
        return;
    }
    const auto settings = deps_.settings();
    auto current = store();
    if (!current) {
        return;
    }
    // Called at start, for every committed chunk and on the heartbeat: only the workers that are
    // missing and now enabled start, and none after teardown (processing_stop_).
    bool need_highlights = false;
    bool need_citations = false;
    bool need_voice = false;
    {
        const std::lock_guard lock(state_mutex_);
        need_highlights = live_highlights_ == nullptr;
        need_citations = citation_verifier_ == nullptr;
        need_voice = voice_commands_ == nullptr;
    }
    std::shared_ptr<meeting_intel::LiveHighlights> highlights;
    const bool enabled = meeting_intel::resolve_typesafe_feature_enabled(
        settings, meeting_intel::kFeatureHighlights);
    if (need_highlights && (enabled || meeting_intel::resolve_typesafe_feature_enabled(
                                           settings, meeting_intel::kFeatureQuestionRadar))) {
        highlights = std::make_shared<meeting_intel::LiveHighlights>(
            current, deps_.repository,
            meeting_intel::LiveHighlights::Deps{
                deps_.local_fast_judge,
                [this] {
                    return !processing_stop_.load() &&
                           meeting_intel::resolve_typesafe_feature_enabled(
                               deps_.settings(), meeting_intel::kFeatureHighlights);
                },
                [this] {
                    return !processing_stop_.load() &&
                           meeting_intel::resolve_typesafe_feature_enabled(
                               deps_.settings(), meeting_intel::kFeatureQuestionRadar);
                }});
    }
    std::shared_ptr<meeting_intel::CitationVerifier> citations;
    if (need_citations && meeting_intel::resolve_typesafe_feature_enabled(
                              settings, meeting_intel::kFeatureCitations)) {
        citations = std::make_shared<meeting_intel::CitationVerifier>(
            current, deps_.repository,
            meeting_intel::CitationVerifier::Deps{
                deps_.local_fast_judge,
                [this] {
                    return !processing_stop_.load() &&
                           meeting_intel::resolve_typesafe_feature_enabled(
                               deps_.settings(), meeting_intel::kFeatureCitations);
                },
                &clock_});
    }
    std::shared_ptr<meeting_intel::VoiceCommandListener> listener;
    if (need_voice && meeting_intel::resolve_typesafe_voice_commands_enabled(settings)) {
        listener = std::make_shared<meeting_intel::VoiceCommandListener>(
            current, meeting_intel::resolve_typesafe_voice_command_names(settings),
            meeting_intel::VoiceCommandListener::Deps{
                deps_.local_fast_judge,
                [this] {
                    return !processing_stop_.load() &&
                           meeting_intel::resolve_typesafe_voice_commands_enabled(deps_.settings());
                },
                [this](const Json& feedback) {
                    // The correction's outcome is the engine's own feedback in OpenWhisper
                    // (_apply_spoken_action), kept in the meeting state; the listener's
                    // acknowledgements are only heard.
                    const auto phase = feedback.value("phase", std::string());
                    emit_voice_feedback(
                        feedback,
                        feedback.value("command", std::string()) == "fix_transcript" &&
                            (phase == "uncertain" || phase == "saved" || phase == "error"));
                },
                [this](const std::string& spoken) { request_spoken_recap(spoken); },
                [this](const std::string& command,
                       const std::vector<meeting_state::OpResult>& results) {
                    // _notify_human_guidance: a spoken correction's note wakes the agent.
                    notify_intelligence_guidance(results);
                    // _on_voice_command_applied
                    const auto applied = std::ranges::count_if(
                        results, [](const auto& result) { return result.ok; });
                    emit("voice_command",
                         Json{{"command", command},
                              {"applied", applied},
                              {"rejected", static_cast<std::int64_t>(results.size()) - applied}});
                }});
    }
    const std::lock_guard lock(state_mutex_);
    if (!processing_stop_.load()) {
        // A worker another thread started meanwhile is kept; this one is dropped unused.
        if (listener && !voice_commands_) {
            voice_commands_ = std::move(listener);
        }
        if (citations && !citation_verifier_) {
            citation_verifier_ = std::move(citations);
        }
        if (highlights && !live_highlights_) {
            live_highlights_ = std::move(highlights);
        }
    }
}

bool MeetingEngine::refresh_live_highlights_status() {
    const auto current = store();
    if (!current || !deps_.settings) {
        return false;
    }
    const auto [meeting_status, previous] = current->with_state(
        [](const auto& state) { return std::pair{state.status, state.live_highlights_status}; });
    // An ended meeting keeps the status it had; current settings never rewrite it.
    if (meeting_status != "active" && meeting_status != "paused") {
        return false;
    }
    std::string status = "off";
    if (meeting_intel::resolve_typesafe_feature_enabled(deps_.settings(),
                                                        meeting_intel::kFeatureHighlights)) {
        // FioriVoce asks only a judge on this computer, so a hosted one is unavailable rather
        // than OpenWhisper's "needs_insights".
        const auto judge = deps_.local_fast_judge ? deps_.local_fast_judge() : nullptr;
        status = judge && judge->is_local() ? "on" : "unavailable";
    }
    return status != previous &&
           current->update_runtime_fields(Json{{"live_highlights_status", status}});
}

void MeetingEngine::emit_voice_feedback(const Json& feedback, bool persist) {
    if (processing_stop_.load()) {
        return;
    }
    if (persist) {
        if (auto current = store()) {
            try {
                current->apply("system", std::string(meeting_state::VOICE_COMMAND_ACTOR),
                               Json::array({Json{{"op", "voice_feedback"},
                                                 {"message", feedback.value("message", "")}}}));
            } catch (const std::exception& error) {
                detail::engine_logger.error_with(error, "Could not save voice feedback");
            }
        }
    }
    // _on_voice_feedback: nothing while the meeting is inactive or paused; each payload is
    // numbered so the dashboard (state.ts) drops one older than what it showed; every socket,
    // guests included, hears it, and so do the listeners.
    if (!active_.load() || meeting_clock_.is_paused()) {
        return;
    }
    auto payload = feedback;
    payload["seq"] = ++voice_feedback_seq_;
    emit("voice_feedback", payload);
    auto message = payload;
    message["type"] = "voice_feedback";
    broadcast(message);
}

void MeetingEngine::request_spoken_recap(const std::string& spoken) {
    // engine.py _apply_spoken_action: "Preparing a recap…" only once the note agent took the
    // request, and the result always after it (add_done_callback on a finished future runs at
    // once), so a completion that comes back first waits for the "working" line.
    struct Progress {
        std::mutex mutex;
        bool announced = false;
        std::optional<Json> finished;
    };
    auto progress = std::make_shared<Progress>();
    // engine.py's finished(): the note request always completes with an agent result, a failed
    // pass included, so anything but saved notes reads "produced no saved notes"; "could not
    // finish" is for a result that cannot be read.
    const auto final_feedback = [](const Json& result) {
        std::string message;
        try {
            const bool saved = result.value("ok", false) && result.value("applied", 0) > 0;
            message = saved ? "Recap added to the live notes."
                            : "The recap produced no saved notes. Try again from the notes "
                              "request box.";
        } catch (const std::exception&) {
            detail::engine_logger.warning("Spoken recap failed.");
            message = "The recap could not finish. Try again from the notes request box.";
        }
        const bool saved = message.starts_with("Recap added");
        return Json{{"phase", saved ? "saved" : "error"},
                    {"command", "recap"},
                    {"message", std::move(message)}};
    };
    try {
        request_note_adjustment(
            "Add a concise recap of the meeting so far as a new live note headed Recap. "
            "Cover the requested scope, cite supporting transcript segments, preserve unrelated "
            "notes, and distinguish proposals from agreements. Spoken request: " +
                py::slice(spoken, 0, 1000),
            [this, progress, final_feedback](const Json& result) {
                {
                    const std::lock_guard lock(progress->mutex);
                    if (!progress->announced) {
                        progress->finished = result;
                        return;
                    }
                }
                emit_voice_feedback(final_feedback(result), true);
            });
    } catch (const std::exception&) {
        // Usually the note agent being offline; the request quotes speech, so no message.
        detail::engine_logger.warning("Spoken recap could not start.");
        emit_voice_feedback(
            Json{{"phase", "error"},
                 {"command", "recap"},
                 {"message", "Recap is unavailable while the note agent is offline."}},
            true);
        return;
    }
    emit_voice_feedback(
        Json{{"phase", "working"}, {"command", "recap"}, {"message", "Preparing a recap…"}}, true);
    std::optional<Json> early;
    {
        const std::lock_guard lock(progress->mutex);
        progress->announced = true;
        early = std::move(progress->finished);
    }
    if (early) {
        emit_voice_feedback(final_feedback(*early), true);
    }
}

void MeetingEngine::observe_voice_commands(const std::vector<Json>& rows) {
    // observe_highlights, called first for the same commit, started any missing listener.
    std::shared_ptr<meeting_intel::VoiceCommandListener> listener;
    {
        const std::lock_guard lock(state_mutex_);
        listener = voice_commands_;
    }
    if (listener) {
        listener->observe(rows);
    }
}

void MeetingEngine::observe_voice_preview(const Json& row) {
    std::shared_ptr<meeting_intel::VoiceCommandListener> listener;
    {
        const std::lock_guard lock(state_mutex_);
        listener = voice_commands_;
    }
    if (!listener) {
        // A listener enabled after the first chunk starts with the next preview.
        start_fast_features();
        const std::lock_guard lock(state_mutex_);
        listener = voice_commands_;
    }
    if (listener) {
        listener->observe_preview(row);
    }
}

void MeetingEngine::observe_highlights(const std::vector<Json>& rows, double frontier) {
    // _on_chunk_result: workers enabled after the meeting started begin with the next chunk.
    start_fast_features();
    std::shared_ptr<meeting_intel::LiveHighlights> worker;
    {
        const std::lock_guard lock(state_mutex_);
        worker = live_highlights_;
    }
    if (worker) {
        worker->observe(rows, frontier);
    }
}

void MeetingEngine::finalize_highlights() {
    if (!deps_.local_fast_judge || !deps_.settings || final_highlights_cancel_.cancelled() ||
        !meeting_intel::resolve_typesafe_feature_enabled(deps_.settings(),
                                                         meeting_intel::kFeatureHighlights)) {
        return;
    }
    const auto current = store();
    if (!current) {
        return;
    }
    meeting_intel::LiveHighlights highlights(
        current, deps_.repository,
        {deps_.local_fast_judge,
         [this] {
             return meeting_intel::resolve_typesafe_feature_enabled(
                 deps_.settings(), meeting_intel::kFeatureHighlights);
         }},
        final_highlights_cancel_.token());
    try {
        highlights.finalize();
    } catch (const std::exception&) {
        log::Logger("meeting.highlights").warning("Final meeting highlights could not finish.");
    }
}

void MeetingEngine::review_final_insights() {
    const auto current = store();
    if (!current || !deps_.settings || (!deps_.local_review_judge && !deps_.local_fast_judge)) {
        return;
    }
    const auto allowed = [this] {
        return meeting_intel::resolve_meeting_insight_review(deps_.settings())
            .value("enabled", false);
    };
    if (allowed()) {
        auto judge = deps_.local_review_judge;
        if (!judge) {
            judge = [fast = deps_.local_fast_judge](double) {
                return fast();
            };
        }
        meeting_intel::run_insight_review(*current, *deps_.repository,
                                          {std::move(judge), allowed, &clock_},
                                          final_insights_cancel().token());
    }
}

void MeetingEngine::stop_fast_features() {
    std::shared_ptr<meeting_intel::VoiceCommandListener> listener;
    std::shared_ptr<meeting_intel::CitationVerifier> citations;
    std::shared_ptr<meeting_intel::LiveHighlights> highlights;
    {
        const std::lock_guard lock(state_mutex_);
        listener = std::move(voice_commands_);
        citations = std::move(citation_verifier_);
        highlights = std::move(live_highlights_);
    }
    if (listener) {
        listener->shutdown();
    }
    if (citations) {
        citations->shutdown();
    }
    if (highlights) {
        highlights->shutdown();
    }
}

} // namespace fv::meeting
