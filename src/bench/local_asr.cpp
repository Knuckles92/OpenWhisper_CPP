#include "bench/local_asr.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <map>
#include <stdexcept>

#include "audio/file_decoder.h"
#include "base/utf8.h"
#include "bench/detail/peak_memory.h"
#include "bench/live_preview.h"
#include "bench/meeting_metrics.h"
#include "bench/provenance.h"

namespace fv::bench {

namespace {

constexpr std::string_view kOfflineVariable = "HF_HUB_OFFLINE";

double seconds_since(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

double median(std::vector<double> values) {
    return percentile(std::move(values), 50).value_or(0.0);
}

Json rss_mb() {
    const auto bytes = detail::peak_resident_bytes();
    return bytes ? Json(static_cast<double>(*bytes) / 1'000'000.0) : Json(nullptr);
}

// Restores a variable when the run ends, however it ends.
class ScopedVariable {
public:
    ScopedVariable(MutableEnv& env, std::string_view name, std::string_view value)
        : env_(env), name_(name), previous_(env.get(name)) {
        env_.set(name_, value);
    }
    ~ScopedVariable() {
        if (previous_) {
            env_.set(name_, *previous_);
        } else {
            env_.unset(name_);
        }
    }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;

private:
    MutableEnv& env_;
    std::string name_;
    std::optional<std::string> previous_;
};

} // namespace

double audio_duration_s(const std::filesystem::path& path) {
    audio::AudioFileDecoder decoder(path);
    std::vector<std::int16_t> samples;
    while (decoder.read_s16(samples)) {
    }
    const int channels = std::max(1, decoder.channels());
    return decoder.rate() > 0
               ? static_cast<double>(samples.size() / static_cast<std::size_t>(channels)) /
                     decoder.rate()
               : 0.0;
}

std::optional<double> word_error_rate(std::string_view reference, std::string_view hypothesis) {
    return score_text(reference, hypothesis).wer;
}

AsrRun run_asr(const AsrArgs& args, const AsrBackendLoader& load) {
    if (args.runs < 1) {
        throw std::invalid_argument("--runs must be at least 1");
    }
    std::vector<double> durations;
    for (const auto& path : args.audio) {
        const double duration = audio_duration_s(path);
        if (duration <= 0) {
            throw std::invalid_argument("Audio duration and repeat count must be positive");
        }
        durations.push_back(duration);
    }
    AsrRun run;
    run.metrics = Json{{"model", "parakeet-v3"}, {"requested_device", args.requested_device}};
    std::optional<AsrBackend> backend;
    try {
        const auto started = std::chrono::steady_clock::now();
        backend = load();
        run.metrics["load_s"] = seconds_since(started);
        run.metrics["device_info"] = backend->device_info;
        run.metrics["actual_device"] = backend->device;
        if (backend->device != args.requested_device) {
            throw std::runtime_error("Requested " + args.requested_device + ", but loaded " +
                                     backend->device + ": " + backend->device_info);
        }
        run.metrics["model_identity"] = backend->identity;
        Json files = Json::array();
        for (std::size_t index = 0; index < args.audio.size(); ++index) {
            const auto& path = args.audio[index];
            std::vector<double> timings;
            std::string transcript;
            for (int i = 0; i <= args.runs; ++i) {
                const auto started_decode = std::chrono::steady_clock::now();
                transcript = backend->transcribe(path);
                timings.push_back(seconds_since(started_decode));
            }
            const std::vector<double> warm(timings.begin() + 1, timings.end());
            Json row{{"audio", path_to_utf8(path)},
                     {"duration_s", durations[index]},
                     {"first_decode_s", timings.front()},
                     {"warm_s", warm},
                     {"warm_median_s", median(warm)},
                     {"warm_p95_s", percentile(warm, 95).value_or(0.0)},
                     {"real_time_factor", median(warm) / durations[index]},
                     {"transcript", transcript}};
            if (index < args.references.size()) {
                const std::string reference = read_text_file(args.references[index]);
                const TextScore score = score_text(reference, transcript);
                row["score"] = score.to_json();
                row["normalized_wer"] = score.wer ? Json(*score.wer) : Json(nullptr);
            }
            files.push_back(std::move(row));
        }
        run.metrics["files"] = std::move(files);
    } catch (const std::exception& error) {
        run.metrics["error"] = error.what();
        run.failed = true;
    }
    if (backend && backend->cleanup) {
        backend->cleanup();
    }
    run.metrics["peak_process_rss_mb"] = rss_mb();
    return run;
}

int run_asr_corpus(const CorpusArgs& args, const AsrBackendLoader& load, MutableEnv& env,
                   const std::function<double(const std::filesystem::path&)>& duration_of) {
    // OpenWhisper_CPP's model lookups honour HF_HUB_OFFLINE as OpenWhisper's did: a corpus run
    // never downloads, and the caller's own setting comes back afterwards.
    const ScopedVariable offline(env, kOfflineVariable, "1");
    const Json manifest = read_json(args.manifest);
    struct Clip {
        Json item;
        std::filesystem::path path;
        double duration_s;
    };
    std::vector<Clip> clips;
    for (const Json& item : manifest.at("clips")) {
        const auto path = std::filesystem::weakly_canonical(
            args.manifest.parent_path() / path_from_utf8(item.at("audio_path").get<std::string>()));
        clips.push_back(Clip{item, path, duration_of(path)});
    }
    if (clips.empty() || std::any_of(clips.begin(), clips.end(),
                                     [](const Clip& clip) { return clip.duration_s <= 0; })) {
        throw std::invalid_argument("Corpus must contain nonempty audio clips");
    }
    IdentityRequest request;
    request.source_root = args.source_root;
    request.inputs.push_back(args.manifest);
    for (const Clip& clip : clips) {
        request.inputs.push_back(clip.path);
    }
    Json report{{"source", manifest.value("source", Json(nullptr))},
                {"normalization", "NFKC lowercase ordered words; punctuation and AMI acronym "
                                  "separators ignored"},
                {"provenance", identity(request)},
                {"results", Json::array()}};
    Json row{{"model", "parakeet-v3"},
             {"requested_device", args.requested_device},
             {"clips", Json::array()},
             {"groups", Json::object()}};
    std::optional<AsrBackend> backend;
    try {
        const auto started = std::chrono::steady_clock::now();
        backend = load();
        row["load_s"] = seconds_since(started);
        row["actual_device"] = backend->device;
        if (backend->device != args.requested_device) {
            throw std::runtime_error(backend->device_info);
        }
        row["model_identity"] = backend->identity;
        backend->transcribe(clips.front().path);
        constexpr const char* kCounts[] = {"words", "errors", "substitutions", "deletions",
                                           "insertions"};
        for (const Clip& clip : clips) {
            const auto started_decode = std::chrono::steady_clock::now();
            const std::string transcript = backend->transcribe(clip.path);
            const double elapsed = seconds_since(started_decode);
            const TextScore score =
                score_text(clip.item.at("reference").get<std::string>(), transcript);
            const Json scored = score.to_json();
            const std::string group = clip.item.value("group", std::string("all"));
            Json& totals = row["groups"][group];
            if (totals.is_null()) {
                for (const char* key : kCounts) {
                    totals[key] = 0;
                }
                totals["decode_s"] = 0;
                totals["audio_s"] = 0;
            }
            Json entry{{"id", clip.item.contains("id") ? clip.item["id"] : clip.item["audio_path"]},
                       {"group", group},
                       {"transcript", transcript},
                       {"score", scored}};
            for (const char* key : kCounts) {
                totals[key] = totals[key].get<std::size_t>() + scored[key].get<std::size_t>();
                entry[key] = scored[key];
            }
            totals["decode_s"] = totals["decode_s"].get<double>() + elapsed;
            totals["audio_s"] = totals["audio_s"].get<double>() + clip.duration_s;
            entry["decode_s"] = elapsed;
            row["clips"].push_back(std::move(entry));
        }
        Json aggregate = Json::object();
        for (const char* key : {"words", "errors", "substitutions", "deletions", "insertions"}) {
            std::size_t sum = 0;
            for (const auto& [name, group] : row["groups"].items()) {
                (void)name;
                sum += group[key].get<std::size_t>();
            }
            aggregate[key] = sum;
        }
        for (const char* key : {"decode_s", "audio_s"}) {
            double sum = 0;
            for (const auto& [name, group] : row["groups"].items()) {
                (void)name;
                sum += group[key].get<double>();
            }
            aggregate[key] = sum;
        }
        row["aggregate"] = std::move(aggregate);
        const auto finish = [](Json& group) {
            const auto words = group["words"].get<std::size_t>();
            group["normalized_wer"] =
                words != 0 ? Json(static_cast<double>(group["errors"].get<std::size_t>()) /
                                  static_cast<double>(words))
                           : Json(nullptr);
            group["real_time_factor"] =
                group["decode_s"].get<double>() / group["audio_s"].get<double>();
        };
        for (auto& [name, group] : row["groups"].items()) {
            (void)name;
            finish(group);
        }
        finish(row["aggregate"]);
    } catch (const std::exception& error) {
        row["error"] = error.what();
    }
    if (backend && backend->cleanup) {
        backend->cleanup();
    }
    const bool failed = row.contains("error");
    report["results"].push_back(std::move(row));
    write_json(args.output, report);
    return failed ? 1 : 0;
}

} // namespace fv::bench
