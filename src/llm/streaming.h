#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "llm/generation.h"
#include "llm/openai_client.h"
#include "llm/profiles.h"

// One generation turn streamed over server-sent events: the same request generate() sends, with
// the stream switched on, and the same TextResult at the end (text, tool calls, usage and the
// assistant message the next turn replays), while the deltas are reported as they arrive in the
// agent SDKs' vocabulary (pi-ai's AssistantMessageEvent: thinking_start ... toolcall_end).
// OpenWhisper's Python never streamed; its meeting agent's SDKs did. Blocking; never on the GUI
// thread. No request or response body reaches a log or an error.
namespace fv::llm {

enum class StreamDelta {
    ThinkingStart,
    ThinkingDelta,
    ThinkingEnd,
    TextDelta,
    ToolCallStart,
    ToolCallDelta,
    ToolCallEnd,
};

// The SDK's name: "thinking_start", "thinking_delta", "thinking_end", "text_delta",
// "toolcall_start", "toolcall_delta", "toolcall_end".
std::string_view stream_delta_name(StreamDelta kind);

struct StreamEvent {
    StreamDelta kind;
    // ThinkingDelta and TextDelta: the new text; ToolCallDelta: the next fragment of the
    // arguments' JSON text; empty for the others.
    std::string text;
    // The tool-call events: the call's id and function name as the result will carry them.
    std::string tool_call_id, tool_name;
};

// Called on the thread that called stream_generate, in stream order. An exception it throws
// ends the transfer and leaves stream_generate.
using StreamSink = std::function<void(const StreamEvent&)>;

// What a StreamError says. Provider bodies are never quoted: a status failure reads as the SDK's
// text for an answer without a body.
inline constexpr std::string_view kStreamConnectionError = "Connection error.";
inline constexpr std::string_view kStreamTimeoutError = "Request timed out.";
inline constexpr std::string_view kStreamDroppedError =
    "Model stream ended before the reply was complete.";
inline constexpr std::string_view kStreamFailedError = "Model stream reported an error.";
inline constexpr std::string_view kStreamMalformedError = "Model stream was malformed.";
// "Error code: <status>".
std::string stream_status_message(int status);

// A failed transfer or a failure the provider reported inside the stream: what the agent loop's
// retry rule reads. A reply that arrived whole but says the output is incomplete, refused or
// blocked raises generate()'s TextGenerationError instead.
class StreamError : public TextGenerationError {
public:
    explicit StreamError(const std::string& message) : TextGenerationError(message) {}

    std::optional<int> status;           // HTTP status, or the status the stream's error names
    bool transient = false;              // 429, >= 500, connection error, dropped stream
    std::optional<double> retry_after_s; // retry-after-ms, retry-after (s or HTTP date); >= 0
    bool output_seen = false;            // a delta reached the sink before the failure
    // The provider's own message (provider_error_message of the error body or the stream's
    // error event), already cleaned; empty when it gave none. Never the body itself.
    std::string provider_message;
};

// The message a provider's error body states, safe to show: its `error.message` (or top-level
// `message`) string, first line only, with anything shaped like a key ("sk-...", "Bearer ...",
// a run of 20 or more base64 characters) and URL queries removed, at most 200 code points.
// Empty when the body is not JSON or names no message. M10.22's gain over amendment 20.
std::string provider_error_message(std::string_view body);
std::string provider_error_message_json(const Json& body);

struct StreamOptions {
    // A reply cut off at the output limit (Chat "length", Responses "incomplete" for
    // max_output_tokens, Anthropic "max_tokens", Google "MAX_TOKENS") returns what arrived, its
    // text and complete tool calls, with finish_reason "length" (Pi's stop reason, which its
    // agent loop counts as a settled turn), instead of generate()'s "Model output was
    // incomplete (...)." A content filter or refusal still raises.
    bool settle_length_stop = false;
};

// One attempt, no retries (the agent loop owns retries); `client`'s own retry count is not used.
// Returns what generate() returns for the same reply; throws StreamError, generate()'s errors
// for what it checks before sending and for the reply (incomplete, refused, blocked), and
// TextGenerationError("canceled") as soon as `request.cancel` is cancelled, even while the
// provider is silent.
//
// Where the request differs from generate()'s: Chat Completions asks for `stream: true` and
// `stream_options: {include_usage: true}`, Responses and Anthropic Messages for `stream: true`,
// Google uses `:streamGenerateContent?alt=sse`; Anthropic Messages and Google take the key as
// `x-api-key` and `x-goog-api-key` (as the SDKs the agent loop replaces send it) instead of a
// bearer Authorization. A 2xx answer that is JSON rather than an event stream (a server that
// ignores `stream`) is read as generate() reads it, without deltas.
TextResult stream_generate(OpenAiCompatClient& client, const TextLLMProfile& profile,
                           GenerateRequest request, const StreamSink& sink);
// Without a profile the model gets TextModelSpec's defaults (Chat Completions).
TextResult stream_generate(OpenAiCompatClient& client, const TextLLMProfile* profile,
                           GenerateRequest request, const StreamSink& sink);
TextResult stream_generate(OpenAiCompatClient& client, const TextLLMProfile& profile,
                           GenerateRequest request, const StreamSink& sink, StreamOptions options);
TextResult stream_generate(OpenAiCompatClient& client, const TextLLMProfile* profile,
                           GenerateRequest request, const StreamSink& sink, StreamOptions options);

} // namespace fv::llm
