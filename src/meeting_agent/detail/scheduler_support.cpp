#include "meeting_agent/detail/scheduler_support.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <system_error>

#include "meeting_agent/detail/py_value.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::meeting_agent::detail {

namespace {

// The bounds of one cleanup window (meeting/finalization.py POLISH_MAX_SEGMENTS and
// POLISH_MAX_TEXT_CHARS), in rows and code points.
constexpr std::size_t kPolishMaxSegments = 80;
constexpr std::size_t kPolishMaxTextChars = 8'000;

double parse_float(std::string_view text) {
    const std::string stripped = py::strip(text);
    double value = 0.0;
    const auto* first = stripped.data();
    const auto* last = stripped.data() + stripped.size();
    if (first != last && *first == '+') {
        ++first;
    }
    const auto [end, error] = std::from_chars(first, last, value);
    if (error != std::errc() || end != last) {
        return 0.0;
    }
    return value;
}

} // namespace

double row_start_s(const Json& row) {
    const Json& value = get(row, "start_s");
    if (!py_truthy(value)) {
        return 0.0;
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? 1.0 : 0.0;
    }
    if (value.is_number()) {
        return value.get<double>();
    }
    if (value.is_string()) {
        return parse_float(value.get_ref<const std::string&>());
    }
    return 0.0;
}

std::string row_id(const Json& row) {
    return str_or_empty(get(row, "id"));
}

std::string row_text(const Json& row) {
    return str_or_empty(get(row, "text"));
}

std::size_t applied_count(const AgentResult& result) {
    return static_cast<std::size_t>(
        std::count_if(result.op_results.begin(), result.op_results.end(),
                      [](const meeting_state::OpResult& op) { return op.ok; }));
}

bool revised_transcript(const AgentResult& result) {
    return std::any_of(result.op_results.begin(), result.op_results.end(),
                       [](const meeting_state::OpResult& op) {
                           return op.ok && is_text(get(op.op, "op"), "revise_segment_text");
                       });
}

std::size_t first_polish_block_size(const std::vector<Json>& segments) {
    std::size_t end = 0;
    std::size_t chars = 0;
    while (end < segments.size() && end < kPolishMaxSegments) {
        const std::size_t size = py::len(row_text(segments[end]));
        if (end > 0 && chars + size > kPolishMaxTextChars) {
            break;
        }
        chars += size;
        ++end;
    }
    return end;
}

std::string fixed(double value, int digits) {
    return py::format_fixed(value, digits);
}

} // namespace fv::meeting_agent::detail
