#pragma once

#include <optional>
#include <string>
#include <vector>

#include "base/cancel.h"
#include "base/json.h"
#include "llm/openai_client.h"
#include "llm/profiles.h"
#include "pyport/errors.h"

// services/text_generation.py: one generation turn over Chat Completions, Responses, Anthropic
// Messages or Google generateContent, whichever the model's spec names. Assistant messages carry
// the provider's opaque data ("_responses", "_anthropic", "_google", reasoning fields) into the
// next tool round, so signed thinking survives without ever becoming text. Blocking; never on
// the GUI thread.
namespace fv::llm {

struct ToolFunction {
    std::string name;
    std::string arguments; // JSON text
};

struct ToolCall {
    std::string id;
    ToolFunction function;
};

struct TextResult {
    std::string text;
    std::vector<ToolCall> tool_calls;
    // Replayed unchanged as the next turn's assistant message; {} when the reply had no choice.
    Json assistant_message = Json::object();
    // Chat: the provider's usage object; the native protocols: {prompt_tokens,
    // completion_tokens, total_tokens}; null when the reply had none.
    Json usage;
    std::string finish_reason;
};

// RuntimeError subclass in Python; what() is the user's sentence ("canceled", "Model output was
// incomplete (length).", ...).
class TextGenerationError : public py::PyError {
public:
    explicit TextGenerationError(const std::string& message)
        : py::PyError("TextGenerationError", message) {}
};

struct GenerateRequest {
    std::string model;
    Json messages = Json::array();
    std::optional<Json> tools; // Chat Completions tool schemas
    CancelToken cancel;        // checked before and after the request; also aborts it
    std::string session_id;    // OpenCode's session header
    std::string reasoning_level;
    // Python's **options: request-body fields (temperature, max_tokens, response_format,
    // reasoning_effort, ...), plus the SDK's own "timeout" (seconds), "extra_headers" and
    // "extra_body".
    Json options = Json::object();
};

// Throws TextGenerationError ("canceled", "This model does not support tool calls.", the
// adapters' messages), ApiError and RequestCanceled's sibling errors from the client, what
// model_spec throws, and Python's KeyError, TypeError or AttributeError (as py::PyError) for a
// reply missing what the adapters read.
TextResult generate(OpenAiCompatClient& client, const TextLLMProfile& profile,
                    GenerateRequest request);
// Without a profile the model gets TextModelSpec's defaults (Chat Completions).
TextResult generate(OpenAiCompatClient& client, const TextLLMProfile* profile,
                    GenerateRequest request);

} // namespace fv::llm
