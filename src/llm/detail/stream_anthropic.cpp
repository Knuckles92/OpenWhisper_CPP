// Anthropic Messages streamed (`stream: true`): message_start with the message and its usage,
// content blocks started, grown by deltas and stopped, message_delta with the stop reason and
// final usage, message_stop. The reply is the message text_generation.py's _anthropic reads
// (text_generation.py:163-207); its content is the `_anthropic` replay, so a thinking block keeps
// the signature its signature_delta brought.

#include <cstdint>
#include <exception>
#include <map>
#include <string>
#include <utility>

#include "llm/detail/stream_protocol.h"
#include "llm/detail/tool_call_accumulator.h"
#include "llm/detail/truthy.h"
#include "pyport/json.h"

namespace fv::llm::detail {

namespace {

struct Block {
    Json content;           // the block as content_block_start gave it, grown by the deltas
    std::string input_json; // a tool_use block's input_json_delta fragments, joined
    bool has_input_json = false;
    bool open = true;
};

std::string joined(const Json& existing, const std::string& more) {
    return (existing.is_string() ? existing.get<std::string>() : std::string()) + more;
}

class AnthropicAccumulator final : public StreamAccumulator {
public:
    void on_event(const SseEvent& event, const EmitDelta& emit) override {
        if (stopped_) {
            return;
        }
        const Json data = event_json(event);
        if (!data.is_object()) {
            return;
        }
        const std::string* named = text(get(data, "type"));
        const std::string type = named != nullptr ? *named : event.event;
        const std::int64_t index =
            get(data, "index").is_number_integer() ? data.at("index").get<std::int64_t>() : 0;
        if (type == "message_start") {
            if (get(data, "message").is_object()) {
                message_ = data.at("message");
            }
        } else if (type == "content_block_start") {
            start_block(index, get(data, "content_block"), emit);
        } else if (type == "content_block_delta") {
            grow_block(index, get(data, "delta"), emit);
        } else if (type == "content_block_stop") {
            if (const auto found = blocks_.find(index); found != blocks_.end()) {
                stop_block(found->second, emit);
            }
        } else if (type == "message_delta") {
            if (const Json& delta = get(data, "delta"); delta.is_object()) {
                for (const auto& [key, value] : delta.items()) {
                    message_[key] = value;
                }
            }
            if (const Json& usage = get(data, "usage"); usage.is_object()) {
                if (!message_["usage"].is_object()) {
                    message_["usage"] = Json::object();
                }
                for (const auto& [key, value] : usage.items()) {
                    if (!value.is_null()) {
                        message_["usage"][key] = value;
                    }
                }
            }
        } else if (type == "message_stop") {
            stopped_ = true;
        } else if (type == "error") {
            throw_stream_failure(error_type_status(string_at(get(data, "error"), "type")), data);
        }
    }

    bool complete() const override { return stopped_; }

    Json reply(const EmitDelta& emit) override {
        Json content = Json::array();
        for (auto& entry : blocks_) {
            stop_block(entry.second, emit);
            content.push_back(entry.second.content);
        }
        Json reply = message_.is_object() ? message_ : Json::object();
        reply["content"] = std::move(content);
        return reply;
    }

private:
    void start_block(std::int64_t index, const Json& content, const EmitDelta& emit) {
        Block& block = blocks_[index];
        block = Block{content.is_object() ? content : Json::object(), {}, false, true};
        const std::string type = string_at(block.content, "type");
        if (type == "thinking") {
            emit(stream_event(StreamDelta::ThinkingStart));
            if (const std::string thought = string_at(block.content, "thinking");
                !thought.empty()) {
                emit(stream_event(StreamDelta::ThinkingDelta, thought));
            }
        } else if (type == "tool_use") {
            const auto added = calls_.add(index, string_at(block.content, "id"),
                                          string_at(block.content, "name"), {});
            const auto& call = calls_.calls()[added.position];
            emit(stream_event(StreamDelta::ToolCallStart, {}, call.id, call.name));
        } else if (type == "text") {
            if (const std::string start = string_at(block.content, "text"); !start.empty()) {
                emit(stream_event(StreamDelta::TextDelta, start));
            }
        }
    }

    void grow_block(std::int64_t index, const Json& delta, const EmitDelta& emit) {
        const auto found = blocks_.find(index);
        if (found == blocks_.end() || !delta.is_object()) {
            return;
        }
        Block& block = found->second;
        Json& content = block.content;
        const std::string type = string_at(delta, "type");
        if (type == "text_delta") {
            const std::string more = string_at(delta, "text");
            content["text"] = joined(get(content, "text"), more);
            if (!more.empty()) {
                emit(stream_event(StreamDelta::TextDelta, more));
            }
        } else if (type == "thinking_delta") {
            const std::string more = string_at(delta, "thinking");
            content["thinking"] = joined(get(content, "thinking"), more);
            emit(stream_event(StreamDelta::ThinkingDelta, more));
        } else if (type == "signature_delta") {
            // The signature belongs to the open thinking block and is replayed with it.
            content["signature"] = joined(get(content, "signature"), string_at(delta, "signature"));
        } else if (type == "input_json_delta") {
            const std::string more = string_at(delta, "partial_json");
            block.input_json += more;
            block.has_input_json = true;
            const auto added = calls_.add(index, {}, {}, more);
            const auto& call = calls_.calls()[added.position];
            if (!more.empty()) {
                emit(stream_event(StreamDelta::ToolCallDelta, more, call.id, call.name));
            }
        } else if (type == "citations_delta") {
            if (!get(content, "citations").is_array()) {
                content["citations"] = Json::array();
            }
            content["citations"].push_back(get(delta, "citation"));
        }
    }

    void stop_block(Block& block, const EmitDelta& emit) {
        if (!block.open) {
            return;
        }
        block.open = false;
        const std::string type = string_at(block.content, "type");
        if (type == "thinking") {
            emit(stream_event(StreamDelta::ThinkingEnd));
        } else if (type == "tool_use") {
            if (block.has_input_json) {
                // The fragments make one JSON object only once joined; none means {}.
                try {
                    block.content["input"] =
                        py::loads(block.input_json.empty() ? std::string("{}") : block.input_json);
                } catch (const std::exception&) {
                    StreamError error{std::string(kStreamMalformedError)};
                    throw error;
                }
            }
            emit(stream_event(StreamDelta::ToolCallEnd, {}, string_at(block.content, "id"),
                              string_at(block.content, "name")));
        }
    }

    bool stopped_ = false;
    Json message_ = Json::object();
    std::map<std::int64_t, Block> blocks_; // by content index, the reply's order
    ToolCallAccumulator calls_;
};

} // namespace

std::unique_ptr<StreamAccumulator> make_anthropic_accumulator() {
    return std::make_unique<AnthropicAccumulator>();
}

} // namespace fv::llm::detail
