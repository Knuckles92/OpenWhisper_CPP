#include "walkthrough/sections.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>

#include "base/utf8.h"
#include "pyport/number.h"
#include "walkthrough/detail/pcm_wav.h"

namespace fv::walkthrough {

namespace {

// int(round(seconds * rate)): Python's round half to even, then int(), which raises for nan
// (ValueError) and infinity (OverflowError).
std::int64_t rounded_frame(double seconds, int rate) {
    const double value = py::round_half_even(seconds * rate);
    if (std::isnan(value)) {
        throw std::invalid_argument("cannot convert float NaN to integer");
    }
    if (std::isinf(value)) {
        throw std::overflow_error("cannot convert float infinity to integer");
    }
    // Python's int has no limit and every caller clamps to the recording, so a moment beyond
    // int64 saturates (with room left for adding the reach).
    constexpr double kLimit = 4.0e18;
    if (std::fabs(value) >= kLimit) {
        return value > 0 ? std::int64_t{4'000'000'000'000'000'000}
                         : std::int64_t{-4'000'000'000'000'000'000};
    }
    return static_cast<std::int64_t>(value);
}

using Span = std::pair<std::int64_t, std::int64_t>;

// Stretches in seconds as frame ranges inside the recording; +inf is its end.
std::vector<Span> frames_of(const Pcm16& audio,
                            std::span<const std::pair<double, double>> stretches_s) {
    const std::int64_t frames = audio.frames();
    const auto frame = [&](double seconds) {
        if (seconds == std::numeric_limits<double>::infinity()) {
            return frames;
        }
        return std::min(std::max(rounded_frame(seconds, audio.rate), std::int64_t{0}), frames);
    };
    std::vector<Span> spans;
    for (const auto& [start, end] : stretches_s) {
        spans.emplace_back(frame(start), frame(end));
    }
    return spans;
}

// `spans` of frames joined into a WAV at `path`; no file when too short to hold words.
Clip clip(const Pcm16& audio, const std::vector<Span>& spans, const std::filesystem::path& path) {
    std::int64_t frames = 0;
    for (const auto& [start, end] : spans) {
        frames += end - start;
    }
    if (audio.rate == 0) {
        throw std::domain_error("division by zero");
    }
    const double duration = static_cast<double>(frames) / static_cast<double>(audio.rate);
    if (duration < MIN_CLIP_S) {
        return {std::nullopt, duration};
    }
    detail::write_pcm16_wav(path, audio, spans);
    return {path, duration};
}

} // namespace

std::vector<std::int64_t> cut_frames(const Pcm16& audio, std::span<const double> marks_s) {
    const std::int64_t total = audio.frames();
    const auto reach = static_cast<std::int64_t>(REACH_S * audio.rate);
    std::vector<std::int64_t> cuts;
    std::int64_t floor = 0;
    for (std::size_t index = 0; index < marks_s.size(); ++index) {
        const std::int64_t target =
            std::min(std::max(rounded_frame(marks_s[index], audio.rate), floor), total);
        std::int64_t ceiling = total;
        if (index + 1 < marks_s.size()) {
            ceiling =
                std::min(total, std::max(rounded_frame(marks_s[index + 1], audio.rate), target));
        }
        const std::int64_t low = std::max(floor, target - reach);
        const std::int64_t high = std::min(ceiling, target + reach);
        const std::int64_t cut = quietest(audio, low, high, target);
        cuts.push_back(cut);
        floor = cut;
    }
    return cuts;
}

std::int64_t quietest(const Pcm16& audio, std::int64_t low, std::int64_t high,
                      std::int64_t target) {
    const std::int64_t width =
        std::max<std::int64_t>(1, static_cast<std::int64_t>(FRAME_S * audio.rate));
    const std::int64_t at_target = std::min(std::max(target, low), high);
    if (high - low < width) {
        return at_target;
    }
    // Energy summed over the channels, through running sums in double: numpy's cumsum adds in
    // order, so the same sums come out.
    const auto channels = static_cast<std::size_t>(audio.channels);
    std::vector<double> sums(static_cast<std::size_t>(high - low) + 1, 0.0);
    for (std::int64_t frame = low; frame < high; ++frame) {
        double power = 0.0;
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const double sample =
                audio.samples[static_cast<std::size_t>(frame) * channels + channel];
            power += sample * sample;
        }
        const auto at = static_cast<std::size_t>(frame - low);
        sums[at + 1] = sums[at] + power;
    }
    const std::int64_t step = std::max<std::int64_t>(1, width / 2);
    std::int64_t best = -1;
    double best_energy = 0;
    std::int64_t best_distance = 0;
    std::int64_t nearest_energy_at = -1;
    double nearest_energy = 0;
    std::int64_t nearest_distance = 0;
    for (std::int64_t offset = 0; offset <= high - low - width; offset += step) {
        const double energy =
            sums[static_cast<std::size_t>(offset + width)] - sums[static_cast<std::size_t>(offset)];
        const std::int64_t middle = low + offset + width / 2;
        const std::int64_t distance = middle > target ? middle - target : target - middle;
        // np.lexsort((distance, energy))[0]: least energy, then least distance, then first.
        if (best < 0 || energy < best_energy ||
            (energy == best_energy && distance < best_distance)) {
            best = middle;
            best_energy = energy;
            best_distance = distance;
        }
        // np.argmin(distance): the first window nearest the target.
        if (nearest_energy_at < 0 || distance < nearest_distance) {
            nearest_energy_at = middle;
            nearest_energy = energy;
            nearest_distance = distance;
        }
    }
    if (best_energy > QUIET_RATIO * nearest_energy) {
        return at_target;
    }
    return best;
}

std::vector<std::pair<std::int64_t, std::int64_t>>
without(std::int64_t start, std::int64_t end,
        std::span<const std::pair<std::int64_t, std::int64_t>> gaps) {
    std::vector<Span> spans;
    std::int64_t at = start;
    for (const auto& [low, high] : gaps) {
        if (high <= at || low >= end) {
            continue;
        }
        if (low > at) {
            spans.emplace_back(at, low);
        }
        at = std::max(at, high);
    }
    if (at < end) {
        spans.emplace_back(at, end);
    }
    return spans;
}

std::vector<Clip> split_recording(const std::filesystem::path& wav, std::span<const double> cuts_s,
                                  const std::filesystem::path& out_dir,
                                  std::span<const std::pair<double, double>> leave_out_s) {
    const Pcm16 audio = read_pcm16_wav(wav);
    const std::vector<Span> gaps = frames_of(audio, leave_out_s);
    std::vector<std::int64_t> edges{0};
    const auto cuts = cut_frames(audio, cuts_s);
    edges.insert(edges.end(), cuts.begin(), cuts.end());
    edges.push_back(audio.frames());
    std::vector<Clip> clips;
    for (std::size_t index = 1; index < edges.size(); ++index) {
        clips.push_back(clip(audio, without(edges[index - 1], edges[index], gaps),
                             out_dir / path_from_utf8(std::format("section-{}.wav", index))));
    }
    return clips;
}

std::vector<Clip> clip_stretches(const std::filesystem::path& wav,
                                 std::span<const std::pair<double, double>> stretches_s,
                                 const std::filesystem::path& out_dir) {
    const Pcm16 audio = read_pcm16_wav(wav);
    std::vector<Clip> clips;
    std::size_t index = 0;
    for (const auto& span : frames_of(audio, stretches_s)) {
        clips.push_back(
            clip(audio, {span}, out_dir / path_from_utf8(std::format("note-{}.wav", ++index))));
    }
    return clips;
}

} // namespace fv::walkthrough
