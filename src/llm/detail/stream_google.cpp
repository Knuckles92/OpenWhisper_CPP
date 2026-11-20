// Gemini's generateContent streamed through `:streamGenerateContent?alt=sse`: each event is a
// GenerateContentResponse carrying the next parts of candidate 0, the finishReason on the last,
// and usageMetadata (the last one counts); a stream without a finishReason was cut short. The reply
// is the response text_generation.py's _google reads (text_generation.py:208-279); its parts are
// the `_google` replay. Consecutive text parts of the same kind (thought or not) are one part,
// keeping the last thoughtSignature, as pi-ai assembles them; function calls and other parts are
// kept as they came.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "llm/detail/stream_protocol.h"
#include "llm/detail/truthy.h"
#include "pyport/json.h"

namespace fv::llm::detail {

namespace {

bool is_text_part(const Json& part) {
    return part.is_object() && has(part, "text") && part.at("text").is_string() &&
           !truthy(get(part, "functionCall"));
}

class GoogleAccumulator final : public StreamAccumulator {
public:
    void on_event(const SseEvent& event, const EmitDelta& emit) override {
        const Json data = event_json(event);
        if (!data.is_object()) {
            return;
        }
        if (const Json& error = get(data, "error"); truthy(error)) {
            std::optional<int> status;
            if (const Json& code = get(error, "code"); code.is_number_integer() &&
                                                       code.get<std::int64_t>() >= 100 &&
                                                       code.get<std::int64_t>() <= 599) {
                status = static_cast<int>(code.get<std::int64_t>());
            }
            throw_stream_failure(status, data);
        }
        if (has(data, "usageMetadata") && !data.at("usageMetadata").is_null()) {
            usage_ = data.at("usageMetadata");
        }
        if (has(data, "promptFeedback")) {
            prompt_feedback_ = data.at("promptFeedback");
        }
        const Json& candidates = get(data, "candidates");
        if (!candidates.is_array() || candidates.empty() || !candidates.front().is_object()) {
            return;
        }
        had_candidate_ = true;
        const Json& candidate = candidates.front();
        for (const auto& [key, value] : candidate.items()) {
            if (key != "content") {
                fields_[key] = value;
            }
        }
        if (truthy(get(candidate, "finishReason"))) {
            finished_ = true;
        }
        const Json& content = get(candidate, "content");
        if (const Json& role = get(content, "role"); role.is_string()) {
            role_ = role;
        }
        const Json& parts = get(content, "parts");
        if (!parts.is_array()) {
            return;
        }
        for (const auto& part : parts) {
            add_part(part, emit);
        }
    }

    // Gemini's last chunk carries the candidate's finishReason; a stream that closes before one
    // came was cut short (pi-ai's google-generative-ai.js: "Google stream ended without a finish
    // reason"). A prompt blocked outright answers with promptFeedback and no candidate.
    bool complete() const override {
        return finished_ || (!had_candidate_ && !prompt_feedback_.is_null());
    }

    Json reply(const EmitDelta& emit) override {
        end_thinking(emit);
        Json reply = Json::object();
        reply["candidates"] = Json::array();
        if (had_candidate_) {
            Json candidate = Json::object();
            candidate["content"] = Json{{"role", role_}, {"parts", parts_}};
            for (const auto& [key, value] : fields_.items()) {
                candidate[key] = value;
            }
            reply["candidates"].push_back(std::move(candidate));
        }
        if (!prompt_feedback_.is_null()) {
            reply["promptFeedback"] = prompt_feedback_;
        }
        if (!usage_.is_null()) {
            reply["usageMetadata"] = usage_;
        }
        return reply;
    }

private:
    void add_part(const Json& part, const EmitDelta& emit) {
        if (is_text_part(part)) {
            const bool thought = truthy(get(part, "thought"));
            const std::string& more = part.at("text").get_ref<const std::string&>();
            if (!parts_.empty() && is_text_part(parts_.back()) &&
                truthy(get(parts_.back(), "thought")) == thought) {
                Json& last = parts_.back();
                last["text"] = last.at("text").get<std::string>() + more;
                if (const Json& signature = get(part, "thoughtSignature"); truthy(signature)) {
                    last["thoughtSignature"] = signature;
                }
            } else {
                parts_.push_back(part);
            }
            if (thought) {
                if (!thinking_) {
                    thinking_ = true;
                    emit(stream_event(StreamDelta::ThinkingStart));
                }
                emit(stream_event(StreamDelta::ThinkingDelta, more));
            } else {
                // Thought parts never join the text.
                end_thinking(emit);
                if (!more.empty()) {
                    emit(stream_event(StreamDelta::TextDelta, more));
                }
            }
            return;
        }
        parts_.push_back(part);
        const Json& call = get(part, "functionCall");
        if (!truthy(call)) {
            return;
        }
        end_thinking(emit);
        // The id generate() gives a call without one: its position among the parts.
        std::string id = string_at(call, "id");
        if (id.empty()) {
            id = "call_" + std::to_string(parts_.size() - 1);
        }
        const std::string name = string_at(call, "name");
        emit(stream_event(StreamDelta::ToolCallStart, {}, id, name));
        emit(stream_event(StreamDelta::ToolCallDelta,
                          py::dumps(get(call, "args").is_null() ? Json::object() : call.at("args")),
                          id, name));
        emit(stream_event(StreamDelta::ToolCallEnd, {}, id, name));
    }

    void end_thinking(const EmitDelta& emit) {
        if (thinking_) {
            thinking_ = false;
            emit(stream_event(StreamDelta::ThinkingEnd));
        }
    }

    bool had_candidate_ = false;
    bool finished_ = false; // a candidate carried a finishReason
    bool thinking_ = false;
    Json role_ = "model";
    Json parts_ = Json::array();
    Json fields_ = Json::object(); // the candidate's other fields; the last value of each counts
    Json usage_;
    Json prompt_feedback_;
};

} // namespace

std::unique_ptr<StreamAccumulator> make_google_accumulator() {
    return std::make_unique<GoogleAccumulator>();
}

} // namespace fv::llm::detail
