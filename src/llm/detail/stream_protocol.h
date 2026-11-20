#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/json.h"
#include "llm/sse.h"
#include "llm/streaming.h"

// One protocol's stream turned back into the reply the provider sends without streaming, which
// generate()'s own reader then reads: that is how a streamed turn returns exactly generate()'s
// TextResult and errors.
namespace fv::llm::detail {

using EmitDelta = std::function<void(const StreamEvent&)>;

class StreamAccumulator {
public:
    virtual ~StreamAccumulator() = default;
    // Throws StreamError for a failure the stream reports or data that is not JSON.
    virtual void on_event(const SseEvent& event, const EmitDelta& emit) = 0;
    // The provider marked the reply finished: Chat's finish_reason or [DONE], a terminal
    // Responses event, Anthropic's message_stop, Google's finishReason (or a prompt it blocked).
    virtual bool complete() const = 0;
    // The non-streamed reply; ends the blocks still open, emitting their end events.
    virtual Json reply(const EmitDelta& emit) = 0;
};

std::unique_ptr<StreamAccumulator> make_chat_accumulator();
std::unique_ptr<StreamAccumulator> make_responses_accumulator();
std::unique_ptr<StreamAccumulator> make_anthropic_accumulator();
std::unique_ptr<StreamAccumulator> make_google_accumulator();

StreamEvent stream_event(StreamDelta kind, std::string text = {}, std::string tool_call_id = {},
                         std::string tool_name = {});

// An event's data as JSON; StreamError(kStreamMalformedError) when it is not.
Json event_json(const SseEvent& event);

// The status an error type or code inside a stream stands for: Anthropic's error types and
// OpenAI's codes ("rate_limit_error" 429, "overloaded_error" 529, "api_error" 500, ...).
std::optional<int> error_type_status(std::string_view type);

// StreamError(kStreamFailedError), transient for 429 and >= 500; its provider_message is the
// cleaned message of `payload`, the stream's error event.
[[noreturn]] void throw_stream_failure(std::optional<int> status, const Json& payload = Json());

// Transfers a cancel left running that are not yet joined (each is joined once it ends).
std::size_t lingering_stream_transfers();

// The string at `key` of an object, or empty.
std::string string_at(const Json& object, std::string_view key);

} // namespace fv::llm::detail
