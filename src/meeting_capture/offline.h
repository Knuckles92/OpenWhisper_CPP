#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/cancel.h"
#include "base/json.h"
#include "engine/transcript.h"
#include "meeting_capture/spool.h"
#include "meeting_state/types.h"
#include "speech/service.h"

// meeting/asr/offline.py: the clean re-decode after End. Live capture uses short chunks for
// latency; this pass cuts each channel's continuous session audio at longer quiet gaps and
// decodes it window by window on the speech thread's Background lane, so dictation overtakes
// it. Any failed window fails the whole pass: a transcript with a missing middle must never pass
// for a complete replacement. Segment ids come from offline_segment_id (segment_ids.h);
// load_channel_session and session_origin_s are in session_audio.h.
namespace fv::meeting {

// Preferred window before quiet-cut scanning, the hard maximum, the quiet stretch required to
// cut (longer than the live 400 ms) and the overlap carried into the next window (none until
// dropping a spanning segment is proven never to lose speech).
inline constexpr double OFFLINE_TARGET_SEC = 15.0;
inline constexpr double OFFLINE_MAX_SEC = 25.0;
inline constexpr double OFFLINE_QUIET_WINDOW_S = 0.7;
inline constexpr double OFFLINE_OVERLAP_S = 0.0;

// Mic echo removal: acoustic delay plus decode jitter between the channels, the share of a mic
// segment's words that must also be heard on system audio, and the words a mic segment needs
// before it can be an echo ("yes", "okay, sure" are said on both sides at once).
inline constexpr double ECHO_TIME_SLACK_S = 1.0;
inline constexpr double ECHO_WORD_OVERLAP = 0.6;
inline constexpr std::size_t ECHO_MIN_WORDS = 3;

// One window failed: "Offline ASR failed for {channel} window {i}/{n} at {t:.1f}s".
class OfflineWindowError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct OfflineCutOptions {
    double target_sec = OFFLINE_TARGET_SEC;
    double max_sec = OFFLINE_MAX_SEC;
    double quiet_window_s = OFFLINE_QUIET_WINDOW_S;
    double overlap_s = OFFLINE_OVERLAP_S;
    double quiet_rms = QUIET_RMS;
};

// [start, end) sample ranges covering [0, sample_count): each cut where find_cut_point cuts
// `audio` from the range's start (a hard cut at max_sec without audio or without a cut), every
// range at least one sample, the next starting overlap_s before the previous end.
std::vector<std::pair<std::int64_t, std::int64_t>>
offline_cut_ranges(std::int64_t sample_count, int sample_rate,
                   std::optional<std::span<const std::int16_t>> audio,
                   const OfflineCutOptions& options = {});

// The segments that end after keep_from_s: the later window owns the overlap, and a segment
// spanning keep_from_s is kept.
std::vector<meeting_state::TranscriptSegment>
drop_overlapped_prefix(std::span<const meeting_state::TranscriptSegment> segments,
                       double keep_from_s);

// Drops each mic segment of at least ECHO_MIN_WORDS words (\w+ of its lower-cased text) of which
// at least ECHO_WORD_OVERLAP also occur in the loopback segments overlapping it within
// ECHO_TIME_SLACK_S: without headphones the microphone re-records remote speakers, and system
// audio is the clean copy. Order is kept.
std::vector<meeting_state::TranscriptSegment>
drop_mic_echo(std::vector<meeting_state::TranscriptSegment> segments);

// (detail, current window, windows): "{Microphone|System Audio}: decoding window {i}/{n}
// ({a:.1f}s - {b:.1f}s)". A callback that throws is logged and ignored.
using OfflineProgress = std::function<void(const std::string& detail, int current, int total)>;

// Decodes one window of 16 kHz float audio; segment times are seconds from its start. Throws to
// fail the pass.
using WindowDecoder = std::function<std::vector<Segment>(std::vector<float> audio)>;

// The live engine's window decoder: one Background-lane job per window on the given slot, with
// `language` ("auto" or a code; never empty, or the dictation language would be used) and the
// cancel token. Blocks the calling thread, which must not be the GUI or the speech thread.
WindowDecoder background_window_decoder(speech::SpeechService& service, std::string language,
                                        CancelToken cancel,
                                        speech::Slot slot = speech::Slot::Meeting);

struct SessionAudioOptions {
    std::string meeting_id;
    std::string channel;
    double origin_s = 0.0; // meeting time of sample 0
    double target_sec = OFFLINE_TARGET_SEC;
    double max_sec = OFFLINE_MAX_SEC;
    double overlap_s = OFFLINE_OVERLAP_S;
    OfflineProgress progress;
};

// transcribe_session_audio: cuts `frames` with offline_cut_ranges and decodes the windows in
// order. Segments with empty text are dropped but keep their ordinal; each window drops its
// overlap prefix, and once it keeps a segment the earlier segments starting at or after its
// origin go. Throws OfflineWindowError when a window's decode throws.
std::vector<meeting_state::TranscriptSegment>
transcribe_session_audio(const WindowDecoder& decode, std::span<const std::int16_t> frames,
                         int sample_rate, const SessionAudioOptions& options);

// One channel's session audio to segments (the injectable decoder of the meeting pass).
using SessionTranscriber = std::function<std::vector<meeting_state::TranscriptSegment>(
    std::span<const std::int16_t> frames, int sample_rate, const std::string& channel,
    double origin_s, const OfflineProgress& progress)>;

struct MeetingSessionsOptions {
    std::vector<std::string> channels = {std::string(meeting_state::kChannelMic),
                                         std::string(meeting_state::kChannelLoopback)};
    // Replaces transcribe_session_audio (tests); anything it throws reaches the caller.
    SessionTranscriber transcribe;
    OfflineProgress progress;
};

// transcribe_meeting_sessions: every channel's session audio (load_channel_session, stitched
// from `chunks` when the session WAV is missing) decoded, mic echoes dropped, and the result
// sorted by (start_s, channel, segment_id). A channel is skipped when the spool folder is missing
// and there are no chunk rows, or when it has no audio.
std::vector<meeting_state::TranscriptSegment>
transcribe_meeting_sessions(const WindowDecoder& decode, const std::filesystem::path& spool_dir,
                            const std::string& meeting_id, const std::vector<Json>& chunks,
                            const MeetingSessionsOptions& options = {});

} // namespace fv::meeting
