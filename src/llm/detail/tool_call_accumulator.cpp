#include "llm/detail/tool_call_accumulator.h"

#include <algorithm>

namespace fv::llm::detail {

ToolCallAccumulator::Added ToolCallAccumulator::add(std::optional<std::int64_t> key,
                                                    std::string_view id, std::string_view name,
                                                    std::string_view arguments) {
    auto found = calls_.end();
    if (key) {
        found = std::ranges::find(calls_, key, &StreamedToolCall::key);
    } else if (!id.empty()) {
        found = std::ranges::find(calls_, id, &StreamedToolCall::id);
    } else if (!calls_.empty()) {
        found = calls_.end() - 1;
    }
    bool started = false;
    if (found == calls_.end()) {
        calls_.push_back(StreamedToolCall{key, {}, {}, {}});
        found = calls_.end() - 1;
        started = true;
    }
    if (found->id.empty()) {
        found->id = id;
    }
    if (found->name.empty()) {
        found->name = name;
    }
    found->arguments.append(arguments);
    return {static_cast<std::size_t>(found - calls_.begin()), started};
}

std::string ToolCallAccumulator::final_arguments(const StreamedToolCall& call) {
    return call.arguments.empty() ? std::string("{}") : call.arguments;
}

} // namespace fv::llm::detail
