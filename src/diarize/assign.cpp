#include "diarize/assign.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>

#include "base/log.h"
#include "dsp/fft_resample.h"

namespace fv::diarize {

namespace {

constexpr log::Logger logger("meeting.diarize.assign");
constexpr int kTargetRate = 16000;

// meeting.asr.audio.prepare_for_whisper: int16 to float32 (value / 32768), resampled to 16 kHz
// with fft_resample when the rate differs, clipped to [-1, 1].
std::vector<float> prepare_for_whisper(std::span<const std::int16_t> frames, int rate) {
    std::vector<float> audio(frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        audio[i] = static_cast<float>(frames[i]) / 32768.0f;
    }
    if (audio.empty()) {
        return audio;
    }
    if (rate != kTargetRate) {
        const auto count = std::max<long long>(
            1, static_cast<long long>(std::nearbyint(static_cast<double>(audio.size()) *
                                                     kTargetRate / static_cast<double>(rate))));
        audio = dsp::fft_resample(audio, static_cast<std::size_t>(count));
    }
    for (float& value : audio) {
        value = std::clamp(value, -1.0f, 1.0f);
    }
    return audio;
}

// int(round((seconds - origin) * rate)), Python's round being half to even.
long long sample_index(double seconds, double origin_s, int rate) {
    return static_cast<long long>(std::nearbyint((seconds - origin_s) * rate));
}

} // namespace

std::vector<std::string> assign_from_frames(Diarizer& diarizer, std::vector<Segment>& segments,
                                            std::span<const std::int16_t> frames, int rate,
                                            double origin_s,
                                            std::function<void(const Segment&)> on_unlabeled) {
    std::vector<std::string> labeled;
    const auto size = static_cast<long long>(frames.size());
    for (Segment& segment : segments) {
        const long long start = std::max(0LL, sample_index(segment.start_s, origin_s, rate));
        const long long end = std::min(size, sample_index(segment.end_s, origin_s, rate));
        if (end <= start) {
            continue;
        }
        std::optional<std::string> participant_id;
        try {
            const std::vector<float> audio =
                prepare_for_whisper(frames.subspan(static_cast<std::size_t>(start),
                                                   static_cast<std::size_t>(end - start)),
                                    rate);
            participant_id = diarizer.assign(segment, audio, kTargetRate);
        } catch (const std::exception& error) {
            logger.error_with(error, std::format("Diarizer assignment failed for {}", segment.id));
            participant_id.reset();
        }
        if (participant_id && !participant_id->empty()) {
            segment.speaker_participant_id = std::move(participant_id);
            segment.speaker_source = "diarizer";
            labeled.push_back(segment.id);
        } else if (on_unlabeled) {
            on_unlabeled(segment);
        }
    }
    return labeled;
}

void refresh_labels(Diarizer& diarizer, std::vector<Segment>& segments) {
    for (Segment& segment : segments) {
        std::optional<std::string> participant_id;
        try {
            participant_id = diarizer.current_label(segment.id);
        } catch (const std::exception& error) {
            logger.error_with(error,
                              std::format("Diarizer label refresh failed for {}", segment.id));
            continue;
        }
        if (participant_id && !participant_id->empty()) {
            segment.speaker_participant_id = std::move(participant_id);
        }
    }
}

} // namespace fv::diarize
