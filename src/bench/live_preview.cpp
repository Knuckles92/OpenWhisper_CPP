#include "bench/live_preview.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>

#include "audio/file_decoder.h"
#include "audio/pcm_tape.h"
#include "base/clock.h"
#include "base/hash.h"
#include "base/testing/manual_executor.h"
#include "base/utf8.h"
#include "bench/meeting_metrics.h"
#include "bench/provenance.h"
#include "dsp/fft_resample.h"
#include "pyport/number.h"
#include "speech/window_preview.h"

namespace fv::bench {

namespace {

constexpr std::string_view kSession = "live-preview-benchmark";
// How long one window may take before the replay gives up on it.
constexpr auto kWindowTimeout = std::chrono::minutes(10);
// The preview's poll is driven by hand: each step is one poll interval of its own clock.
constexpr auto kPoll = std::chrono::milliseconds(100);
// Steps a window may wait to start decoding (the tape is checked once per step).
constexpr int kIdleSteps = 50;

double steady_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void feed(audio::PcmTape& tape, std::span<const std::int16_t> audio, std::int64_t start,
          std::int64_t end, int block_frames) {
    // Recorder callbacks arrive in blocks of block_frames, the last of a recording shorter.
    for (std::int64_t at = start; at < end; at += block_frames) {
        const auto count = std::min<std::int64_t>(block_frames, end - at);
        tape.append(audio.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(count)));
    }
}

// The windows finalize() decodes from `frames` of retained audio: blocks accumulate until they
// reach chunk_duration_s, and a remainder makes one more.
std::size_t tail_windows(std::int64_t frames, int rate, int block_frames, double chunk) {
    std::size_t windows = 0;
    double duration = 0.0;
    for (std::int64_t at = 0; at < frames; at += block_frames) {
        duration += static_cast<double>(std::min<std::int64_t>(block_frames, frames - at)) / rate;
        if (duration >= chunk) {
            ++windows;
            duration = 0.0;
        }
    }
    if (duration > 0.0) {
        ++windows;
    }
    return windows;
}

[[noreturn]] void decode_failed() {
    // Production catches inference errors; never score those as silence.
    throw std::runtime_error("Preview decode failed; inspect inference log");
}

// Steps the preview's UI executor until the window after `count` was processed.
void wait_for_window(speech::WindowPreview& preview, ManualExecutor& ui, std::size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + kWindowTimeout;
    bool started = false;
    int idle = 0;
    for (;;) {
        ui.advance(kPoll);
        ui.run_all();
        const bool busy = preview.decoding();
        if (!busy && preview.chunk_count() > count) {
            return;
        }
        if (busy) {
            started = true;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        } else if (started || ++idle > kIdleSteps) {
            decode_failed();
        }
        if (std::chrono::steady_clock::now() > deadline) {
            decode_failed();
        }
    }
}

std::string visible_text(const Json& updates, double at_s) {
    for (auto it = updates.rbegin(); it != updates.rend(); ++it) {
        if ((*it)["visible_s"].get<double>() <= at_s && !(*it)["flush"].get<bool>()) {
            return (*it)["text"].get<std::string>();
        }
    }
    return "";
}

Json number_or_null(const std::optional<double>& value, double scale = 1.0) {
    return value ? Json(*value * scale) : Json(nullptr);
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot read " + path_to_utf8(path) + ".");
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// decode_audio(path, sampling_rate=44100) and the recorder's PCM16 conversion.
std::vector<std::int16_t> decode_pcm(const std::filesystem::path& path, int rate) {
    audio::AudioFileDecoder decoder(path);
    std::vector<std::int16_t> interleaved;
    while (decoder.read_s16(interleaved)) {
    }
    const int channels = std::max(1, decoder.channels());
    std::vector<float> mono(interleaved.size() / static_cast<std::size_t>(channels));
    for (std::size_t frame = 0; frame < mono.size(); ++frame) {
        float sum = 0.0F;
        for (int c = 0; c < channels; ++c) {
            sum += static_cast<float>(interleaved[frame * static_cast<std::size_t>(channels) +
                                                  static_cast<std::size_t>(c)]) /
                   32768.0F;
        }
        mono[frame] = sum / static_cast<float>(channels);
    }
    if (decoder.rate() != rate && !mono.empty()) {
        const auto count =
            static_cast<std::size_t>(static_cast<double>(mono.size()) * rate / decoder.rate());
        mono = dsp::fft_resample(mono, count);
    }
    std::vector<std::int16_t> pcm(mono.size());
    for (std::size_t i = 0; i < mono.size(); ++i) {
        const double clipped = std::clamp(static_cast<double>(mono[i]), -1.0, 32767.0 / 32768.0);
        pcm[i] = static_cast<std::int16_t>(std::rint(clipped * 32768.0));
    }
    return pcm;
}

std::string clip_id(const Json& item) {
    return item.contains("id") ? item["id"].get<std::string>()
                               : item.at("audio_path").get<std::string>();
}

std::string clip_group(const Json& item) {
    return item.value("group", std::string("all"));
}

} // namespace

std::vector<std::int64_t> block_ends(std::int64_t length, int sample_rate, double cadence,
                                     int block_frames) {
    const auto size = static_cast<std::int64_t>(
                          std::ceil(cadence * sample_rate / static_cast<double>(block_frames))) *
                      block_frames;
    std::vector<std::int64_t> ends;
    if (size <= 0) {
        return ends;
    }
    for (std::int64_t end = size; end <= length; end += size) {
        ends.push_back(end);
    }
    return ends;
}

Json preview_score(std::string_view reference, std::string_view hypothesis) {
    const TextScore score = score_text(reference, hypothesis);
    return Json{{"substitutions", score.counts.substitutions},
                {"deletions", score.counts.deletions},
                {"insertions", score.counts.insertions},
                {"words", score.words},
                {"errors", score.errors()},
                {"wer", number_or_null(score.wer)}};
}

Json replay(speech::SpeechService& service, std::span<const std::int16_t> audio,
            std::string_view reference, const ReplayOptions& options) {
    const int rate = options.sample_rate;
    const auto length = static_cast<std::int64_t>(audio.size());
    const double duration = static_cast<double>(length) / rate;
    const auto now = options.perf_counter ? options.perf_counter : steady_seconds;

    speech::WindowPreviewOptions settings;
    settings.chunk_duration_s = options.cadence;
    settings.overlap_s = options.overlap;
    settings.block_frames = options.block_frames;
    settings.poll = kPoll;
    ManualExecutor ui;
    speech::WindowPreview preview(service, ui, system_clock(), settings);
    auto tape = std::make_shared<audio::PcmTape>(rate);
    preview.start(tape, std::string(kSession), [](std::string, bool) {});

    std::vector<std::pair<std::int64_t, bool>> endpoints;
    for (const std::int64_t end : block_ends(length, rate, options.cadence, options.block_frames)) {
        endpoints.emplace_back(end, false);
    }
    if (endpoints.empty() || endpoints.back().first < length) {
        endpoints.emplace_back(length, true);
    }

    Json calls = Json::array();
    Json updates = Json::array();
    double clock = 0.0;
    std::int64_t start = 0;
    std::string text;
    bool stopped = false;
    for (const auto& [end, flush] : endpoints) {
        const double ready = static_cast<double>(end) / rate;
        const double began = now();
        const std::size_t count = preview.chunk_count();
        feed(*tape, audio, start, end, options.block_frames);
        if (!flush) {
            wait_for_window(preview, ui, count);
        } else {
            // What the live windows left is the dictation's tail: stop() never decodes it,
            // finalize() does, with the windowing the live preview would have used.
            preview.stop();
            stopped = true;
            if (!preview.finalize(kWindowTimeout)) {
                decode_failed();
            }
            const std::size_t expected =
                tail_windows(end - start, rate, options.block_frames, options.cadence);
            if (preview.chunk_count() != count + expected) {
                decode_failed();
            }
        }
        ui.run_all();
        text = preview.preview_text();
        const double elapsed = now() - began;
        clock = std::max(clock, ready) + elapsed;
        Json call{{"audio_end_s", ready},
                  {"service_s", elapsed},
                  {"visible_s", clock},
                  {"backlog_s", clock - ready},
                  {"flush", flush}};
        if (!text.empty() &&
            (updates.empty() || text != updates.back()["text"].get<std::string>())) {
            Json update = call;
            update["text"] = text;
            updates.push_back(std::move(update));
        }
        calls.push_back(std::move(call));
        start = end;
    }
    if (!stopped) {
        preview.stop();
    }
    preview.cancel();

    const std::string live = visible_text(updates, duration);
    Json first = nullptr;
    for (const Json& update : updates) {
        if (!update["flush"].get<bool>() && update["visible_s"].get<double>() <= duration) {
            first = update["visible_s"];
            break;
        }
    }
    Json result{{"duration_s", duration},
                {"calls", calls},
                {"updates", updates},
                {"live_text", live},
                {"drained_text", text},
                {"live", preview_score(reference, live)},
                {"drained", preview_score(reference, text)},
                {"first_text_before_stop_s", first},
                {"drain_after_stop_s", std::max(0.0, clock - duration)}};
    if (options.timed_words) {
        Json checkpoints = Json::array();
        for (const std::int64_t end :
             block_ends(length, rate, options.checkpoint_cadence, options.block_frames)) {
            const double at = static_cast<double>(end) / rate;
            std::string words;
            for (const Json& word : *options.timed_words) {
                if (word.at("end_s").get<double>() <= at) {
                    words += (words.empty() ? "" : " ") + word.at("text").get<std::string>();
                }
            }
            Json checkpoint{{"at_s", at}};
            checkpoint.update(preview_score(words, visible_text(updates, at)));
            checkpoints.push_back(std::move(checkpoint));
        }
        result["checkpoints"] = std::move(checkpoints);
    }
    return result;
}

std::optional<double> percentile(std::vector<double> values, double p) {
    if (values.empty()) {
        return std::nullopt;
    }
    std::sort(values.begin(), values.end());
    const double rank = p / 100.0 * static_cast<double>(values.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(rank));
    const auto upper = std::min(lower + 1, values.size() - 1);
    return values[lower] + (values[upper] - values[lower]) * (rank - static_cast<double>(lower));
}

Json aggregate(const std::vector<Json>& clips, double cadence) {
    std::vector<double> services;
    double max_backlog = 0.0;
    double all_service = 0.0;
    std::vector<double> first;
    double total_audio = 0.0;
    std::size_t without_live = 0;
    std::size_t call_count = 0;
    std::size_t misses = 0;
    for (const Json& clip : clips) {
        for (const Json& call : clip.at("calls")) {
            all_service += call.at("service_s").get<double>();
            if (call.at("flush").get<bool>()) {
                continue;
            }
            ++call_count;
            const double service = call.at("service_s").get<double>();
            services.push_back(service);
            misses += service > cadence ? 1 : 0;
            max_backlog = std::max(max_backlog, call.at("backlog_s").get<double>());
        }
        if (!clip.at("first_text_before_stop_s").is_null()) {
            first.push_back(clip["first_text_before_stop_s"].get<double>());
        }
        total_audio += clip.at("duration_s").get<double>();
        without_live += clip.at("live_text").get<std::string>().empty() ? 1 : 0;
    }
    Json result{{"clips", clips.size()},
                {"audio_s", total_audio},
                {"calls", call_count},
                {"update_median_ms", number_or_null(percentile(services, 50), 1000.0)},
                {"update_p95_ms", number_or_null(percentile(services, 95), 1000.0)},
                {"first_text_median_s", number_or_null(percentile(first, 50))},
                {"clips_without_live_text", without_live},
                {"rtf", total_audio > 0 ? all_service / total_audio : 0.0},
                {"update_deadline_misses", misses},
                {"max_backlog_s", max_backlog}};
    for (const char* field : {"live", "drained"}) {
        Json counts = Json::object();
        for (const char* key : {"words", "errors", "substitutions", "deletions", "insertions"}) {
            std::size_t sum = 0;
            for (const Json& clip : clips) {
                sum += clip.at(field).at(key).get<std::size_t>();
            }
            counts[key] = sum;
        }
        const auto words = counts["words"].get<std::size_t>();
        counts["wer"] = words != 0 ? Json(static_cast<double>(counts["errors"].get<std::size_t>()) /
                                          static_cast<double>(words))
                                   : Json(nullptr);
        result[field] = std::move(counts);
    }
    std::size_t checkpoint_words = 0;
    std::size_t checkpoint_errors = 0;
    bool any_checkpoint = false;
    for (const Json& clip : clips) {
        for (const Json& point : clip.value("checkpoints", Json::array())) {
            any_checkpoint = true;
            checkpoint_words += point.at("words").get<std::size_t>();
            checkpoint_errors += point.at("errors").get<std::size_t>();
        }
    }
    if (any_checkpoint) {
        result["checkpoint_wer"] = checkpoint_words != 0
                                       ? Json(static_cast<double>(checkpoint_errors) /
                                              static_cast<double>(checkpoint_words))
                                       : Json(nullptr);
    }
    return result;
}

std::pair<Json, std::vector<PreviewClip>> load_clips(const std::filesystem::path& manifest_path,
                                                     std::optional<int> limit) {
    std::string text = read_file(manifest_path);
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3); // utf-8-sig
    }
    Json manifest = Json::parse(text);
    const Json& items = manifest.at("clips");
    const std::size_t count =
        limit ? std::min<std::size_t>(items.size(), static_cast<std::size_t>(*limit))
              : items.size();
    if (count == 0) {
        throw std::invalid_argument("Manifest contains no clips");
    }
    std::vector<PreviewClip> clips;
    for (std::size_t i = 0; i < count; ++i) {
        const Json& item = items[i];
        const auto path = std::filesystem::weakly_canonical(
            manifest_path.parent_path() / path_from_utf8(item.at("audio_path").get<std::string>()));
        PreviewClip clip{item, decode_pcm(path, kRecorderRate), file_identity(path)};
        if (clip.audio.empty()) {
            throw std::invalid_argument("Empty audio: " + path_to_utf8(path));
        }
        clips.push_back(std::move(clip));
    }
    return {std::move(manifest), std::move(clips)};
}

Json run_live_preview(speech::SpeechService& service, const Json& manifest,
                      const std::vector<PreviewClip>& clips, const LivePreviewArgs& args,
                      const std::function<void(const std::string&)>& progress) {
    const double effective =
        std::ceil(args.chunk * kRecorderRate / kCallbackFrames) * kCallbackFrames / kRecorderRate;
    Json corpus = Json::array();
    for (const PreviewClip& clip : clips) {
        corpus.push_back(
            Json{{"id", clip_id(clip.item)},
                 {"group", clip_group(clip.item)},
                 {"audio_sha256", clip.sha256},
                 {"duration_s", static_cast<double>(clip.audio.size()) / kRecorderRate},
                 {"reference", clip.item.at("reference")}});
    }
    Json metrics{
        {"settings",
         Json{{"sample_rate", kRecorderRate},
              {"callback_frames", kCallbackFrames},
              {"chunk_s", args.chunk},
              {"overlap_s", args.overlap},
              {"effective_cadence_s", effective},
              {"repeats", args.repeats},
              {"normalization",
               "meeting_mode.normalize_tokens: NFKC, lowercase; punctuation ignored, apostrophes "
               "retained, AMI acronym underscores removed; fillers/numbers retained"},
              {"timing", "Measured serial service; predicted arrival=max(previous "
                         "completion,audio end)+service. No sleeps, recorder queues/drops, GUI "
                         "paint, or concurrent final ASR."},
              {"live_accuracy", "Full reference versus preview visible at audio EOF, before "
                                "tail/finish; missing tails count as deletions."},
              {"drained_accuracy", "After the stopped preview's tail is finalized; successful "
                                   "preview drain, not the product final ASR."},
              {"decoding", "speech::WindowPreview on the dictation engine (Parakeet), language "
                           "auto, Preview lane."}}},
        {"source", manifest.value("source", Json(nullptr))},
        {"corpus", std::move(corpus)},
        {"passes", Json::array()}};

    ReplayOptions options;
    options.cadence = args.chunk;
    options.overlap = args.overlap;
    const auto warm_started = steady_seconds();
    replay(service, clips.front().audio, clips.front().item.at("reference").get<std::string>(),
           options);
    const double warmup_s = steady_seconds() - warm_started;

    std::vector<Json> every;
    for (int repeat = 0; repeat < args.repeats; ++repeat) {
        Json pass{{"repeat", repeat + 1},
                  {"warmup_s", repeat == 0 ? Json(warmup_s) : Json(nullptr)}};
        std::vector<Json> results;
        for (std::size_t index = 0; index < clips.size(); ++index) {
            if (progress && index % 10 == 0) {
                progress("  pass " + std::to_string(repeat + 1) + "/" +
                         std::to_string(args.repeats) + ": clip " + std::to_string(index + 1) +
                         "/" + std::to_string(clips.size()));
            }
            const PreviewClip& clip = clips[index];
            ReplayOptions timed = options;
            if (clip.item.contains("words")) {
                timed.timed_words = clip.item["words"];
            }
            Json result =
                replay(service, clip.audio, clip.item.at("reference").get<std::string>(), timed);
            result["id"] = clip_id(clip.item);
            result["group"] = clip_group(clip.item);
            results.push_back(std::move(result));
        }
        pass["summary"] = aggregate(results, effective);
        std::set<std::string> groups;
        for (const Json& result : results) {
            groups.insert(result["group"].get<std::string>());
        }
        Json by_group = Json::object();
        for (const std::string& group : groups) {
            std::vector<Json> members;
            for (const Json& result : results) {
                if (result["group"] == group) {
                    members.push_back(result);
                }
            }
            by_group[group] = aggregate(members, effective);
        }
        pass["groups"] = std::move(by_group);
        pass["clips"] = results;
        every.insert(every.end(), results.begin(), results.end());
        metrics["passes"].push_back(std::move(pass));
    }
    metrics["summary"] = aggregate(every, effective);
    std::set<std::string> groups;
    for (const Json& result : every) {
        groups.insert(result["group"].get<std::string>());
    }
    Json by_group = Json::object();
    for (const std::string& group : groups) {
        std::vector<Json> members;
        for (const Json& result : every) {
            if (result["group"] == group) {
                members.push_back(result);
            }
        }
        by_group[group] = aggregate(members, effective);
    }
    metrics["groups"] = std::move(by_group);
    return metrics;
}

namespace {

const Json& at_path(const Json& report, std::initializer_list<const char*> keys) {
    static const Json missing = nullptr;
    const Json* node = &report;
    for (const char* key : keys) {
        if (!node->is_object() || !node->contains(key)) {
            return missing;
        }
        node = &(*node)[key];
    }
    return *node;
}

std::string number_text(const Json& value, int decimals) {
    return value.is_null() ? std::string("none") : py::format_fixed(value.get<double>(), decimals);
}

std::string percent_text(const Json& value) {
    return value.is_null() ? std::string("n/a")
                           : py::format_fixed(value.get<double>() * 100.0, 1) + "%";
}

} // namespace

Json pool_live_preview_reports(const std::vector<Json>& reports,
                               const std::vector<std::string>& names) {
    if (reports.empty()) {
        throw std::invalid_argument("At least one report is required");
    }
    const Json& base = reports.front();
    for (std::size_t i = 1; i < reports.size(); ++i) {
        const Json& report = reports[i];
        for (const auto& [field, value] : at_path(base, {"metrics", "settings"}).items()) {
            if (field == "repeats") {
                continue;
            }
            if (at_path(report, {"metrics", "settings"}).value(field, Json(nullptr)) != value) {
                throw std::invalid_argument("Cannot pool runs with different " + field);
            }
        }
        for (const char* field : {"corpus", "source"}) {
            if (at_path(report, {"metrics", field}) != at_path(base, {"metrics", field})) {
                throw std::invalid_argument(std::string("Cannot pool runs with different ") +
                                            field);
            }
        }
        for (const char* field : {"platform", "machine", "cpu_count"}) {
            if (at_path(report, {"provenance", "machine", field}) !=
                at_path(base, {"provenance", "machine", field})) {
                throw std::invalid_argument(
                    std::string("Cannot pool runs with different hardware ") + field);
            }
        }
    }
    Json pooled{{"settings", at_path(base, {"metrics", "settings"})},
                {"corpus", at_path(base, {"metrics", "corpus"})},
                {"raw_reports", Json::array()},
                {"failures", Json::array()}};
    std::vector<Json> clips;
    Json per_pass = Json::array();
    Json load_s = Json::array();
    const Json* first = nullptr;
    for (std::size_t i = 0; i < reports.size(); ++i) {
        const Json& report = reports[i];
        pooled["raw_reports"].push_back(
            Json{{"file", i < names.size() ? Json(names[i]) : Json(nullptr)},
                 {"sha256", sha256_hex(report.dump())}});
        if (at_path(report, {"metrics"}).contains("error")) {
            pooled["failures"].push_back(
                Json{{"model", "parakeet-v3"}, {"error", at_path(report, {"metrics", "error"})}});
        }
        if (at_path(report, {"metrics", "passes"}).is_null()) {
            continue;
        }
        if (first == nullptr) {
            first = &report;
        } else {
            // Each path is a call's argument: an initializer_list kept in a pair or an array
            // would outlive the array behind it.
            const auto require_same = [&](const char* field,
                                          std::initializer_list<const char*> path) {
                if (at_path(report, path) != at_path(*first, path)) {
                    throw std::invalid_argument(
                        std::string("Cannot pool parakeet-v3 with different ") + field);
                }
            };
            require_same("runtime", {"provenance", "runtime"});
            require_same("model", {"provenance", "model"});
            require_same("device_info", {"metrics", "device_info"});
            require_same("effective cadence", {"metrics", "settings", "effective_cadence_s"});
        }
        load_s.push_back(at_path(report, {"metrics", "load_s"}));
        for (const Json& pass : at_path(report, {"metrics", "passes"})) {
            per_pass.push_back(pass.at("summary"));
            for (const Json& clip : pass.at("clips")) {
                clips.push_back(clip);
            }
        }
    }
    const double cadence =
        at_path(base, {"metrics", "settings", "effective_cadence_s"}).is_number()
            ? at_path(base, {"metrics", "settings", "effective_cadence_s"}).get<double>()
            : 0.0;
    pooled["passes"] = per_pass.size();
    pooled["effective_cadence_s"] = cadence;
    pooled["summary"] = aggregate(clips, cadence);
    std::set<std::string> groups;
    for (const Json& clip : clips) {
        groups.insert(clip.value("group", std::string("all")));
    }
    Json by_group = Json::object();
    for (const std::string& group : groups) {
        std::vector<Json> members;
        for (const Json& clip : clips) {
            if (clip.value("group", std::string("all")) == group) {
                members.push_back(clip);
            }
        }
        by_group[group] = aggregate(members, cadence);
    }
    pooled["groups"] = std::move(by_group);
    pooled["per_pass"] = std::move(per_pass);
    pooled["load_s"] = std::move(load_s);
    if (first != nullptr) {
        pooled["runtime"] = at_path(*first, {"provenance", "runtime"});
        pooled["model"] = at_path(*first, {"provenance", "model"});
        pooled["device_info"] = at_path(*first, {"metrics", "device_info"});
    }
    Json scores = Json::array();
    for (const Json& clip : clips) {
        scores.push_back(Json{{"id", clip.value("id", Json(nullptr))},
                              {"group", clip.value("group", Json(nullptr))},
                              {"live", clip.at("live")},
                              {"drained", clip.at("drained")},
                              {"first_text_before_stop_s", clip.at("first_text_before_stop_s")}});
    }
    pooled["clip_scores"] = std::move(scores);
    return pooled;
}

std::string live_preview_markdown(const Json& pooled) {
    const Json& s = pooled.at("summary");
    std::string md =
        "# Live preview measured results\n\nTiming is measured service plus a simulated serial "
        "arrival clock; it excludes UI paint and recorder queue drops.\n\n| Model / path | Device "
        "| Update p50 / p95 (ms) | First text (s) | Live WER | Drained WER | RTF |\n"
        "|---|---|---:|---:|---:|---:|---:|\n";
    md += "| parakeet-v3 / window | " + pooled.value("device_info", std::string()) + " | " +
          number_text(s.at("update_median_ms"), 0) + " / " + number_text(s.at("update_p95_ms"), 0) +
          " | " + number_text(s.at("first_text_median_s"), 2) + " | " +
          percent_text(s.at("live").at("wer")) + " | " + percent_text(s.at("drained").at("wer")) +
          " | " + py::format_fixed(s.at("rtf").get<double>(), 3) + " |\n";
    md += "\nLive WER includes unshown tails at EOF; drained WER includes the finalized tail. "
          "Lower is better.\n\n| Group | Live / drained WER | Silence insertions (live / "
          "drained) |\n|---|---:|---:|\n";
    for (const auto& [group, summary] : pooled.at("groups").items()) {
        md += "| " + group + " | " + percent_text(summary.at("live").at("wer")) + " / " +
              percent_text(summary.at("drained").at("wer")) + " | " +
              std::to_string(summary.at("live").at("insertions").get<std::size_t>()) + " / " +
              std::to_string(summary.at("drained").at("insertions").get<std::size_t>()) + " |\n";
    }
    md +=
        "\n| Passes | Updates slower than cadence | Clips with no live text |\n|---:|---:|---:|\n";
    md += "| " + std::to_string(pooled.at("passes").get<std::size_t>()) + " | " +
          std::to_string(s.at("update_deadline_misses").get<std::size_t>()) + " / " +
          std::to_string(s.at("calls").get<std::size_t>()) + " | " +
          std::to_string(s.at("clips_without_live_text").get<std::size_t>()) + " / " +
          std::to_string(s.at("clips").get<std::size_t>()) + " |\n";
    if (!pooled.at("failures").empty()) {
        md += "\nFailures: " + pooled.at("failures").dump() + "\n";
    }
    return md;
}

} // namespace fv::bench
