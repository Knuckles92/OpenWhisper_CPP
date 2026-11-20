// MeetingEngine: speaker labels on live chunks and the diarizer's lifecycle (meeting/engine.py
// _assign_speakers, _label_mic_segments, _start_diarizer_background, _start_diarizer,
// _check_diarizer_degraded, _on_diarizer_relabel).

#include <algorithm>
#include <exception>
#include <format>
#include <utility>

#include "base/utf8.h"
#include "diarize/assign.h"
#include "meeting/detail/engine_internal.h"
#include "meeting/engine.h"
#include "meeting_capture/asr_audio.h"

namespace fv::meeting {

namespace {

const log::Logger& logger = detail::engine_logger;

using meeting_state::TranscriptSegment;

diarize::Segment view_of(const TranscriptSegment& segment) {
    diarize::Segment view;
    view.id = segment.segment_id;
    view.start_s = segment.start_s;
    view.end_s = segment.end_s;
    view.speaker_participant_id = segment.speaker_participant_id;
    view.speaker_source = segment.speaker_source;
    return view;
}

void copy_back(const diarize::Segment& view, TranscriptSegment& segment) {
    segment.speaker_participant_id = view.speaker_participant_id;
    segment.speaker_source = view.speaker_source;
    if (!view.embedding.empty()) {
        std::vector<std::uint8_t> bytes(view.embedding.size());
        std::transform(view.embedding.begin(), view.embedding.end(), bytes.begin(),
                       [](std::byte value) { return static_cast<std::uint8_t>(value); });
        segment.embedding = std::move(bytes);
    }
}

} // namespace

// Microphone segments become "Me"; system-audio segments go to the diarizer chunk by chunk.
void MeetingEngine::assign_speakers(std::vector<TranscriptSegment>& segments) {
    const std::shared_ptr<diarize::Diarizer> current = diarizer();
    const auto by_chunk = label_mic_segments(segments, current != nullptr);
    if (by_chunk.empty()) {
        return;
    }
    const auto on_unlabeled = [this](const diarize::Segment&) {
        if (!degraded_diarization_.load()) {
            check_diarizer_degraded();
        }
    };
    std::vector<diarize::Segment> labeled;
    std::vector<std::size_t> labeled_index;
    for (const auto& [chunk_id, indexes] : by_chunk) {
        std::optional<meeting_state::SpooledChunk> chunk;
        if (chunk_id) {
            const std::lock_guard index(index_mutex_);
            if (const auto found = chunk_index_.find(*chunk_id); found != chunk_index_.end()) {
                chunk = found->second;
            }
        }
        if (!chunk) {
            continue;
        }
        std::vector<std::int16_t> frames;
        int rate = 0;
        try {
            std::tie(frames, rate) = load_wav_int16(path_from_utf8(chunk->file_path));
            failures_.recovered("reload_chunk");
        } catch (const std::exception&) {
            // A missing, truncated or non-16-bit file: this chunk stays unlabeled.
            failures_.failed("reload_chunk",
                             std::format("Failed to reload chunk {} for diarization", *chunk_id));
            continue;
        }
        std::vector<diarize::Segment> views;
        views.reserve(indexes.size());
        for (const std::size_t index : indexes) {
            views.push_back(view_of(segments[index]));
        }
        const std::vector<std::string> ids = diarize::assign_from_frames(
            *current, views, frames, rate, chunk->start_s, on_unlabeled);
        for (std::size_t i = 0; i < views.size(); ++i) {
            copy_back(views[i], segments[indexes[i]]);
            if (std::find(ids.begin(), ids.end(), views[i].id) != ids.end()) {
                labeled.push_back(views[i]);
                labeled_index.push_back(indexes[i]);
            }
        }
    }
    diarize::refresh_labels(*current, labeled);
    for (std::size_t i = 0; i < labeled.size(); ++i) {
        copy_back(labeled[i], segments[labeled_index[i]]);
    }
}

std::vector<std::pair<std::optional<std::int64_t>, std::vector<std::size_t>>>
MeetingEngine::label_mic_segments(std::vector<TranscriptSegment>& segments, bool have_diarizer) {
    std::optional<std::string> me;
    {
        const std::lock_guard lock(state_mutex_);
        me = me_participant_id_;
    }
    // By chunk, in the order the chunks first appear (Python's dict).
    std::vector<std::pair<std::optional<std::int64_t>, std::vector<std::size_t>>> by_chunk;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        TranscriptSegment& segment = segments[i];
        if (segment.channel == meeting_state::kChannelMic) {
            segment.speaker_participant_id = me;
            segment.speaker_source = "channel";
        } else if (segment.channel == meeting_state::kChannelLoopback && have_diarizer) {
            const auto found =
                std::find_if(by_chunk.begin(), by_chunk.end(), [&segment](const auto& entry) {
                    return entry.first == segment.chunk_id;
                });
            if (found == by_chunk.end()) {
                by_chunk.push_back({segment.chunk_id, {i}});
            } else {
                found->second.push_back(i);
            }
        }
    }
    return by_chunk;
}

// The speaker model may take minutes to download: it never sits on the startup path.
void MeetingEngine::start_diarizer_background() {
    if (options_.speaker_id_backend == "off") {
        disable_diarizer();
        return;
    }
    const std::lock_guard lock(lifecycle_mutex_);
    if (processing_stop_.load()) {
        return;
    }
    diarizer_init_.done = false;
    diarizer_init_.started = true;
    diarizer_init_.thread = std::thread([this] {
        detail::name_engine_thread("fv-meeting-diarizer-start");
        diarizer_init_worker();
    });
    diarizer_init_.id = diarizer_init_.thread.get_id();
}

void MeetingEngine::diarizer_init_worker() {
    try {
        start_diarizer();
    } catch (const std::exception& error) {
        if (!processing_stop_.load()) {
            logger.error_with(error, "Speaker separation initialization failed");
        } else {
            logger.debug("Speaker separation initialization stopped by End; ignoring its error: "
                         "{}",
                         error.what());
        }
    }
    if (!processing_stop_.load()) {
        // Readiness landed on its own thread: tell the dashboard.
        emit_status();
    }
    set_worker_done(diarizer_init_);
}

void MeetingEngine::start_diarizer() {
    if (options_.speaker_id_backend == "off") {
        disable_diarizer();
        return;
    }
    degraded_diarization_ = false;
    std::string reason;
    try {
        resolve_diarization_model();
    } catch (const std::exception& error) {
        logger.warning("Speaker model unavailable: {}", error.what());
        reason = error.what();
        if (reason.empty()) {
            reason = std::string(DIARIZATION_NO_MODEL);
        }
    }
    if (processing_stop_.load()) {
        return;
    }
    std::shared_ptr<diarize::Diarizer> created;
    if (reason.empty()) {
        created = create_diarizer();
    }
    const bool available = prepare_diarizer(created.get());
    if (!available && reason.empty()) {
        bool have_path = false;
        {
            const std::lock_guard lock(state_mutex_);
            have_path = diarization_model_path_.has_value() && !diarization_model_path_->empty();
        }
        reason = std::string(have_path ? DIARIZATION_LOAD_FAILED : DIARIZATION_NO_MODEL);
    }
    {
        // The probe can block, and End may have claimed the meeting meanwhile: adoption and its
        // status write are one guarded transition after it.
        const std::lock_guard lock(lifecycle_mutex_);
        if (processing_stop_.load()) {
            return;
        }
        {
            const std::lock_guard state(state_mutex_);
            diarizer_ = available ? created : nullptr;
        }
        if (!available) {
            logger.info("Diarization disabled; loopback stays channel-labeled");
        }
        if (const auto current = store()) {
            current->update_runtime_fields(
                Json{{"diarization_available", available}, {"diarization_reason", reason}});
        }
    }
    emit_status();
}

std::unique_ptr<diarize::Diarizer> MeetingEngine::create_diarizer() {
    std::optional<std::filesystem::path> model;
    {
        const std::lock_guard lock(state_mutex_);
        model = diarization_model_path_;
    }
    try {
        return deps_.diarizer(model, options_.onnxruntime_path, diarizer_host_, meeting_id());
    } catch (const std::exception& error) {
        logger.error_with(error, "Diarizer unavailable");
        return nullptr;
    }
}

void MeetingEngine::disable_diarizer() {
    {
        const std::lock_guard lock(lifecycle_mutex_);
        if (processing_stop_.load()) {
            return;
        }
        degraded_diarization_ = false;
        {
            const std::lock_guard state(state_mutex_);
            diarizer_.reset();
        }
        if (const auto current = store()) {
            current->update_runtime_fields(
                Json{{"diarization_available", false},
                     {"diarization_reason", std::string(DIARIZATION_OFF)}});
        }
    }
    logger.info("Speaker identification off; loopback stays channel-labeled");
}

// Fills in the speaker model path when needed; throws when none can be had.
void MeetingEngine::resolve_diarization_model() {
    {
        const std::lock_guard lock(state_mutex_);
        if ((diarization_model_path_ && !diarization_model_path_->empty()) ||
            !options_.diarization_model_resolver) {
            return;
        }
    }
    std::optional<std::filesystem::path> path = options_.diarization_model_resolver();
    if (!processing_stop_.load()) {
        const std::lock_guard lock(state_mutex_);
        diarization_model_path_ = std::move(path);
    }
}

// Probes availability; true when the diarizer can be used. Relabels reach the engine through
// the diarizer host it was built with.
bool MeetingEngine::prepare_diarizer(diarize::Diarizer* candidate) {
    if (candidate == nullptr) {
        return false;
    }
    try {
        return candidate->is_available();
    } catch (const std::exception& error) {
        logger.error_with(error, "Diarizer availability probe failed");
        return false;
    }
}

// Called only after assign declined to label a segment. A bare "no label" is normal for a
// segment too short to embed, so the availability probe tells a quiet segment from a diarizer
// that failed mid-meeting. The diarizer is kept, so human speaker corrections still reach pin.
void MeetingEngine::check_diarizer_degraded() {
    const auto current = diarizer();
    if (!current || degraded_diarization_.load()) {
        return;
    }
    try {
        if (current->is_available()) {
            return;
        }
    } catch (const std::exception& error) {
        logger.error_with(error, "Diarizer availability probe failed");
    }
    degraded_diarization_ = true;
    if (const auto state = store()) {
        state->update_runtime_fields(
            Json{{"diarization_available", false},
                 {"diarization_reason", std::string(DIARIZATION_DEGRADED)}});
    }
    logger.warning("Diarization degraded mid-meeting; loopback segments fall back to "
                   "channel-level labels");
    emit_status();
}

// Re-cluster relabels go through the single-writer store, which refuses segments still in flight
// (assigned but not committed with their chunk); the ASR path refreshes those from the diarizer
// before the commit.
void MeetingEngine::on_diarizer_relabel(const diarize::RelabelOps& ops) {
    const auto current = store();
    if (!current || ops.empty()) {
        return;
    }
    Json batch = Json::array();
    for (const auto& [segment_id, participant_id] : ops) {
        batch.push_back(Json{{"op", "reassign_segment_speaker"},
                             {"segment_id", segment_id},
                             {"participant_id", participant_id}});
    }
    try {
        current->apply("system", std::string("diarizer"), batch);
        failures_.recovered("diarizer_relabel");
    } catch (const std::exception&) {
        failures_.failed("diarizer_relabel", "Diarizer relabel batch failed");
    }
}

} // namespace fv::meeting
