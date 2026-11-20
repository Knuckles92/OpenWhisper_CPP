// Chat Completions streamed (`stream: true`, `stream_options: {include_usage: true}`): chunks of
// `choices[0].delta` with content, reasoning and indexed tool-call fragments, a finish_reason,
// usage in a last chunk without choices, and "[DONE]". The reply is the chat.completion that
// text_generation.py's _chat reads (text_generation.py:83-114).

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "llm/detail/stream_protocol.h"
#include "llm/detail/tool_call_accumulator.h"
#include "llm/detail/truthy.h"
#include "pyport/json.h"

namespace fv::llm::detail {

namespace {

// The string fields of one reasoning_details entry that arrive in pieces.
constexpr std::array<std::string_view, 3> kDetailTextFields{"text", "summary", "data"};

class ChatAccumulator final : public StreamAccumulator {
public:
    void on_event(const SseEvent& event, const EmitDelta& emit) override {
        if (done_) {
            return;
        }
        if (event.data == "[DONE]") {
            done_ = true;
            return;
        }
        const Json chunk = event_json(event);
        if (!chunk.is_object()) {
            return;
        }
        if (const auto error = chunk.find("error"); error != chunk.end() && truthy(*error)) {
            // OpenRouter reports a provider failure after the 200 as a chunk with an error.
            std::optional<int> status;
            if (error->is_object()) {
                if (const auto code = error->find("code");
                    code != error->end() && code->is_number_integer() &&
                    code->get<std::int64_t>() >= 100 && code->get<std::int64_t>() <= 599) {
                    status = static_cast<int>(code->get<std::int64_t>());
                } else if (code != error->end() && code->is_string()) {
                    status = error_type_status(code->get<std::string>());
                }
            }
            throw_stream_failure(status, chunk);
        }
        if (const auto usage = chunk.find("usage"); usage != chunk.end() && !usage->is_null()) {
            usage_ = *usage;
        }
        const Json* choice = first_choice(chunk);
        if (choice == nullptr) {
            return;
        }
        had_choice_ = true;
        if (chunk.find("usage") == chunk.end() || chunk.at("usage").is_null()) {
            // Some providers (Moonshot) put the usage on the choice.
            if (const auto usage = choice->find("usage");
                usage != choice->end() && !usage->is_null()) {
                usage_ = *usage;
            }
        }
        if (const auto reason = choice->find("finish_reason");
            reason != choice->end() && !reason->is_null()) {
            finish_ = *reason;
        }
        const auto delta = choice->find("delta");
        if (delta == choice->end() || !delta->is_object()) {
            return;
        }
        read_delta(*delta, emit);
    }

    // A finish_reason, whether or not [DONE] follows. A stream that delivered a choice and then
    // ended with [DONE] without ever saying why the reply stopped is as incomplete as one cut off
    // (pi-ai raises for it); [DONE] after no choice at all is an empty reply, which generate()
    // reads as one.
    bool complete() const override { return !finish_.is_null() || (done_ && !had_choice_); }

    Json reply(const EmitDelta& emit) override {
        end_thinking(emit);
        for (const auto& call : calls_.calls()) {
            emit(stream_event(StreamDelta::ToolCallEnd, {}, call.id, call.name));
        }
        Json message = Json::object();
        message["role"] = "assistant";
        message["content"] = content_seen_ ? Json(content_) : Json();
        if (refusal_seen_) {
            message["refusal"] = refusal_;
        }
        if (!calls_.empty()) {
            Json list = Json::array();
            for (const auto& call : calls_.calls()) {
                list.push_back(Json{
                    {"id", call.id},
                    {"type", "function"},
                    {"function", Json{{"name", call.name},
                                      {"arguments", ToolCallAccumulator::final_arguments(call)}}}});
            }
            message["tool_calls"] = std::move(list);
        }
        if (reasoning_content_) {
            message["reasoning_content"] = *reasoning_content_;
        }
        if (reasoning_) {
            message["reasoning"] = *reasoning_;
        }
        if (reasoning_details_.is_array()) {
            message["reasoning_details"] = reasoning_details_;
        }
        Json reply = Json::object();
        reply["choices"] = Json::array();
        if (had_choice_) {
            reply["choices"].push_back(
                Json{{"index", 0}, {"message", std::move(message)}, {"finish_reason", finish_}});
        }
        if (!usage_.is_null()) {
            reply["usage"] = usage_;
        }
        return reply;
    }

private:
    static const Json* first_choice(const Json& chunk) {
        const auto choices = chunk.find("choices");
        if (choices == chunk.end() || !choices->is_array()) {
            return nullptr;
        }
        for (const auto& choice : *choices) {
            if (choice.is_object() && choice.value("index", Json(0)) == Json(0)) {
                return &choice;
            }
        }
        return nullptr;
    }

    void read_delta(const Json& delta, const EmitDelta& emit) {
        // The first non-empty reasoning field is the thinking text, as pi-ai reads it: some
        // gateways send the same text in two of them.
        std::string thought;
        for (const std::string_view key : {"reasoning_content", "reasoning", "reasoning_text"}) {
            const auto found = delta.find(key);
            if (found == delta.end() || !found->is_string()) {
                continue;
            }
            const auto& text = found->get_ref<const std::string&>();
            if (key == "reasoning_content") {
                reasoning_content_ = reasoning_content_.value_or("") + text;
            } else if (key == "reasoning") {
                reasoning_ = reasoning_.value_or("") + text;
            }
            if (thought.empty()) {
                thought = text;
            }
        }
        if (const auto details = delta.find("reasoning_details");
            details != delta.end() && details->is_array()) {
            // OpenRouter sends the same text as `reasoning` beside its details.
            const bool from_strings = !thought.empty();
            for (const auto& detail : *details) {
                const std::string text = merge_detail(detail);
                if (!from_strings) {
                    thought += text;
                }
            }
        }
        if (!thought.empty()) {
            if (!thinking_) {
                thinking_ = true;
                emit(stream_event(StreamDelta::ThinkingStart));
            }
            emit(stream_event(StreamDelta::ThinkingDelta, thought));
        }
        if (const auto content = delta.find("content"); content != delta.end()) {
            if (content->is_string()) {
                const auto& text = content->get_ref<const std::string&>();
                content_seen_ = true;
                content_ += text;
                if (!text.empty()) {
                    end_thinking(emit);
                    emit(stream_event(StreamDelta::TextDelta, text));
                }
            }
        }
        if (const auto refusal = delta.find("refusal");
            refusal != delta.end() && refusal->is_string()) {
            refusal_seen_ = true;
            refusal_ += refusal->get_ref<const std::string&>();
        }
        if (const auto tools = delta.find("tool_calls");
            tools != delta.end() && tools->is_array()) {
            for (const auto& call : *tools) {
                read_tool_call(call, emit);
            }
        }
    }

    // Streamed reasoning_details entries are pieces of the entries the non-streamed message
    // holds: an entry with the type and index of an earlier one continues it.
    std::string merge_detail(const Json& detail) {
        if (!reasoning_details_.is_array()) {
            reasoning_details_ = Json::array();
        }
        std::string text;
        if (!detail.is_object()) {
            reasoning_details_.push_back(detail);
            return text;
        }
        for (const auto field : kDetailTextFields) {
            if (const auto found = detail.find(field);
                found != detail.end() && found->is_string()) {
                if (field != "data") {
                    text += found->get_ref<const std::string&>();
                }
            }
        }
        const auto index = detail.find("index");
        Json* target = nullptr;
        if (index != detail.end() && !index->is_null()) {
            for (auto& entry : reasoning_details_) {
                if (entry.is_object() && entry.value("index", Json()) == *index &&
                    entry.value("type", Json()) == detail.value("type", Json())) {
                    target = &entry;
                }
            }
        }
        if (target == nullptr) {
            reasoning_details_.push_back(detail);
            return text;
        }
        for (const auto& [key, value] : detail.items()) {
            const bool joined =
                std::ranges::find(kDetailTextFields, key) != kDetailTextFields.end();
            const auto existing = target->find(key);
            if (joined && value.is_string() && existing != target->end() && existing->is_string()) {
                *existing = existing->get<std::string>() + value.get<std::string>();
            } else if (!value.is_null() || existing == target->end()) {
                (*target)[key] = value;
            }
        }
        return text;
    }

    void read_tool_call(const Json& call, const EmitDelta& emit) {
        if (!call.is_object()) {
            return;
        }
        std::optional<std::int64_t> key;
        if (const auto index = call.find("index");
            index != call.end() && index->is_number_integer()) {
            key = index->get<std::int64_t>();
        }
        const std::string id = string_at(call, "id");
        std::string name;
        std::string arguments;
        if (const auto function = call.find("function");
            function != call.end() && function->is_object()) {
            name = string_at(*function, "name");
            if (const auto args = function->find("arguments"); args != function->end()) {
                arguments = args->is_string() ? args->get<std::string>()
                            : args->is_null() ? std::string()
                                              : py::dumps(*args);
            }
        }
        const auto added = calls_.add(key, id, name, arguments);
        const auto& stored = calls_.calls()[added.position];
        if (added.started) {
            end_thinking(emit);
            emit(stream_event(StreamDelta::ToolCallStart, {}, stored.id, stored.name));
        }
        if (!arguments.empty()) {
            emit(stream_event(StreamDelta::ToolCallDelta, arguments, stored.id, stored.name));
        }
    }

    void end_thinking(const EmitDelta& emit) {
        if (thinking_) {
            thinking_ = false;
            emit(stream_event(StreamDelta::ThinkingEnd));
        }
    }

    bool done_ = false;
    bool had_choice_ = false;
    bool thinking_ = false;
    Json finish_;
    Json usage_;
    bool content_seen_ = false;
    std::string content_;
    bool refusal_seen_ = false;
    std::string refusal_;
    std::optional<std::string> reasoning_content_;
    std::optional<std::string> reasoning_;
    Json reasoning_details_; // null until an entry arrives
    ToolCallAccumulator calls_;
};

} // namespace

std::unique_ptr<StreamAccumulator> make_chat_accumulator() {
    return std::make_unique<ChatAccumulator>();
}

} // namespace fv::llm::detail
