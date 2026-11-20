#include "bench/meeting_run.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <stdexcept>

#include "base/utf8.h"
#include "bench/meeting_metrics.h"
#include "bench/provenance.h"
#include "meeting_capture/asr_audio.h"
#include "meeting_capture/asr_engine.h"
#include "meeting_capture/spool.h"
#include "meeting_state/types.h"
#include "pyport/number.h"

namespace fv::bench {

namespace {

constexpr std::string_view kChannel = "loopback";

double seconds_since(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

Json segment_json(const meeting_state::TranscriptSegment& segment) {
    return Json{{"id", segment.segment_id},
                {"start_s", segment.start_s},
                {"end_s", segment.end_s},
                {"text", segment.text},
                {"channel", segment.channel}};
}

// The session WAV the spool writes: native audio resampled in windows of
// SESSION_RESAMPLE_WINDOW_S, as the live writer does, not in one FFT over the meeting.
std::vector<std::int16_t> session_audio(std::span<const std::int16_t> pcm, int rate) {
    if (rate == meeting::TARGET_RATE) {
        return {pcm.begin(), pcm.end()};
    }
    const auto window =
        static_cast<std::size_t>(std::llround(meeting::SESSION_RESAMPLE_WINDOW_S * rate));
    std::vector<std::int16_t> out;
    for (std::size_t at = 0; at < pcm.size(); at += window) {
        const auto piece =
            meeting::resample_to_16k(pcm.subspan(at, std::min(window, pcm.size() - at)), rate);
        out.insert(out.end(), piece.begin(), piece.end());
    }
    return out;
}

std::string percent(double value) {
    return py::format_fixed(value * 100.0, 2) + "%";
}

std::string signed_percent(double value) {
    return (value >= 0 ? "+" : "") + percent(value);
}

double get_or(const Json& item, std::string_view key, double fallback) {
    const auto found = item.find(key);
    return found == item.end() || found->is_null() ? fallback : found->get<double>();
}

} // namespace

MeetingArgs::MeetingArgs()
    : target_sec(meeting::DEFAULT_TARGET_SEC), max_sec(meeting::DEFAULT_MAX_SEC) {}

std::vector<std::pair<std::int64_t, std::int64_t>> chunk_ranges(std::span<const std::int16_t> audio,
                                                                int sample_rate, double target_sec,
                                                                double max_sec) {
    std::vector<std::pair<std::int64_t, std::int64_t>> ranges;
    const auto size = static_cast<std::int64_t>(audio.size());
    const auto max_samples = std::max<std::int64_t>(1, std::llround(max_sec * sample_rate));
    const auto min_flush = std::llround(meeting::MIN_FLUSH_S * sample_rate);
    std::int64_t position = 0;
    while (position < size) {
        const auto candidate =
            audio.subspan(static_cast<std::size_t>(position),
                          static_cast<std::size_t>(std::min(max_samples, size - position)));
        auto cut = meeting::find_cut_point(candidate, sample_rate, target_sec, max_sec)
                       .value_or(static_cast<std::int64_t>(candidate.size()));
        if (cut <= 0) {
            break;
        }
        const std::int64_t end = std::min(size, position + cut);
        if (end - position < min_flush) {
            break;
        }
        ranges.emplace_back(position, end);
        position = end;
    }
    return ranges;
}

Json decode_meeting(const MeetingDecoders& decoders, const MeetingSpec& spec,
                    const std::filesystem::path& source_path, double target_sec, double max_sec,
                    bool run_offline, const std::function<void(const std::string&)>& progress) {
    const auto [pcm, source_rate] = meeting::load_wav_int16(source_path);
    const std::string meeting_id = "bench_" + spec.meeting_id;

    Json draft_segments = Json::array();
    std::int64_t chunk_count = 0;
    const auto started = std::chrono::steady_clock::now();
    for (const auto& [start, end] : chunk_ranges(pcm, source_rate, target_sec, max_sec)) {
        const auto native = std::span<const std::int16_t>(pcm).subspan(
            static_cast<std::size_t>(start), static_cast<std::size_t>(end - start));
        const std::vector<std::int16_t> samples = meeting::resample_to_16k(native, source_rate);
        meeting_state::SpooledChunk chunk;
        chunk.chunk_id = chunk_count + 1;
        chunk.meeting_id = meeting_id;
        chunk.channel = kChannel;
        chunk.seq = chunk_count;
        chunk.start_s = static_cast<double>(start) / source_rate;
        chunk.duration_s = static_cast<double>(samples.size()) / meeting::TARGET_RATE;
        chunk.sample_rate = meeting::TARGET_RATE;
        std::vector<Segment> decoded;
        if (!meeting::is_digital_silence(samples)) {
            decoded = decoders.draft(meeting::prepare_for_decoder(samples, meeting::TARGET_RATE));
        }
        for (const auto& segment : meeting::chunk_segments(meeting_id, chunk, decoded)) {
            draft_segments.push_back(segment_json(segment));
        }
        ++chunk_count;
        if (progress && chunk_count % 25 == 0) {
            progress("  " + spec.meeting_id + ": " +
                     py::format_fixed(static_cast<double>(end) / source_rate / 60.0, 1) +
                     " audio min, " + std::to_string(chunk_count) + " chunks, " +
                     py::format_fixed(seconds_since(started) / 60.0, 1) + " wall min");
        }
    }
    const double elapsed_s = seconds_since(started);
    const double duration_s = static_cast<double>(pcm.size()) / source_rate;

    Json offline_segments = Json::array();
    double offline_elapsed_s = 0.0;
    if (run_offline) {
        if (!decoders.offline) {
            throw std::runtime_error("Required offline ASR model is unavailable");
        }
        const auto offline_started = std::chrono::steady_clock::now();
        const std::vector<std::int16_t> session = session_audio(pcm, source_rate);
        meeting::SessionAudioOptions options;
        options.meeting_id = meeting_id;
        options.channel = kChannel;
        for (const auto& segment : meeting::transcribe_session_audio(
                 decoders.offline, session, meeting::TARGET_RATE, options)) {
            offline_segments.push_back(segment_json(segment));
        }
        offline_elapsed_s = seconds_since(offline_started);
        if (progress) {
            progress("  " + spec.meeting_id + ": offline " +
                     std::to_string(offline_segments.size()) + " segments, " +
                     py::format_fixed(offline_elapsed_s / 60.0, 1) + " wall min");
        }
    }
    const auto per_audio = [&](double seconds) {
        return duration_s > 0 ? seconds / duration_s : 0.0;
    };
    return Json{{"meeting_id", spec.meeting_id},
                {"description", spec.description},
                {"audio_path", path_to_utf8(source_path)},
                {"duration_s", duration_s},
                {"source_rate", source_rate},
                {"chunks", chunk_count},
                {"target_sec", target_sec},
                {"max_sec", max_sec},
                {"elapsed_s", elapsed_s},
                {"rtf", per_audio(elapsed_s)},
                {"offline_elapsed_s", offline_elapsed_s},
                {"offline_rtf", per_audio(offline_elapsed_s)},
                {"combined_rtf", per_audio(elapsed_s + offline_elapsed_s)},
                {"draft_segments", draft_segments},
                // Parakeet's chunks are stored as decoded: the stored live transcript is the
                // draft.
                {"final_segments", draft_segments},
                {"offline_segments", std::move(offline_segments)},
                {"run_offline", run_offline}};
}

Json score_result(Json result, const std::filesystem::path& annotations_dir) {
    const auto reference =
        parse_reference_words(annotations_dir, result.at("meeting_id").get<std::string>());
    Json stats{{"words", reference.size()},
               {"first_s", reference.front().start_s},
               {"last_s", reference.back().end_s}};
    stats.update(reference_overlap_stats(reference));
    result["reference"] = std::move(stats);
    result["draft_score"] =
        score_timed_transcript(reference, timed_texts(result.at("draft_segments")));
    result["final_score"] =
        score_timed_transcript(reference, timed_texts(result.at("final_segments")));
    const bool run_offline = result.value("run_offline", true);
    result["offline_score"] =
        run_offline ? score_timed_transcript(
                          reference, timed_texts(result.value("offline_segments", Json::array())))
                    : Json(nullptr);
    return result;
}

Json meeting_summary(std::span<const Json> results, std::string_view model,
                     std::string_view language, double target_sec, double max_sec,
                     bool run_offline) {
    if (run_offline && std::any_of(results.begin(), results.end(), [](const Json& item) {
            return !item.contains("offline_score") || item["offline_score"].is_null();
        })) {
        throw std::invalid_argument("Required offline score is missing");
    }
    const std::string score_key = run_offline ? "offline_score" : "final_score";
    double duration_s = 0.0;
    double elapsed_s = 0.0;
    double offline_elapsed_s = 0.0;
    std::vector<Json> draft;
    std::vector<Json> final_scores;
    std::vector<Json> offline;
    std::vector<Json> product;
    std::size_t overlap_words = 0;
    std::size_t overlapping = 0;
    for (const Json& item : results) {
        duration_s += item.at("duration_s").get<double>();
        elapsed_s += item.at("elapsed_s").get<double>();
        offline_elapsed_s += get_or(item, "offline_elapsed_s", 0.0);
        draft.push_back(item.at("draft_score"));
        final_scores.push_back(item.at("final_score"));
        if (item.contains("offline_score") && item["offline_score"].is_object()) {
            offline.push_back(item["offline_score"]);
        }
        product.push_back(item.at(score_key));
        overlap_words += item.at("reference").at("reference_words").get<std::size_t>();
        overlapping += item.at("reference").at("overlapping_reference_words").get<std::size_t>();
    }
    const double audio_hours = duration_s / 3600.0;
    const auto per_audio = [&](double seconds) {
        return duration_s > 0 ? seconds / duration_s : 0.0;
    };
    const double combined_rtf = per_audio(elapsed_s + offline_elapsed_s);
    const Json product_total = aggregate_scores(product);
    double worst = 1.0;
    if (!product.empty()) {
        worst = product.front().at("wer").get<double>();
        for (const Json& score : product) {
            worst = std::max(worst, score.at("wer").get<double>());
        }
    }
    const Json checks{{"meeting_count", results.size() >= MIN_GATE_MEETINGS},
                      {"audio_hours", audio_hours >= MIN_GATE_AUDIO_HOURS},
                      {"micro_tcwer", product_total.at("wer").get<double>() <= MAX_GATE_MICRO_WER},
                      {"worst_meeting_tcwer", worst <= MAX_GATE_MEETING_WER},
                      {"runtime_factor", combined_rtf <= MAX_GATE_RTF}};
    bool passed = true;
    for (const auto& [name, value] : checks.items()) {
        (void)name;
        passed = passed && value.get<bool>();
    }
    Json per_meeting = Json::array();
    for (const Json& item : results) {
        const Json& offline_score =
            item.contains("offline_score") ? item["offline_score"] : Json(nullptr);
        per_meeting.push_back(Json{
            {"meeting_id", item.at("meeting_id")},
            {"duration_s", item.at("duration_s")},
            {"chunks", item.at("chunks")},
            {"rtf", item.at("rtf")},
            {"offline_rtf", get_or(item, "offline_rtf", 0.0)},
            {"draft_wer", item.at("draft_score").at("wer")},
            {"final_wer", item.at("final_score").at("wer")},
            {"offline_wer", offline_score.is_object() ? offline_score.at("wer") : Json(nullptr)},
            {"deletion_rate", item.at(score_key).at("deletion_rate")},
            {"insertion_rate", item.at(score_key).at("insertion_rate")}});
    }
    return Json{
        {"benchmark_version", kMeetingBenchmarkVersion},
        {"model", std::string(model)},
        {"language", std::string(language)},
        {"run_offline", run_offline},
        {"target_sec", target_sec},
        {"max_sec", max_sec},
        {"corpus", "AMI manual annotations 1.6.2 / headset mix"},
        {"meetings", results.size()},
        {"audio_hours", audio_hours},
        {"elapsed_hours", elapsed_s / 3600.0},
        {"offline_elapsed_hours", offline_elapsed_s / 3600.0},
        {"rtf", per_audio(elapsed_s)},
        {"offline_rtf", per_audio(offline_elapsed_s)},
        {"combined_rtf", combined_rtf},
        {"reference_overlap",
         Json{{"reference_words", overlap_words}, {"overlapping_reference_words", overlapping}}},
        {"draft", aggregate_scores(draft)},
        {"final", aggregate_scores(final_scores)},
        {"offline", offline.empty() ? Json(nullptr) : aggregate_scores(offline)},
        {"quality_gate", Json{{"passed", passed},
                              {"product", run_offline ? "offline" : "final"},
                              {"checks", checks},
                              {"thresholds", Json{{"minimum_meetings", MIN_GATE_MEETINGS},
                                                  {"minimum_audio_hours", MIN_GATE_AUDIO_HOURS},
                                                  {"maximum_micro_tcwer", MAX_GATE_MICRO_WER},
                                                  {"maximum_meeting_tcwer", MAX_GATE_MEETING_WER},
                                                  {"maximum_runtime_factor", MAX_GATE_RTF}}},
                              {"worst_meeting_tcwer", worst}}},
        {"per_meeting", std::move(per_meeting)}};
}

void write_meeting_summary(const std::filesystem::path& dir, const Json& summary) {
    write_json(dir / "summary.json", summary);
    const Json& overlap = summary.at("reference_overlap");
    const double overlap_rate =
        static_cast<double>(overlap.at("overlapping_reference_words").get<std::size_t>()) /
        static_cast<double>(
            std::max<std::size_t>(1, overlap.at("reference_words").get<std::size_t>()));
    const Json& gate = summary.at("quality_gate");
    std::string md = "# Meeting Mode dogfood benchmark\n\n";
    md += "- Model: `" + summary.at("model").get<std::string>() + "`\n";
    md += "- Language: `" + summary.at("language").get<std::string>() + "`\n";
    md += "- Chunk target / maximum: `" + py::float_repr(summary.at("target_sec").get<double>()) +
          "` / `" + py::float_repr(summary.at("max_sec").get<double>()) + "` seconds\n";
    md += "- Meetings: " + std::to_string(summary.at("meetings").get<std::size_t>()) + "\n";
    md += "- Audio: " + py::format_fixed(summary.at("audio_hours").get<double>(), 2) + " hours\n";
    md += "- Runtime factor (live draft): " + py::format_fixed(summary.at("rtf").get<double>(), 3) +
          "\n";
    md += "- Runtime factor (offline pass): " +
          py::format_fixed(summary.at("offline_rtf").get<double>(), 3) + "\n";
    md += "- Runtime factor (combined): " +
          py::format_fixed(summary.at("combined_rtf").get<double>(), 3) + "\n";
    md += "- Exceptional-quality gate (" + gate.at("product").get<std::string>() + "): `" +
          (gate.at("passed").get<bool>() ? "PASS" : "FAIL") + "`\n";
    md += "- Reference words overlapping another speaker: " + percent(overlap_rate) + "\n";
    md += "- Draft tcWER: " + percent(summary.at("draft").at("wer").get<double>()) + "\n";
    md += "- Final (stored live) tcWER: " + percent(summary.at("final").at("wer").get<double>()) +
          "\n";
    const Json& offline = summary.at("offline");
    if (offline.is_object()) {
        const double draft = summary.at("draft").at("wer").get<double>();
        const double delta = offline.at("wer").get<double>() - draft;
        const double relative = draft != 0 ? delta / draft : 0.0;
        md += "- Offline tcWER: " + percent(offline.at("wer").get<double>()) + "\n";
        md += "- Offline vs draft: " + signed_percent(delta) + " absolute (" +
              signed_percent(relative) + " relative)\n";
        md += "- Offline substitutions: " + percent(offline.at("substitution_rate").get<double>()) +
              "\n";
        md += "- Offline deletions: " + percent(offline.at("deletion_rate").get<double>()) + "\n";
        md += "- Offline insertions: " + percent(offline.at("insertion_rate").get<double>()) + "\n";
    } else {
        const Json& final_score = summary.at("final");
        md +=
            "- Final substitutions: " + percent(final_score.at("substitution_rate").get<double>()) +
            "\n";
        md += "- Final deletions: " + percent(final_score.at("deletion_rate").get<double>()) + "\n";
        md +=
            "- Final insertions: " + percent(final_score.at("insertion_rate").get<double>()) + "\n";
    }
    md +=
        "\n| Meeting | Minutes | Chunks | Draft WER | Final WER | Offline WER | Live RTF | Offline "
        "RTF |\n|---|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const Json& item : summary.at("per_meeting")) {
        const Json& offline_wer = item.at("offline_wer");
        md += "| " + item.at("meeting_id").get<std::string>() + " | " +
              py::format_fixed(item.at("duration_s").get<double>() / 60.0, 1) + " | " +
              std::to_string(item.at("chunks").get<std::int64_t>()) + " | " +
              percent(item.at("draft_wer").get<double>()) + " | " +
              percent(item.at("final_wer").get<double>()) + " | " +
              (offline_wer.is_null() ? std::string("\u2014") : percent(offline_wer.get<double>())) +
              " | " + py::format_fixed(item.at("rtf").get<double>(), 3) + " | " +
              py::format_fixed(get_or(item, "offline_rtf", 0.0), 3) + " |\n";
    }
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    std::ofstream out(dir / "summary.md", std::ios::binary | std::ios::trunc);
    out << md;
}

} // namespace fv::bench
