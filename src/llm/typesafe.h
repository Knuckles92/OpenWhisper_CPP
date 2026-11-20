#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/json.h"
#include "llm/judge.h"

// services/typesafe.py's Jev judge, answer validation and key verification (371-614,
// 701-773). TypeSafe and OpenRouter share request and response shapes: POST {"model", "state",
// "questions"} with a bearer key, answered {"answers": {name: answer}, "usage":
// {"input_tokens"}}. Nothing here logs the state, the questions or the key, and an error body
// never reaches the caller (it may echo the credential).
namespace fv::llm {

// What a transport answered: the status and, for a 2xx answer, the body.
struct TransportReply {
    int status = 0;
    std::string text;
};
// Python's Transport: takes the JSON payload and the budget (seconds) and returns (status,
// body), or the class name of the exception the request failed with ("TimeoutError").
using JudgeTransport =
    std::function<std::expected<TransportReply, std::string>(const Json& payload, double)>;

// Checks that `body` answers exactly the asked `questions` with the right types: a noul in
// [0, 1] (never a bool), a choice among the criteria with a confidence in [0, 1], a score in
// [0, len(criteria) - 1]. The validated answers in question order; throws TypeSafeError.
Json validate_answers(const Json& body, const Json& questions);

struct TypeSafeJudgeOptions {
    JudgeRoute route = typesafe_route();
    std::optional<std::string> model;    // the route's when unset
    std::optional<double> timeout_s;     // the route's when unset
    std::optional<std::string> endpoint; // the route's when unset; tests point it at loopback
    JudgeTransport transport;            // the keep-alive POST when unset
    const Clock* clock = nullptr;        // usage latency and warning spacing
};

// Asks Jev; every failure returns nullopt, counts in usage() and is logged sparsely ("TypeSafe"
// or "TypeSafe via <route>" ...). Affinity: thread-safe: each call is one round trip on a
// connection no other call is using.
class TypeSafeJudge final : public RouteJudge {
public:
    // Throws std::invalid_argument("<route> API key is required") for an empty key.
    TypeSafeJudge(std::string api_key, JudgeEnvironment& environment,
                  TypeSafeJudgeOptions options = {});

    // Nullopt at once for no questions, a state that cannot be written as JSON, or a request
    // over kMaxStateChars; else one POST within the budget.
    std::optional<Json> ask(const Json& state, const Json& questions,
                            std::optional<double> timeout_s, const CancelToken& cancel) override;
    const std::string& endpoint() const { return endpoint_; }
    // Same endpoint, model and key.
    bool same_connection(const Judge& other) const override;

private:
    std::string api_key_;
    std::string endpoint_;
    JudgeTransport transport_;
    std::string label_;
};

// A manual test is a one-off, so it can wait longer than a live judgment (seconds).
inline constexpr double kVerifyTimeoutS = 10.0;
// _VERIFY_STATE and _VERIFY_QUESTIONS: the smallest real judgment there is. It proves the key
// and the pinned model in one round trip and carries nothing about the user.
const Json& verify_state();
const Json& verify_questions();
// VERIFY_HOST: TypeSafe's host, shown in verification copy.
std::string verify_host();

struct VerifyResult {
    bool ok = false;
    std::string message;
    bool operator==(const VerifyResult&) const = default;
};

struct VerifyOptions {
    double timeout_s = kVerifyTimeoutS;
    std::optional<std::string> endpoint; // the route's when unset
    JudgeTransport transport;            // the keep-alive POST when unset
};

// Makes one authenticated judgment with `key` over `route` and says how it went: unlike ask, it
// reports failures, so somebody can find out why nothing is happening. Only the status class
// reaches the message. The text-model route goes to verify_text_model (llm/text_judge.h) with
// at least the route's budget. Blocks; never on the GUI thread.
VerifyResult verify_key(const JudgeRoute& route, std::string_view key,
                        JudgeEnvironment& environment, const VerifyOptions& options = {});

// _status_outcome: what an HTTP status decides, nullopt for 200. 429 proves the key; 404 means
// "no route" only on the OpenRouter route.
std::optional<VerifyResult> status_outcome(int status, std::string_view host,
                                           const JudgeRoute& route);

} // namespace fv::llm
