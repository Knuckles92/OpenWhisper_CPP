#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"

// meeting/interfaces.py: the value types that cross Meeting Mode's subsystems (capture, ASR,
// state store, persistence).
// Affinity: values; thread-compatible.
namespace fv::meeting_state {

using Json = fv::Json;

// The two capture streams.
inline constexpr std::string_view kChannelMic = "mic";
inline constexpr std::string_view kChannelLoopback = "loopback";
inline constexpr std::array<std::string_view, 2> kChannels = {kChannelMic, kChannelLoopback};

// A finalized on-disk audio chunk registered for transcription (a meeting_audio_chunks row).
struct SpooledChunk {
    std::int64_t chunk_id = 0;
    std::string meeting_id;
    std::string channel;   // kChannelMic or kChannelLoopback
    std::int64_t seq = 0;  // per (meeting, channel)
    std::string file_path; // UTF-8, the 16 kHz mono int16 WAV file
    double start_s = 0.0;  // meeting-clock offset of the first sample
    double duration_s = 0.0;
    int sample_rate = 0; // Hz, of the WAV file (16000)

    friend bool operator==(const SpooledChunk&, const SpooledChunk&) = default;
};

// A timestamped transcript segment from the ASR engine. start_s and end_s are meeting-clock
// seconds, so segment ids are stable evidence anchors.
struct TranscriptSegment {
    std::string segment_id;
    std::string meeting_id;
    std::optional<std::int64_t> chunk_id;
    std::string channel;
    double start_s = 0.0;
    double end_s = 0.0;
    std::string text;
    std::optional<std::string> speaker_participant_id;
    std::string speaker_source = "channel"; // channel | diarizer | human
    bool speaker_pinned = false;
    std::optional<std::vector<std::uint8_t>> embedding; // float32 speaker-embedding bytes

    friend bool operator==(const TranscriptSegment&, const TranscriptSegment&) = default;
};

// The outcome of one state-patch op. `op` is the op as submitted (an object, normalized by its
// handler); `effect` and `inverse` are null where Python's are None. `seq` is set by the store
// for an applied op and stays empty for a rejected one.
struct OpResult {
    bool ok = false;
    Json op = Json::object();
    std::optional<std::string> reason;    // rejection reason ("human_edited", "unknown_op", ...)
    std::optional<std::string> target_id; // the item, question, participant or segment
    Json effect;                          // the post-apply entity, for broadcast
    Json inverse;                         // the op that undoes this one, when derivable
    std::optional<std::int64_t> current_revision; // echoed on revision_mismatch
    std::optional<std::int64_t> seq;

    // Every field in the dataclass's order: ok, op, reason, target_id, effect, inverse,
    // current_revision, seq (null for an empty optional).
    Json to_json() const;
};

} // namespace fv::meeting_state
