// The Responses API streamed (`stream: true`): output items added and done, text, reasoning and
// argument deltas, and a terminal response.completed, response.incomplete or response.failed
// whose `response` is the whole reply text_generation.py's _responses reads
// (text_generation.py:115-162). That `output` stays the replay (`_responses`), encrypted
// reasoning included; nothing is rebuilt from the deltas.

#include <cstdint>
#include <map>
#include <string>

#include "llm/detail/stream_protocol.h"
#include "llm/detail/truthy.h"

namespace fv::llm::detail {

namespace {

struct OpenItem {
    std::string type;
    std::string call_id;
    std::string name;
    bool open = true;
};

class ResponsesAccumulator final : public StreamAccumulator {
public:
    void on_event(const SseEvent& event, const EmitDelta& emit) override {
        if (complete_) {
            return;
        }
        const Json data = event_json(event);
        if (!data.is_object()) {
            return;
        }
        const std::string* named = text(get(data, "type"));
        const std::string type = named != nullptr ? *named : event.event;
        const std::int64_t index = get(data, "output_index").is_number_integer()
                                       ? data.at("output_index").get<std::int64_t>()
                                       : -1;
        if (type == "response.output_item.added") {
            const Json& item = get(data, "item");
            OpenItem& open = items_[index];
            open = OpenItem{string_at(item, "type"), string_at(item, "call_id"),
                            string_at(item, "name"), true};
            if (open.type == "reasoning") {
                emit(stream_event(StreamDelta::ThinkingStart));
            } else if (open.type == "function_call") {
                emit(stream_event(StreamDelta::ToolCallStart, {}, open.call_id, open.name));
                if (const std::string arguments = string_at(item, "arguments");
                    !arguments.empty()) {
                    emit(stream_event(StreamDelta::ToolCallDelta, arguments, open.call_id,
                                      open.name));
                }
            }
        } else if (type == "response.reasoning_summary_text.delta" ||
                   type == "response.reasoning_text.delta") {
            OpenItem& open = items_[index];
            if (open.type.empty()) {
                // A delta whose item was never announced still reads as thinking.
                open = OpenItem{"reasoning", {}, {}, true};
                emit(stream_event(StreamDelta::ThinkingStart));
            }
            emit(stream_event(StreamDelta::ThinkingDelta, string_at(data, "delta")));
        } else if (type == "response.output_text.delta") {
            if (const std::string delta = string_at(data, "delta"); !delta.empty()) {
                emit(stream_event(StreamDelta::TextDelta, delta));
            }
        } else if (type == "response.function_call_arguments.delta") {
            const OpenItem& open = items_[index];
            emit(stream_event(StreamDelta::ToolCallDelta, string_at(data, "delta"), open.call_id,
                              open.name));
        } else if (type == "response.output_item.done") {
            const auto found = items_.find(index);
            if (found != items_.end()) {
                close(found->second, emit);
            }
        } else if (type == "response.failed" && failed_with_status(get(data, "response"))) {
            const Json& error = get(get(data, "response"), "error");
            throw_stream_failure(error_type_status(string_at(error, "code")),
                                 Json{{"error", error}});
        } else if (type == "response.completed" || type == "response.incomplete" ||
                   type == "response.failed") {
            final_ = get(data, "response");
            complete_ = true;
        } else if (type == "error") {
            // OpenAI's error event: {"type": "error", "code", "message", "param"}.
            const Json& error = has(data, "error") ? data.at("error") : data;
            std::string code = string_at(error, "code");
            if (code.empty()) {
                code = string_at(error, "type");
            }
            throw_stream_failure(error_type_status(code), data);
        }
    }

    bool complete() const override { return complete_; }

    Json reply(const EmitDelta& emit) override {
        for (auto& entry : items_) {
            close(entry.second, emit);
        }
        return final_.is_null() ? Json::object() : final_;
    }

private:
    // A failed response whose error code names a status ("server_error", "rate_limit_exceeded"):
    // a provider failure the agent loop may retry, not a reply. Without one the reply goes to
    // generate(), which raises "Model output was incomplete or failed."
    static bool failed_with_status(const Json& response) {
        const Json& error = get(response, "error");
        return error.is_object() && error_type_status(string_at(error, "code")).has_value();
    }

    static void close(OpenItem& item, const EmitDelta& emit) {
        if (!item.open) {
            return;
        }
        item.open = false;
        if (item.type == "reasoning") {
            emit(stream_event(StreamDelta::ThinkingEnd));
        } else if (item.type == "function_call") {
            emit(stream_event(StreamDelta::ToolCallEnd, {}, item.call_id, item.name));
        }
    }

    bool complete_ = false;
    Json final_;
    std::map<std::int64_t, OpenItem> items_; // by output_index
};

} // namespace

std::unique_ptr<StreamAccumulator> make_responses_accumulator() {
    return std::make_unique<ResponsesAccumulator>();
}

} // namespace fv::llm::detail
