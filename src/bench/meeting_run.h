#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "base/json.h"
#include "bench/ami.h"
#include "meeting_capture/offline.h"

// benchmarks/meeting_mode/run.py: real AMI meetings replayed offline through the meeting ASR
// path: the spool's quiet-aligned chunk cuts (find_cut_point), resample_to_16k, the digital-
// silence skip and chunk_segments for the live draft, then the post-meeting clean re-decode
// (transcribe_session_audio). Whisper's draft prompts, beam sizes and stored-live corrections
// have no Parakeet counterpart: the stored live transcript is the draft itself.
namespace fv::bench {

inline constexpr int kMeetingBenchmarkVersion = 1;

// A deliberately demanding release gate: at least a full workday of natural meetings, strict
// WER (overlap and fillers included) at most 30 %, no meeting above 35 %, and ample live
// headroom.
inline constexpr std::size_t MIN_GATE_MEETINGS = 10;
inline constexpr double MIN_GATE_AUDIO_HOURS = 8.0;
inline constexpr double MAX_GATE_MICRO_WER = 0.30;
inline constexpr double MAX_GATE_MEETING_WER = 0.35;
inline constexpr double MAX_GATE_RTF = 0.50;

// The command line of `fiorivoce-bench meeting`. The defaults are the production meeting
// profile: the spool's chunk target and maximum (meeting_capture DEFAULT_TARGET_SEC and
// DEFAULT_MAX_SEC) and the offline pass on.
struct MeetingArgs {
    std::filesystem::path data_dir = "out/bench/ami";
    std::filesystem::path results_dir = "out/bench/meeting";
    std::string language = "auto";
    std::vector<std::string> meetings;
    std::optional<int> max_meetings;
    double target_sec;
    double max_sec;
    bool offline_pass = true;
    bool force = false;
    std::optional<std::string> runtime;
    std::optional<std::string> model;
    std::string device = "cpu";
    std::optional<std::filesystem::path> output;

    MeetingArgs();
};

// _chunk_ranges: the [start, end) native-sample ranges the live spool writer cuts, a range
// shorter than MIN_FLUSH_S ending the meeting.
std::vector<std::pair<std::int64_t, std::int64_t>> chunk_ranges(std::span<const std::int16_t> audio,
                                                                int sample_rate, double target_sec,
                                                                double max_sec);

// The decoders a replay uses: the live meeting lane's for draft chunks and the offline pass's
// for session windows. Each takes 16 kHz float audio and returns segments from its start.
struct MeetingDecoders {
    meeting::WindowDecoder draft;
    meeting::WindowDecoder offline;
};

// decode_meeting: one meeting's WAV (16-bit PCM, channels averaged) replayed. Returns {meeting_id,
// description, audio_path, duration_s, source_rate, chunks, target_sec, max_sec, elapsed_s,
// rtf, offline_elapsed_s, offline_rtf, combined_rtf, draft_segments, final_segments,
// offline_segments, run_offline}; segments are {id, start_s, end_s, text, channel}. Throws
// std::runtime_error("Required offline ASR model is unavailable") when the offline pass is on
// without an offline decoder.
Json decode_meeting(const MeetingDecoders& decoders, const MeetingSpec& spec,
                    const std::filesystem::path& source_path, double target_sec, double max_sec,
                    bool run_offline, const std::function<void(const std::string&)>& progress = {});

// _score_result: adds reference {words, first_s, last_s, overlap stats} and the tcWER of the
// draft, final and offline segments; offline_score is null when the offline pass was off, and
// an empty offline transcript scores as deletions.
Json score_result(Json result, const std::filesystem::path& annotations_dir);

// _summary: totals, the three aggregates and the quality gate, whose product is the offline
// score when the offline pass ran, else the final one. Throws std::invalid_argument("Required
// offline score is missing") when the pass ran and a meeting has no offline score: a draft
// score never stands in.
Json meeting_summary(std::span<const Json> results, std::string_view model,
                     std::string_view language, double target_sec, double max_sec,
                     bool run_offline);

// _write_summary: summary.json and summary.md in `dir`.
void write_meeting_summary(const std::filesystem::path& dir, const Json& summary);

} // namespace fv::bench
