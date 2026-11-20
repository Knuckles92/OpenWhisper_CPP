#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Tool calls assembled from streamed fragments. Chat Completions keys each call by `index`; its
// `id` and `function.name` come only in the call's first chunk, and the arguments arrive as text
// fragments that only make JSON once joined. Anthropic keys tool_use blocks by content index.
namespace fv::llm::detail {

struct StreamedToolCall {
    std::optional<std::int64_t> key; // the stream's index for the call
    std::string id;
    std::string name;
    std::string arguments; // the fragments joined, as sent
};

class ToolCallAccumulator {
public:
    struct Added {
        std::size_t position; // in calls()
        bool started;         // this fragment began the call
    };

    // One fragment: the call with `key`; without a key the call with `id`; without either the
    // last call. A call's id and name are the first non-empty ones it is given.
    Added add(std::optional<std::int64_t> key, std::string_view id, std::string_view name,
              std::string_view arguments);

    const std::vector<StreamedToolCall>& calls() const { return calls_; }
    bool empty() const { return calls_.empty(); }

    // The arguments a call ends with: the joined text, "{}" when no fragment carried any.
    static std::string final_arguments(const StreamedToolCall& call);

private:
    std::vector<StreamedToolCall> calls_;
};

} // namespace fv::llm::detail
