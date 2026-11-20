#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "speech/service.h"

// benchmarks/live_preview.py: measures the dictation preview (speech::WindowPreview, the
// production code, never a copy) on a corpus of recordings with reference text. Replay does not
// sleep: measured service times drive a serial virtual clock. Visibility excludes recorder
// queues, dropped blocks and paint latency. Only the window path is ported: Parakeet TDT is
// offline-only, so the native streaming path (Nemotron, Moonshine) has nothing to measure.
namespace fv::bench {

// config.SAMPLE_RATE and CHUNK_SIZE: the recorder's rate and callback size, which quantise a
// window's boundaries.
inline constexpr int kRecorderRate = 44100;
inline constexpr int kCallbackFrames = 1024;

// block_ends: the frame counts at which a window of `cadence` seconds completes. Recorder
// callbacks are indivisible, so 3 s at 44.1 kHz takes 130 x 1024 frames.
std::vector<std::int64_t> block_ends(std::int64_t length, int sample_rate, double cadence,
                                     int block_frames = kCallbackFrames);

// score: {substitutions, deletions, insertions, words, errors, wer}, wer null for a silent
// reference (its insertions still count).
Json preview_score(std::string_view reference, std::string_view hypothesis);

struct ReplayOptions {
    double cadence = 3.0;  // STREAMING_CHUNK_DURATION_SEC
    double overlap = 0.75; // STREAMING_OVERLAP_SEC
    int sample_rate = kRecorderRate;
    int block_frames = kCallbackFrames;
    // time.perf_counter: read once before and once after each window. Steady time by default.
    std::function<double()> perf_counter;
    // A timed clip's words [{text, end_s}]: adds checkpoints of prefix WER every 3 s.
    std::optional<Json> timed_words;
    double checkpoint_cadence = 3.0;
};

// replay: feeds `audio` (mono int16 at sample_rate) to a WindowPreview on a fresh PcmTape in
// callback-sized blocks, up to each window's end, and waits for that window; the remainder after
// the last full window is decoded by stop() and finalize(), as a dictation's empty final does.
// Returns {duration_s, calls, updates, live_text, drained_text, live, drained,
// first_text_before_stop_s, drain_after_stop_s[, checkpoints]}: live is the text visible at the
// end of the audio (post-stop text never counts), drained the text after finalize. Each call
// records {audio_end_s, service_s, visible_s, backlog_s, flush}. `service` must have the
// dictation engine Ready. A window the preview did not count (production logs and swallows a
// failed decode) throws std::runtime_error("Preview decode failed; inspect inference log"), so a
// failure is never scored as silence.
Json replay(speech::SpeechService& service, std::span<const std::int16_t> audio,
            std::string_view reference, const ReplayOptions& options = {});

// numpy.percentile with linear interpolation; nullopt for no values.
std::optional<double> percentile(std::vector<double> values, double p);

// aggregate: over the clips' replay results; errors are summed and divided by the summed words
// (word-weighted, never a mean of clip WERs). Flush calls are not updates.
Json aggregate(const std::vector<Json>& clips, double cadence);

// The validated settings of `fiorivoce-bench live-preview` (parse_args): repeats and limit
// positive, chunk and overlap finite, chunk positive and overlap in [0, chunk).
struct LivePreviewArgs {
    std::filesystem::path manifest;
    std::filesystem::path output = "out/perf/bench-live-preview.json";
    double chunk = 3.0;
    double overlap = 0.75;
    int repeats = 3;
    std::optional<int> limit;
    std::optional<std::string> runtime;
    std::optional<std::string> model;
    std::string device = "cpu";
};

// One corpus clip: {audio_path, reference[, id][, group][, words]} relative to the manifest.
struct PreviewClip {
    Json item;
    std::vector<std::int16_t> audio;
    std::string sha256;
};

// load_clips: the manifest and its first `limit` clips, each decoded with FFmpeg, channels
// averaged and resampled to 44.1 kHz int16 (the recorder's PCM). Throws std::invalid_argument
// "Manifest contains no clips" or "Empty audio: <path>".
std::pair<Json, std::vector<PreviewClip>> load_clips(const std::filesystem::path& manifest,
                                                     std::optional<int> limit);

// The benchmark over loaded clips on a ready service: a warm-up replay of the first clip, then
// `repeats` passes. Returns the report's metrics: {settings, corpus, passes: [{repeat,
// warmup_s, clips, summary, groups}], summary, groups} (summary and groups over every pass).
Json run_live_preview(speech::SpeechService& service, const Json& manifest,
                      const std::vector<PreviewClip>& clips, const LivePreviewArgs& args,
                      const std::function<void(const std::string&)>& progress = {});

// scripts/report_live_preview.py summarize: pools `fiorivoce-bench live-preview` reports of one
// corpus and setup while keeping groups and exact counts. Throws std::invalid_argument "At least
// one report is required", "Cannot pool runs with different <settings field, corpus or
// source>", "Cannot pool runs with different hardware <platform, machine or cpu_count>" and
// "Cannot pool parakeet-v3 with different <runtime, model, device_info or effective cadence>":
// numbers from another engine build, model file or machine never mix. A report whose run failed
// is listed under failures. Returns {settings, corpus, raw_reports, failures, passes,
// effective_cadence_s, summary, groups, per_pass, load_s, runtime, model, device_info,
// clip_scores}.
Json pool_live_preview_reports(const std::vector<Json>& reports,
                               const std::vector<std::string>& names = {});

// markdown(): the pooled summary's tables.
std::string live_preview_markdown(const Json& pooled);

} // namespace fv::bench
