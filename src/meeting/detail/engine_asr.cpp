// MeetingEngine: the meeting's ASR, its chunks and the live caption previews
// (meeting/engine.py _start_asr, _load_deferred_asr, _adopt_asr, _stop_asr, _on_chunk,
// _on_chunk_result, _on_asr_backlog_status, _on_asr_chunk_failure, _on_speech_preview).

#include <algorithm>
#include <exception>
#include <format>
#include <utility>

#include "meeting/detail/engine_internal.h"
#include "meeting/detail/py_value.h"
#include "meeting/engine.h"
#include "pyport/text.h"

namespace fv::meeting {

namespace {

const log::Logger& logger = detail::engine_logger;

using meeting_state::SpooledChunk;
using meeting_state::TranscriptSegment;

} // namespace

void MeetingEngine::start_asr(bool defer_load) {
    std::string language = py::lower(py::strip(options_.asr_language));
    if (language.empty()) {
        language = "auto";
    }
    std::shared_ptr<MeetingAsr> created;
    try {
        AsrCallbacks callbacks;
        callbacks.on_backlog_status = [this](const std::string& message, bool overloaded) {
            on_asr_backlog_status(message, overloaded);
        };
        callbacks.on_chunk_failure = [this](const std::string& message) {
            on_asr_chunk_failure(message);
        };
        if (options_.asr_remote) {
            callbacks.connected_message =
                "Transcribing on " + options_.asr_remote->value("host_name", std::string());
            callbacks.on_connection_status = [this](const std::string& message, bool connected) {
                if (auto current = store()) {
                    auto speech = current->with_state(
                        [](const meeting_state::MeetingState& state) { return state.speech; });
                    speech["connected"] = connected;
                    speech["message"] = message;
                    current->update_runtime_fields(Json{{"speech", speech}});
                    emit_status(message);
                }
            };
        }
        created = deps_.asr(meeting_id(), options_.asr_model, language, std::move(callbacks));
        if (created) {
            const std::lock_guard lock(retired_mutex_);
            retired_asr_.push_back(created);
        }
        if (created && defer_load && !load_deferred_asr(created)) {
            return;
        }
    } catch (const std::exception& error) {
        logger.error_with(error, "Meeting ASR engine unavailable");
        emit("error", Json{{"code", "asr_unavailable"}, {"message", error.what()}});
        return;
    }
    if (!created || !created->is_available()) {
        report_unavailable_asr(created);
        return;
    }
    adopt_asr(created);
}

bool MeetingEngine::load_deferred_asr(const std::shared_ptr<MeetingAsr>& candidate) {
    {
        // Published before the native load, so End or Quit can cancel it.
        const std::lock_guard lock(lifecycle_mutex_);
        if (processing_stop_.load()) {
            candidate->stop();
            return false;
        }
        const std::lock_guard state(state_mutex_);
        asr_ = candidate;
    }
    candidate->load_backend(processing_cancel_.token());
    bool release_due = false;
    bool loaded = true;
    {
        const std::lock_guard lock(lifecycle_mutex_);
        if (processing_stop_.load()) {
            if (asr() == candidate) {
                stop_asr_locked("processing startup canceled", release_due);
            } else {
                candidate->stop();
            }
            loaded = false;
        }
    }
    if (release_due) {
        emit_asr_released();
    }
    return loaded;
}

void MeetingEngine::report_unavailable_asr(const std::shared_ptr<MeetingAsr>& candidate) {
    {
        const std::lock_guard lock(state_mutex_);
        if (asr_ == candidate) {
            asr_.reset();
        }
    }
    if (candidate) {
        // It never started; stopping it gives back the speech engine it holds.
        try {
            candidate->stop();
        } catch (const std::exception& error) {
            logger.error_with(error, "ASR stop failed (unavailable model)");
        }
    }
    logger.error("Meeting ASR model {} is not available; chunks will stay pending",
                 py::repr(options_.asr_model));
    emit("error", Json{{"code", "asr_unavailable"},
                       {"message", "The transcription model is not available; audio is recorded "
                                   "and can be transcribed later."}});
}

void MeetingEngine::adopt_asr(const std::shared_ptr<MeetingAsr>& candidate) {
    const std::lock_guard lock(lifecycle_mutex_);
    if (processing_stop_.load()) {
        candidate->stop();
        return;
    }
    {
        const std::lock_guard state(state_mutex_);
        asr_ = candidate;
    }
    {
        // The pending SQLite chunks replay under the chunk lock before live delivery opens.
        const std::lock_guard chunks(chunk_mutex_);
        {
            const std::lock_guard index(index_mutex_);
            preview_frontiers_.clear();
        }
        candidate->start(
            [this](const SpooledChunk& chunk, std::vector<TranscriptSegment> segments) {
                on_chunk_result(chunk, std::move(segments));
            });
        candidate->requeue_pending();
        const std::lock_guard state(state_mutex_);
        asr_ready_ = true;
    }
    candidate->start_preview([this](const Json& preview) { on_speech_preview(preview); });
    preview_target_ = candidate.get();
}

bool MeetingEngine::stop_asr_locked(std::string_view context, bool& release_due) {
    // Meeting intelligence (M8) shuts its voice commands and live signals down here.
    if (const auto current = asr()) {
        preview_target_ = nullptr;
        try {
            current->stop();
        } catch (const std::exception& error) {
            logger.error_with(error, std::format("ASR stop failed ({})", context));
            return false;
        }
        const std::lock_guard state(state_mutex_);
        asr_.reset();
        asr_ready_ = false;
    }
    // End can come before the processing start built an ASR: the stop flag prevents any later
    // model load, so the host's dictation lease is free on that path too.
    if (!asr_release_emitted_) {
        asr_release_emitted_ = true;
        release_due = true;
    }
    return true;
}

// asr_released is a separate event from `ended`, which fires before the offline pass that still
// decodes on this model: only once the ASR stopped are the weights gone, so only then may a
// listener load an engine of its own.
bool MeetingEngine::stop_asr(std::string_view context) {
    bool release_due = false;
    bool stopped = false;
    {
        const std::lock_guard lock(lifecycle_mutex_);
        stopped = stop_asr_locked(context, release_due);
    }
    if (release_due) {
        emit_asr_released();
    }
    return stopped;
}

void MeetingEngine::emit_asr_released() {
    const std::string id = meeting_id();
    emit("asr_released", Json{{"meeting_id", id.empty() ? Json() : Json(id)}});
}

// Routes a finished chunk to ASR, once per chunk id.
void MeetingEngine::on_chunk(const SpooledChunk& chunk) {
    const std::lock_guard chunks(chunk_mutex_);
    {
        const std::lock_guard index(index_mutex_);
        chunk_index_[chunk.chunk_id] = chunk;
        if (enqueued_chunk_ids_.contains(chunk.chunk_id)) {
            return;
        }
    }
    std::shared_ptr<MeetingAsr> current;
    {
        const std::lock_guard state(state_mutex_);
        if (!asr_ || !asr_ready_) {
            // SQLite owns this pending work while the model starts; its ordered replay takes the
            // chunk lock before live delivery opens, so this callback cannot overtake it.
            return;
        }
        current = asr_;
    }
    try {
        // False means the bounded queue deferred this durable row; the refill picks it up.
        current->enqueue(chunk);
        {
            const std::lock_guard index(index_mutex_);
            enqueued_chunk_ids_.insert(chunk.chunk_id);
        }
        failures_.recovered("enqueue");
    } catch (const std::exception&) {
        // The row stays pending in SQLite, so the meeting goes on.
        failures_.failed("enqueue", std::format("Failed to enqueue chunk {}", chunk.chunk_id));
    }
}

// Assigns speakers, commits the chunk atomically, then publishes it.
void MeetingEngine::on_chunk_result(const SpooledChunk& chunk,
                                    std::vector<TranscriptSegment> segments) {
    {
        // The refill reads bounded SQLite pages and never fills the live index: keep this chunk
        // available to diarization.
        const std::lock_guard index(index_mutex_);
        chunk_index_[chunk.chunk_id] = chunk;
        enqueued_chunk_ids_.insert(chunk.chunk_id);
    }
    try {
        assign_speakers(segments);
        failures_.recovered("assign_speakers");
    } catch (const std::exception&) {
        // Diarization is optional; the chunk still commits unlabeled.
        failures_.failed("assign_speakers", "Speaker assignment failed");
    }
    const meeting_state::ChunkCommit commit =
        deps_.repository->commit_chunk_transcription(meeting_id(), chunk.chunk_id, segments);
    if (!commit.committed) {
        return;
    }
    const double frontier = chunk.start_s + chunk.duration_s;
    {
        const std::lock_guard index(index_mutex_);
        double& value = preview_frontiers_[chunk.channel];
        value = std::max(value, frontier);
    }
    Json preview = Json::object();
    preview["type"] = "speech_preview";
    preview["channel"] = chunk.channel;
    preview["text"] = "";
    preview["start_s"] = chunk.start_s;
    preview["end_s"] = frontier;
    preview["final"] = true;
    broadcast(preview);
    observe_highlights(commit.rows, frontier);
    if (!commit.rows.empty()) {
        const Json items(commit.rows);
        emit("segments", Json{{"items", items}});
        broadcast(Json{{"type", "segments"}, {"items", items}});
        notify_intelligence_segments(static_cast<int>(commit.rows.size()));
        observe_voice_commands(commit.rows);
    }
}

// The desktop and the dashboard learn when durable ASR is catching up.
void MeetingEngine::on_asr_backlog_status(const std::string& message, bool overloaded) {
    const auto current = store();
    if (!current) {
        return;
    }
    Json speech = current->with_state([](const meeting_state::MeetingState& state) {
        return state.speech.is_object() ? state.speech : Json::object();
    });
    speech["backlog"] = overloaded;
    speech["message"] = message;
    if (current->update_runtime_fields(Json{{"speech", speech}})) {
        emit_status(message);
    }
}

// A saved but untranscribed chunk stays visible to the host.
void MeetingEngine::on_asr_chunk_failure(const std::string& message) {
    if (const auto current = store()) {
        Json speech = current->with_state([](const meeting_state::MeetingState& state) {
            return state.speech.is_object() ? state.speech : Json::object();
        });
        speech["chunk_error"] = message;
        speech["message"] = message;
        if (current->update_runtime_fields(Json{{"speech", speech}})) {
            emit_status(message);
        }
    }
    emit("error", Json{{"code", "asr_chunk_failed"},
                       {"message", message + " After fixing the audio file or speech engine, "
                                             "restart the app and retry this meeting from "
                                             "Recovery."}});
}

void MeetingEngine::on_speech_preview(const Json& preview) {
    const std::string channel = detail::str_or_empty(detail::get(preview, "channel"));
    double frontier = -1.0;
    {
        const std::lock_guard index(index_mutex_);
        if (const auto found = preview_frontiers_.find(channel);
            found != preview_frontiers_.end()) {
            frontier = found->second;
        }
    }
    const Json& end_value = detail::get(preview, "end_s");
    const double end_s = end_value.is_number() ? end_value.get<double>() : 0.0;
    if (active_.load() && !meeting_clock_.is_paused() && end_s > frontier) {
        Json message = Json::object();
        message["type"] = "speech_preview";
        message.update(preview);
        broadcast(message);
        observe_voice_preview(preview);
    }
}

void MeetingEngine::feed_preview(const CaptureBlock& block) noexcept {
    if (MeetingAsr* target = preview_target_.load()) {
        target->feed_preview(block, meeting_clock_.meeting_time(block.t_mono));
    }
}

} // namespace fv::meeting
