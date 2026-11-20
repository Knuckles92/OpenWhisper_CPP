#include "walkthrough/detail/patterns.h"

namespace fv::walkthrough::detail {

const py::Regex& image_pattern() {
    static const py::Regex regex(
        R"re(!\[([^\]\n]*)\]\(\s*(<[^>\n]*>|[^\s)]+)(?:\s+"[^"\n]*")?\s*\))re");
    return regex;
}

const py::Regex& shot_pattern() {
    static const py::Regex regex(R"(shot-[1-9]\d*\.png\Z)");
    return regex;
}

} // namespace fv::walkthrough::detail
