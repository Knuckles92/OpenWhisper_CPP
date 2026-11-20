#include "uimath/sample_voice.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

#include "base/json.h"
#include "uimath/data/sample_voice.json.h"

namespace fv {
namespace {

std::vector<SampleWord> parse_words() {
    const Json document = Json::parse(fv::embedded::uimath_sample_voice);
    if (document.at("loop_s").get<double>() != kLoopS) {
        throw std::runtime_error("sample_voice.json: loop_s differs from uimath/sample_voice.h");
    }
    const Json& stages = document.at("stages");
    if (!stages.is_array() || stages.size() != kStages.size()) {
        throw std::runtime_error("sample_voice.json: expected four stages");
    }
    for (std::size_t i = 0; i < kStages.size(); ++i) {
        if (stages[i].at(0).get<std::string>() != kStages[i].name ||
            stages[i].at(1).get<double>() != kStages[i].start) {
            throw std::runtime_error("sample_voice.json: stages differ from uimath/sample_voice.h");
        }
    }
    const Json& words = document.at("words");
    if (!words.is_array() || words.empty()) {
        throw std::runtime_error("sample_voice.json: no words");
    }
    std::vector<SampleWord> result;
    for (const Json& word : words) {
        if (!word.is_array() || word.size() != 3) {
            throw std::runtime_error("sample_voice.json: a word is not [start, length, peak]");
        }
        result.push_back(
            SampleWord{word[0].get<double>(), word[1].get<double>(), word[2].get<double>()});
    }
    return result;
}

} // namespace

const std::vector<SampleWord>& sample_words() {
    static const std::vector<SampleWord> words = parse_words();
    return words;
}

double sample_level(double t) {
    // Python's t %= LOOP_S: the result takes the sign of the divisor.
    t = std::fmod(t, kLoopS);
    if (t < 0.0) {
        t += kLoopS;
    }
    double level = 0.0025 + 0.001 * std::sin(t * 37.0);
    for (const SampleWord& word : sample_words()) {
        if (word.start <= t && t <= word.start + word.length) {
            // Rounding can leave the sine a hair below 0 at a word's very end.
            const double shape =
                std::max(0.0, std::sin(std::numbers::pi * (t - word.start) / word.length));
            level = std::max(level, word.peak * std::pow(shape, 0.7));
        }
    }
    return level;
}

} // namespace fv
