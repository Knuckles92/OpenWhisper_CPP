#include "meeting_capture/offline.h"

#include <algorithm>
#include <exception>
#include <format>
#include <memory>
#include <set>
#include <system_error>

#include "base/log.h"
#include "meeting_capture/asr_audio.h"
#include "meeting_capture/segment_ids.h"
#include "meeting_capture/session_audio.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"

namespace fv::meeting {

using meeting_state::TranscriptSegment;

namespace {

constexpr log::Logger logger("meeting.asr.offline");

// Python's int(x) of a float: truncation toward zero.
std::int64_t py_trunc(double value) {
    return static_cast<std::int64_t>(value);
}

std::string channel_display_name(std::string_view channel) {
    if (channel == meeting_state::kChannelMic) {
        return "Microphone";
    }
    if (channel == meeting_state::kChannelLoopback) {
        return "System Audio";
    }
    return py::capitalize(channel);
}

void report_progress(const OfflineProgress& progress, const std::string& detail, int current,
                     int total) {
    try {
        progress(detail, current, total);
    } catch (const std::exception& error) {
        logger.error_with(error, "Offline ASR progress callback failed");
    } catch (...) {
        logger.error("Offline ASR progress callback failed");
    }
}

// The window's segments on the meeting clock, minus the empty ones (which keep their ordinal).
std::vector<TranscriptSegment> window_segments(const std::vector<Segment>& decoded,
                                               double window_origin,
                                               const SessionAudioOptions& options) {
    std::vector<TranscriptSegment> out;
    std::int64_t ordinal = -1;
    for (const Segment& segment : decoded) {
        ++ordinal;
        std::string text = py::strip(segment.text);
        if (text.empty()) {
            continue;
        }
        TranscriptSegment made;
        made.start_s = window_origin + segment.start;
        made.end_s = window_origin + segment.end;
        made.segment_id =
            offline_segment_id(options.meeting_id, options.channel, made.start_s, ordinal);
        made.meeting_id = options.meeting_id;
        made.channel = options.channel;
        made.text = std::move(text);
        out.push_back(std::move(made));
    }
    return out;
}

const py::Regex& word_pattern() {
    static const py::Regex pattern("\\w+");
    return pattern;
}

std::vector<std::string> words_of(std::string_view text) {
    std::vector<std::string> out;
    for (const py::Match& match : word_pattern().finditer(py::lower(text))) {
        out.push_back(match.group());
    }
    return out;
}

} // namespace

std::vector<std::pair<std::int64_t, std::int64_t>>
offline_cut_ranges(std::int64_t sample_count, int sample_rate,
                   std::optional<std::span<const std::int16_t>> audio,
                   const OfflineCutOptions& options) {
    const int rate = std::max(1, sample_rate);
    const std::int64_t total = std::max<std::int64_t>(0, sample_count);
    std::vector<std::pair<std::int64_t, std::int64_t>> ranges;
    if (total <= 0) {
        return ranges;
    }
    const std::int64_t overlap_n = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(py::round_half_even(options.overlap_s * rate)));
    // find_cut_point reads no sample past max(max_sec, target_sec) seconds (its scanner stops
    // at the hard maximum), so each scan is given only that much: the same cut, without
    // rescanning the rest of an hour-long session for every window.
    const std::int64_t scan_limit = std::max<std::int64_t>(
        0, std::max(py_trunc(options.max_sec * rate), py_trunc(options.target_sec * rate)));
    std::int64_t position = 0;
    while (position < total) {
        const std::int64_t remaining_n = total - position;
        std::optional<std::int64_t> cut;
        if (audio && static_cast<std::int64_t>(audio->size()) > position) {
            std::span<const std::int16_t> remaining =
                audio->subspan(static_cast<std::size_t>(position));
            remaining = remaining.first(
                std::min<std::size_t>(remaining.size(), static_cast<std::size_t>(scan_limit)));
            cut = find_cut_point(remaining, rate, options.target_sec, options.max_sec,
                                 options.quiet_rms, options.quiet_window_s);
        }
        if (!cut) {
            cut = std::min(remaining_n,
                           static_cast<std::int64_t>(py::round_half_even(options.max_sec * rate)));
        }
        const std::int64_t length = std::max<std::int64_t>(1, std::min(*cut, remaining_n));
        const std::int64_t end = position + length;
        ranges.emplace_back(position, end);
        if (end >= total) {
            break;
        }
        std::int64_t next = end - overlap_n;
        if (next <= position) {
            next = end;
        }
        position = next;
    }
    return ranges;
}

std::vector<TranscriptSegment> drop_overlapped_prefix(std::span<const TranscriptSegment> segments,
                                                      double keep_from_s) {
    std::vector<TranscriptSegment> out;
    for (const TranscriptSegment& segment : segments) {
        if (segment.end_s > keep_from_s) {
            out.push_back(segment);
        }
    }
    return out;
}

std::vector<TranscriptSegment> drop_mic_echo(std::vector<TranscriptSegment> segments) {
    struct Heard {
        double start;
        double end;
        std::set<std::string> words;
    };
    std::vector<Heard> loopback;
    for (const TranscriptSegment& segment : segments) {
        if (segment.channel == meeting_state::kChannelLoopback) {
            const std::vector<std::string> words = words_of(segment.text);
            loopback.push_back({segment.start_s, segment.end_s, {words.begin(), words.end()}});
        }
    }
    if (loopback.empty()) {
        return segments;
    }
    std::vector<TranscriptSegment> kept;
    std::size_t dropped = 0;
    for (TranscriptSegment& segment : segments) {
        if (segment.channel != meeting_state::kChannelMic) {
            kept.push_back(std::move(segment));
            continue;
        }
        const std::vector<std::string> words = words_of(segment.text);
        if (words.size() < ECHO_MIN_WORDS) {
            kept.push_back(std::move(segment));
            continue;
        }
        const double start = segment.start_s - ECHO_TIME_SLACK_S;
        const double end = segment.end_s + ECHO_TIME_SLACK_S;
        std::set<std::string> heard;
        for (const Heard& other : loopback) {
            if (other.start < end && other.end > start) {
                heard.insert(other.words.begin(), other.words.end());
            }
        }
        const auto shared = static_cast<std::size_t>(std::ranges::count_if(
            words, [&](const std::string& word) { return heard.contains(word); }));
        if (static_cast<double>(shared) >= ECHO_WORD_OVERLAP * static_cast<double>(words.size())) {
            ++dropped;
            continue;
        }
        kept.push_back(std::move(segment));
    }
    if (dropped > 0) {
        logger.info("Offline ASR dropped {} mic segment(s) echoing system audio", dropped);
    }
    return kept;
}

WindowDecoder background_window_decoder(speech::SpeechService& service, std::string language,
                                        CancelToken cancel, speech::Slot slot) {
    return [&service, language = std::move(language), cancel = std::move(cancel),
            slot](std::vector<float> audio) {
        speech::DecodeJob job;
        speech::AudioClip clip;
        clip.end = audio.size();
        clip.samples = std::make_shared<const std::vector<float>>(std::move(audio));
        job.input = std::move(clip);
        job.options.language = language;
        job.lane = speech::Lane::Background;
        job.slot = slot;
        job.cancel = cancel;
        return service.run(std::move(job)).transcript.segments;
    };
}

std::vector<TranscriptSegment> transcribe_session_audio(const WindowDecoder& decode,
                                                        std::span<const std::int16_t> frames,
                                                        int sample_rate,
                                                        const SessionAudioOptions& options) {
    std::vector<TranscriptSegment> collected;
    if (frames.empty() || !decode) {
        return collected;
    }
    OfflineCutOptions cuts;
    cuts.target_sec = options.target_sec;
    cuts.max_sec = options.max_sec;
    cuts.overlap_s = options.overlap_s;
    const auto ranges =
        offline_cut_ranges(static_cast<std::int64_t>(frames.size()), sample_rate, frames, cuts);
    const auto total = static_cast<int>(ranges.size());
    const std::string display = channel_display_name(options.channel);
    const auto rate = static_cast<double>(sample_rate);
    double keep_from_s = options.origin_s;
    int index = 0;
    for (const auto& [start, end] : ranges) {
        ++index;
        const std::span<const std::int16_t> window =
            frames.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start));
        std::vector<float> audio = prepare_for_decoder(window, sample_rate);
        if (audio.empty()) {
            continue;
        }
        const double window_origin = options.origin_s + static_cast<double>(start) / rate;
        const double window_end = window_origin + static_cast<double>(end - start) / rate;
        if (options.progress) {
            report_progress(options.progress,
                            std::format("{}: decoding window {}/{} ({}s - {}s)", display, index,
                                        total, py::format_fixed(window_origin, 1),
                                        py::format_fixed(window_end, 1)),
                            index, total);
        }
        const std::string failed =
            std::format("Offline ASR failed for {} window {}/{} at {}s", options.channel, index,
                        total, py::format_fixed(window_origin, 1));
        const std::string where =
            std::format("Offline ASR window failed ({} {}-{}s)", options.channel,
                        py::format_fixed(window_origin, 1), py::format_fixed(window_end, 1));
        std::vector<Segment> decoded;
        try {
            decoded = decode(std::move(audio));
        } catch (const std::exception& error) {
            logger.error_with(error, where);
            std::throw_with_nested(OfflineWindowError(failed));
        } catch (...) {
            logger.error("{}", where);
            std::throw_with_nested(OfflineWindowError(failed));
        }
        std::vector<TranscriptSegment> kept =
            drop_overlapped_prefix(window_segments(decoded, window_origin, options), keep_from_s);
        if (!kept.empty()) {
            // The later window owns the overlap: earlier segments starting at or after its origin
            // would be counted twice.
            std::erase_if(collected, [&](const TranscriptSegment& segment) {
                return !(segment.start_s < window_origin - 1e-6);
            });
        }
        for (TranscriptSegment& segment : kept) {
            collected.push_back(std::move(segment));
        }
        keep_from_s = options.origin_s + static_cast<double>(end) / rate;
    }
    return collected;
}

std::vector<TranscriptSegment> transcribe_meeting_sessions(const WindowDecoder& decode,
                                                           const std::filesystem::path& spool_dir,
                                                           const std::string& meeting_id,
                                                           const std::vector<Json>& chunks,
                                                           const MeetingSessionsOptions& options) {
    const SessionTranscriber transcriber =
        options.transcribe ? options.transcribe
                           : SessionTranscriber([&](std::span<const std::int16_t> frames, int rate,
                                                    const std::string& channel, double origin_s,
                                                    const OfflineProgress& progress) {
                                 SessionAudioOptions session;
                                 session.meeting_id = meeting_id;
                                 session.channel = channel;
                                 session.origin_s = origin_s;
                                 session.progress = progress;
                                 return transcribe_session_audio(decode, frames, rate, session);
                             });
    std::vector<TranscriptSegment> merged;
    for (const std::string& channel : options.channels) {
        std::error_code ignored;
        if (!std::filesystem::is_directory(spool_dir, ignored) && chunks.empty()) {
            continue;
        }
        auto [frames, rate, origin] = load_channel_session(spool_dir, channel, chunks);
        if (!frames || frames->empty()) {
            logger.info("No session audio for offline ASR on channel {}", channel);
            continue;
        }
        std::vector<TranscriptSegment> decoded =
            transcriber(*frames, rate, channel, origin, options.progress);
        const double seconds =
            static_cast<double>(frames->size()) / static_cast<double>(rate != 0 ? rate : 16000);
        logger.info("Offline ASR {}: {} segments from {}s of audio", channel, decoded.size(),
                    py::format_fixed(seconds, 1));
        for (TranscriptSegment& segment : decoded) {
            merged.push_back(std::move(segment));
        }
    }
    merged = drop_mic_echo(std::move(merged));
    std::ranges::stable_sort(merged, [](const TranscriptSegment& a, const TranscriptSegment& b) {
        if (a.start_s != b.start_s) {
            return a.start_s < b.start_s;
        }
        if (a.channel != b.channel) {
            return a.channel < b.channel;
        }
        return a.segment_id < b.segment_id;
    });
    return merged;
}

} // namespace fv::meeting
