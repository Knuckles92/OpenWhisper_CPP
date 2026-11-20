#include "speech/window_decode.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "dsp/window_splitter.h"
#include "pyport/text.h"

namespace fv::speech {

bool audible(std::span<const float> audio) {
    if (audio.empty()) {
        return false;
    }
    float peak = 0.0f;
    for (const float sample : audio) {
        if (std::isnan(sample)) {
            return false; // np.max propagates NaN, and NaN > x is false
        }
        peak = std::max(peak, std::fabs(sample));
    }
    return peak > kSilencePeak;
}

std::string join_texts(std::span<const std::string> texts) {
    std::string joined;
    for (const std::string& text : texts) {
        std::string stripped = py::strip(text);
        if (stripped.empty()) {
            continue;
        }
        if (!joined.empty()) {
            joined += ' ';
        }
        joined += stripped;
    }
    return joined;
}

Transcript transcribe_audio(std::span<const float> audio, const WindowRecognizer& recognize,
                            const std::function<void()>& check_cancel) {
    Transcript result;
    std::vector<std::string> texts;
    std::size_t start = 0;
    while (start < audio.size()) {
        check_cancel();
        const std::size_t end = start + dsp::split_point(audio.subspan(start));
        const std::span<const float> window = audio.subspan(start, end - start);
        if (audible(window)) {
            Transcript piece = recognize(window);
            texts.push_back(std::move(piece.text));
            // Python adds start / 16000 (a float) to each time.
            const double shift = static_cast<double>(start) / 16000.0;
            for (Segment& segment : piece.segments) {
                segment.start += shift;
                segment.end += shift;
                result.segments.push_back(std::move(segment));
            }
            for (Word& word : piece.words) {
                word.start += shift;
                word.end += shift;
                result.words.push_back(std::move(word));
            }
        }
        start = end;
    }
    result.text = join_texts(texts);
    result.audio_processed = static_cast<double>(audio.size()) / 16000.0;
    return result;
}

} // namespace fv::speech
