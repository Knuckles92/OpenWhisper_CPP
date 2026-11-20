#include "uimath/tidy_lines.h"

#include <stdexcept>

#include "base/json.h"
#include "uimath/data/overlay_tables.json.h"

namespace fv {
namespace {

std::array<ScribbleLine, 3> parse_lines() {
    const Json document = Json::parse(fv::embedded::uimath_overlay_tables);
    // The table was laid out for these two constants; a regenerated table from a checkout that
    // changed them must not be read against the old ones.
    if (document.at("half_span").get<double>() != kTidyHalfSpan ||
        document.at("word_gap").get<double>() != kTidyWordGap) {
        throw std::runtime_error(
            "overlay_tables.json: half_span or word_gap differs from uimath/tidy_lines.h");
    }
    const Json& lines = document.at("lines");
    if (!lines.is_array() || lines.size() != 3) {
        throw std::runtime_error("overlay_tables.json: expected three scribbled lines");
    }
    std::array<ScribbleLine, 3> result;
    for (std::size_t row = 0; row < 3; ++row) {
        if (!lines[row].is_array() || lines[row].empty()) {
            throw std::runtime_error("overlay_tables.json: a line is not a list of words");
        }
        for (const Json& word : lines[row]) {
            if (!word.is_array() || word.size() != 4) {
                throw std::runtime_error(
                    "overlay_tables.json: a word is not [length, dy, dx, phase]");
            }
            result[row].push_back(ScribbleWord{word[0].get<double>(), word[1].get<double>(),
                                               word[2].get<double>(), word[3].get<double>()});
        }
    }
    return result;
}

} // namespace

const std::array<ScribbleLine, 3>& scribble_lines() {
    static const std::array<ScribbleLine, 3> lines = parse_lines();
    return lines;
}

} // namespace fv
