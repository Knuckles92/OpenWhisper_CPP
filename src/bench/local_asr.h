#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "base/json.h"
#include "process/process_env.h"

// scripts/benchmark_local_asr.py and benchmark_local_asr_corpus.py: the installed Parakeet
// engine on audio files, never downloading. FioriVoce has one local engine, so OpenWhisper's
// model matrix (Whisper base and turbo, the optional engines) is one row.
namespace fv::bench {

// What a run needs of a loaded engine. transcribe throws to fail the row; cleanup runs once,
// whether the row failed or not.
struct AsrBackend {
    std::string model = "parakeet-v3";
    std::string device;
    std::string device_info;
    Json identity = nullptr;
    std::function<std::string(const std::filesystem::path&)> transcribe;
    std::function<void()> cleanup;
};
// Loads the engine (timed as load_s); throws with the reason when it cannot.
using AsrBackendLoader = std::function<AsrBackend()>;

// The audio's length in seconds, decoded with FFmpeg (decode_audio at 16 kHz).
double audio_duration_s(const std::filesystem::path& path);

// word_error_rate: score_text's WER (null for a silent reference).
std::optional<double> word_error_rate(std::string_view reference, std::string_view hypothesis);

struct AsrArgs {
    std::vector<std::filesystem::path> audio;
    // The reference transcript of each audio file, in order (fewer means the rest have none).
    std::vector<std::filesystem::path> references;
    int runs = 3;
    std::string requested_device = "cpu";
};

// benchmark_local_asr: loads once, then per file one cold decode and `runs` warm ones.
// Returns the report's metrics {load_s, requested_device, actual_device, device_info,
// model_identity, peak_process_rss_mb, files: [{audio, duration_s, first_decode_s, warm_s,
// warm_median_s, warm_p95_s, real_time_factor, transcript[, score, normalized_wer]}][, error]}
// and whether it failed (`error` set). Throws std::invalid_argument for runs < 1 or an audio
// file of no length.
struct AsrRun {
    Json metrics;
    bool failed = false;
};
AsrRun run_asr(const AsrArgs& args, const AsrBackendLoader& load);

// benchmark_local_asr_corpus: the clips of a JSON manifest ({clips: [{audio_path, reference[,
// group][, id]}], source}) decoded once each after a warm-up decode, scored per clip and summed
// per group; writes {source, normalization, provenance, results: [row]} to `output` and returns
// 1 when the row failed, else 0. HF_HUB_OFFLINE is 1 for the run (model lookups never download)
// and restored afterwards, also when the manifest is invalid (its parse error propagates).
// Throws std::invalid_argument("Corpus must contain nonempty audio clips").
struct CorpusArgs {
    std::filesystem::path manifest;
    std::filesystem::path output;
    std::string requested_device = "cpu";
    std::optional<std::filesystem::path> source_root;
};
int run_asr_corpus(
    const CorpusArgs& args, const AsrBackendLoader& load, MutableEnv& env,
    const std::function<double(const std::filesystem::path&)>& duration_of = audio_duration_s);

} // namespace fv::bench
