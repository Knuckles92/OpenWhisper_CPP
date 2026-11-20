#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include "base/cancel.h"
#include "base/json.h"
#include "llm/judge.h"
#include "llm/typesafe.h"
#include "pyport/errors.h"

// services/text_judge.py: Fast judgments answered by one of the user's own text models (a
// local server or a hosted endpoint), with Jev's ask contract, answer shapes and failure
// policy. A noul is the model's stated probability of yes; a choice is an option picked by
// number with the probability that it is right, turned into Jev's confidence
// (p - 1/n) / (1 - 1/n), and no distribution; a score is refused. The probabilities are the
// model's own statement at temperature 0 under a JSON schema, not calibrated as Jev's are.
namespace fv::llm {

// Room for the JSON punctuation, then for each answer (tokens).
inline constexpr std::int64_t kJudgeBaseTokens = 64;
inline constexpr std::int64_t kJudgeTokensPerQuestion = 40;
// Seconds: the most of a judgment's budget spent asking a server the user runs what its models
// are, once per session.
inline constexpr double kJudgeLearnTimeoutS = 3.0;

// SYSTEM_PROMPT, verbatim.
extern const std::string_view kJudgeSystemPrompt;

// The model's context window cannot hold a judgment and its answer (a ValueError). what() is
// "the model's context of 4,096 tokens is too small for a judgment of about 9,123 tokens".
class ContextTooSmall : public py::ValueError {
public:
    ContextTooSmall(std::int64_t window, std::int64_t needed);
    std::int64_t window;
    std::int64_t needed;
};

// _fit_context: refuses a judgment of `content_chars` code points and `max_tokens` of answer
// that would overflow the model's window, (len(SYSTEM_PROMPT) + content) // 3 + 32 + max_tokens
// tokens. A server truncates an overlong prompt without saying so, and the model would then
// judge part of the state. Throws ContextTooSmall, or what model_spec throws.
void fit_context(const TextLLMProfile& profile, std::string_view model, std::size_t content_chars,
                 std::int64_t max_tokens);

// render: the questions as the model reads them, option number -> criterion per choice
// question, and the strict answer schema. Options are numbered "1".."n" in their order: an
// option can be a segment id or a whole span of dictation, and a number is short to copy back.
// Throws TypeSafeError for a question that is neither noul nor choice.
struct RenderedQuestions {
    Json shown = Json::object();
    Json labels = Json::object();
    Json schema = Json::object();
};
RenderedQuestions render(const Json& questions);

// _json_object: the outermost {...} of a reply after removing <think>...</think> (reasoning
// models served without a reasoning field, servers without a schema that wrap the object).
// Throws TypeSafeError("answer is not a JSON object" | "answer is not valid JSON").
Json json_object(std::string_view text);

// Jev's Choice confidence for a top probability over `options` options.
double confidence(double probability, std::int64_t options);

// The model's reply in Jev's answer shapes, validated as Jev's are. Throws TypeSafeError: not
// JSON, a question missing, an unknown option, an impossible probability.
Json parse(std::string_view text, const Json& questions, const Json& labels);

// _rejects_schema: an HTTP 400 or 422 whose message names response_format or json_schema.
bool rejects_schema(const std::exception& error);

// Answers Jev's questions with a text model; every failure returns nullopt. Affinity:
// thread-safe: each call is one blocking request on the shared client.
class TextModelJudge final : public RouteJudge {
public:
    // Throws std::invalid_argument("the text-model route needs a text model profile").
    TextModelJudge(std::string api_key, JudgeRoute route, JudgeEnvironment& environment,
                   const JudgeOptions& options = {});

    std::optional<Json> ask(const Json& state, const Json& questions,
                            std::optional<double> timeout_s, const CancelToken& cancel) override;

    struct Answer {
        Json answers;
        std::int64_t input_tokens = 0;
    };
    // One judgment that throws instead of returning nullopt: TypeSafeError, ContextTooSmall and
    // ValueError for what is refused before sending, py::PyError("TimeoutError") when nothing is
    // left of the budget after reading the server's model list, and generate's errors.
    Answer request(const Json& state, const Json& questions, double timeout_s,
                   const CancelToken& cancel = {}) const;

    const TextLLMProfile& profile() const { return *route().profile; }
    // Same profile, model and key.
    bool same_connection(const Judge& other) const override;

private:
    std::string api_key_;
    std::string label_;
};

// verify_text_model: asks the route's model the one-word verification judgment and says why it
// failed, from status classes and exception names only (a provider's error body may echo the
// key). A self-hosted server's model list is read afresh first, and the check also proves that a
// judgment of kMaxStateChars would fit the model's context.
VerifyResult verify_text_model(std::string_view api_key, const JudgeRoute& route, double timeout_s,
                               JudgeEnvironment& environment);

// _failure_reason: why the judgment failed, from its HTTP status or exception class only.
std::string failure_reason(const std::exception& error, const JudgeRoute& route,
                           std::string_view where, double timeout_s);

// _MORE_CONTEXT: how to give a model `tokens` of context on a server preset, or the general
// advice for any other kind.
std::string more_context_advice(std::string_view kind, std::int64_t tokens);

} // namespace fv::llm
