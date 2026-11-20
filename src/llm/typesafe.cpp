#include "llm/typesafe.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <memory>
#include <stdexcept>
#include <utility>

#include "llm/detail/keep_alive.h"
#include "llm/detail/truthy.h"
#include "llm/detail/url.h"
#include "llm/text_judge.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::llm {

namespace {

constexpr log::Logger kLog("services.typesafe");

using detail::get;

// isinstance(value, (int, float)): bool is an int in Python.
bool is_number(const Json& value) {
    return value.is_number() || value.is_boolean();
}

double as_float(const Json& value) {
    if (value.is_boolean()) {
        return value.get<bool>() ? 1.0 : 0.0;
    }
    return value.get<double>();
}

// _is_unit_interval. NaN compares false, so it is refused, as in Python.
bool is_unit_interval(const Json& value) {
    if (!is_number(value)) {
        return false;
    }
    const double number = as_float(value);
    return number >= 0.0 && number <= 1.0;
}

// str(value) of what json.loads makes.
std::string py_str(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "True" : "False";
    }
    if (value.is_null()) {
        return "None";
    }
    return py::dumps(value);
}

std::string_view py_type_name(const Json& value) {
    switch (value.type()) {
    case Json::value_t::null:
        return "NoneType";
    case Json::value_t::boolean:
        return "bool";
    case Json::value_t::number_float:
        return "float";
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned:
        return "int";
    case Json::value_t::string:
        return "str";
    case Json::value_t::array:
        return "list";
    case Json::value_t::object:
        return "dict";
    default:
        return "object";
    }
}

// `choice in criteria`: a dict's keys, a list's items, a str's substrings.
bool contains(const Json& criteria, const Json& choice) {
    if (criteria.is_object()) {
        return choice.is_string() && criteria.contains(choice.get<std::string>());
    }
    if (criteria.is_array()) {
        return std::ranges::any_of(criteria,
                                   [&](const Json& item) { return py::py_equal(item, choice); });
    }
    if (criteria.is_string()) {
        if (!choice.is_string()) {
            throw py::TypeError("'in <string>' requires string as left operand, not " +
                                std::string(py_type_name(choice)));
        }
        return criteria.get<std::string>().find(choice.get<std::string>()) != std::string::npos;
    }
    throw py::TypeError("argument of type '" + std::string(py_type_name(criteria)) +
                        "' is not iterable");
}

// len(value) of a container or str.
std::size_t py_len(const Json& value) {
    if (value.is_object() || value.is_array()) {
        return value.size();
    }
    if (value.is_string()) {
        return py::len(value.get<std::string>());
    }
    throw py::TypeError("object of type '" + std::string(py_type_name(value)) + "' has no len()");
}

Json checked_noul(const std::string& name, const Json& answer) {
    const Json& value = get(answer, "noul");
    if (value.is_boolean() || !is_unit_interval(value)) {
        throw TypeSafeError("invalid probability on " + name);
    }
    return Json{{"type", "noul"}, {"noul", as_float(value)}};
}

Json checked_choice(const std::string& name, const Json& question, const Json& answer) {
    const Json& choice = get(answer, "choice");
    const Json criteria =
        detail::has(question, "criteria") ? question.at("criteria") : Json::object();
    if (!contains(criteria, choice)) {
        throw TypeSafeError("unknown choice on " + name);
    }
    const Json confidence = detail::has(answer, "confidence") ? answer.at("confidence") : Json(0.0);
    if (!is_unit_interval(confidence)) {
        throw TypeSafeError("invalid confidence on " + name);
    }
    Json probabilities = get(answer, "probabilities");
    if (!detail::truthy(probabilities)) {
        probabilities = Json::object();
    }
    if (!probabilities.is_object()) {
        throw TypeSafeError("invalid probabilities on " + name);
    }
    Json kept = Json::object();
    for (const auto& [key, value] : probabilities.items()) {
        if (is_number(value)) {
            kept[key] = as_float(value);
        }
    }
    return Json{{"type", "choice"},
                {"choice", py_str(choice)},
                {"confidence", as_float(confidence)},
                {"probabilities", std::move(kept)}};
}

Json checked_score(const std::string& name, const Json& question, const Json& answer) {
    const Json& score = get(answer, "score");
    const Json& criteria = get(question, "criteria");
    const std::size_t count = detail::truthy(criteria) ? py_len(criteria) : 0;
    const double top = count > 0 ? static_cast<double>(count - 1) : 0.0;
    if (!is_number(score) || !(as_float(score) >= 0.0 && as_float(score) <= top)) {
        throw TypeSafeError("invalid score on " + name);
    }
    const Json confidence = detail::has(answer, "confidence") ? answer.at("confidence") : Json(0.0);
    return Json{{"type", "score"},
                {"score", as_float(score)},
                {"confidence", is_number(confidence) ? as_float(confidence) : 0.0}};
}

std::string seconds_text(double seconds) {
    return py::format_fixed(seconds, 2);
}

} // namespace

Json validate_answers(const Json& body, const Json& questions) {
    if (!body.is_object() || !get(body, "answers").is_object()) {
        throw TypeSafeError("response has no answers");
    }
    const Json& answers = body.at("answers");
    bool same = answers.size() == questions.size();
    for (const auto& [name, answer] : answers.items()) {
        same = same && questions.contains(name);
    }
    if (!same) {
        throw TypeSafeError("response question ids do not match the request");
    }
    Json checked = Json::object();
    for (const auto& [name, question] : questions.items()) {
        const Json& answer = answers.at(name);
        const Json& kind = get(question, "type");
        if (!answer.is_object() || get(answer, "type") != kind) {
            throw TypeSafeError("answer type mismatch on " + name);
        }
        if (kind == "noul") {
            checked[name] = checked_noul(name, answer);
        } else if (kind == "choice") {
            checked[name] = checked_choice(name, question, answer);
        } else if (kind == "score") {
            checked[name] = checked_score(name, question, answer);
        } else {
            throw TypeSafeError("unsupported question type on " + name);
        }
    }
    return checked;
}

// ---------------------------------------------------------------------------------------------
// TypeSafeJudge

TypeSafeJudge::TypeSafeJudge(std::string api_key, JudgeEnvironment& environment,
                             TypeSafeJudgeOptions options)
    : RouteJudge(options.route, JudgeOptions{options.model, options.timeout_s, options.clock},
                 environment, kLog),
      api_key_(std::move(api_key)), transport_(std::move(options.transport)) {
    if (api_key_.empty()) {
        throw std::invalid_argument(route().key_label() + " is required");
    }
    endpoint_ =
        options.endpoint && !options.endpoint->empty() ? *options.endpoint : route().endpoint;
    label_ = route() == typesafe_route() ? std::string("TypeSafe") : "TypeSafe via " + route().name;
}

std::optional<Json> TypeSafeJudge::ask(const Json& state, const Json& questions,
                                       std::optional<double> timeout_s, const CancelToken& cancel) {
    if (!detail::truthy(questions) || !questions.is_object()) {
        return std::nullopt;
    }
    const double timeout = budget(timeout_s);
    using Steady = std::chrono::steady_clock;
    using Seconds = std::chrono::duration<double>;
    const auto request_started = Steady::now();
    const double max_wait = Seconds(Steady::time_point::max() - request_started).count() / 2;
    const auto deadline =
        request_started + std::chrono::duration_cast<Steady::duration>(Seconds(
                              std::isnan(timeout) ? 0.0 : std::clamp(timeout, 0.0, max_wait)));
    Json payload = Json::object();
    payload["model"] = model();
    payload["state"] = state;
    payload["questions"] = questions;
    std::string encoded;
    try {
        encoded = py::dumps(payload);
    } catch (const std::exception&) {
        warn(label_ + " state is not JSON-serializable; skipping judgment");
        return std::nullopt;
    }
    // ensure_ascii: every character is one byte.
    if (encoded.size() > kMaxStateChars) {
        warn(std::format("{} request of {} chars exceeds the {} bound; skipping", label_,
                         encoded.size(), kMaxStateChars));
        return std::nullopt;
    }

    struct Outcome {
        std::optional<TransportReply> reply;
        std::string failure = "Exception";
    };
    auto outcome = std::make_shared<Outcome>();
    const double started = now();
    const bool finished = environment().run_cancellable(
        [outcome, payload, timeout, deadline, transport = transport_, key = api_key_,
         endpoint = endpoint_, headers = route().headers,
         environment = &environment()](const CancelToken& token) {
            const double remaining = Seconds(deadline - Steady::now()).count();
            if (remaining <= 0 || token.cancelled()) {
                return;
            }
            if (transport) {
                auto reply = transport(payload, timeout);
                if (reply) {
                    outcome->reply = std::move(*reply);
                } else {
                    outcome->failure = std::move(reply.error());
                }
                return;
            }
            auto reply = detail::http_post(environment->pool(), *environment->http(),
                                           environment->proxy_lookup(), payload, remaining, key,
                                           endpoint, headers, token);
            if (reply) {
                outcome->reply = TransportReply{reply->status, std::move(reply->text)};
            } else {
                outcome->failure = detail::python_exception_name(reply.error());
            }
        },
        cancel, deadline);
    if (!finished) {
        record(started, false);
        if (cancel.cancelled()) {
            debug(label_ + " request canceled");
        } else {
            warn(label_ + " request failed: TimeoutError");
        }
        return std::nullopt;
    }
    if (!outcome->reply) {
        // Network, timeout, TLS.
        record(started, false);
        warn(label_ + " request failed: " + outcome->failure);
        return std::nullopt;
    }
    if (outcome->reply->status != 200) {
        record(started, false);
        warn(std::format("{} returned HTTP {}", label_, outcome->reply->status));
        return std::nullopt;
    }
    Json body;
    Json answers;
    try {
        body = py::loads(outcome->reply->text);
        answers = validate_answers(body, questions);
    } catch (const std::exception& error) {
        record(started, false);
        warn(label_ + " response rejected: " + error.what());
        return std::nullopt;
    }
    const Json& usage = get(body, "usage");
    const Json& tokens = usage.is_object() ? get(usage, "input_tokens") : usage;
    std::int64_t counted = 0;
    if (usage.is_object() && (tokens.is_number_integer() || tokens.is_boolean())) {
        counted = tokens.is_boolean() ? (tokens.get<bool>() ? 1 : 0) : tokens.get<std::int64_t>();
    }
    record(started, true, counted);
    return answers;
}

bool TypeSafeJudge::same_connection(const Judge& other) const {
    const auto* judge = dynamic_cast<const TypeSafeJudge*>(&other);
    return judge != nullptr && endpoint_ == judge->endpoint_ && model() == judge->model() &&
           api_key_ == judge->api_key_;
}

// ---------------------------------------------------------------------------------------------
// Verification

const Json& verify_state() {
    static const Json state = Json{{"text", "ok"}};
    return state;
}

const Json& verify_questions() {
    static const Json questions =
        Json{{"q", Json{{"type", "noul"}, {"instructions", "Is `text` non-empty?"}}}};
    return questions;
}

std::string verify_host() {
    return typesafe_route().host();
}

VerifyResult verify_key(const JudgeRoute& route, std::string_view key,
                        JudgeEnvironment& environment, const VerifyOptions& options) {
    const std::string stripped = py::strip(key);
    if (stripped.empty()) {
        return {false, "Paste a key first."};
    }
    if (route.profile) {
        // A local server may first have to load the model into memory.
        return verify_text_model(stripped, route, std::max(options.timeout_s, route.timeout_s),
                                 environment);
    }
    const std::string endpoint =
        options.endpoint && !options.endpoint->empty() ? *options.endpoint : route.endpoint;
    std::string host;
    try {
        host = detail::urlsplit(endpoint).hostname().value_or("");
    } catch (const std::exception&) {
        host.clear();
    }
    if (host.empty()) {
        host = route.host();
    }
    Json payload = Json::object();
    payload["model"] = route.model;
    payload["state"] = verify_state();
    payload["questions"] = verify_questions();

    const auto started = std::chrono::steady_clock::now();
    std::expected<TransportReply, std::string> reply = std::unexpected(std::string("Exception"));
    if (options.transport) {
        reply = options.transport(payload, options.timeout_s);
    } else {
        auto posted =
            detail::http_post(environment.pool(), *environment.http(), environment.proxy_lookup(),
                              payload, options.timeout_s, stripped, endpoint, route.headers);
        if (posted) {
            reply = TransportReply{posted->status, std::move(posted->text)};
        } else {
            reply = std::unexpected(detail::python_exception_name(posted.error()));
        }
    }
    if (!reply) {
        kLog.debug("TypeSafe key verification failed: {}", reply.error());
        return {false, "Couldn't reach " + host + " (" + reply.error() + ")."};
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (auto outcome = status_outcome(reply->status, host, route)) {
        return *outcome;
    }
    try {
        validate_answers(py::loads(reply->text), verify_questions());
    } catch (const std::exception& error) {
        return {false, host + " accepted the key but answered oddly: " + error.what() + "."};
    }
    return {true, host + " accepted the key and " + route.model + " answered in " +
                      seconds_text(elapsed) + " s."};
}

std::optional<VerifyResult> status_outcome(int status, std::string_view host,
                                           const JudgeRoute& route) {
    const std::string at(host);
    if (status == 401) {
        return VerifyResult{false, at + " rejected the key (HTTP 401)."};
    }
    if (status == 402) {
        // OpenRouter's answer for an account with no credits left.
        return VerifyResult{
            false, at + " accepted the key, but the account is out of credits (HTTP 402)."};
    }
    if (status == 403) {
        return VerifyResult{false, at + " accepted the key but denied access (HTTP 403)."};
    }
    if (status == 404 && route == openrouter_route()) {
        // For chat models OpenRouter answers 404 when the account's privacy settings exclude
        // every provider of the model; assumed the same here.
        return VerifyResult{false, at + " has no route to " + route.model +
                                       " for this key (HTTP 404). Check the provider and privacy "
                                       "settings on your OpenRouter account."};
    }
    if (status == 429) {
        // Throttling only happens after authentication, so the key is good.
        return VerifyResult{true, at + " accepted the key but is rate limiting it (HTTP 429)."};
    }
    if (status != 200) {
        return VerifyResult{false, at + " answered HTTP " + std::to_string(status) + "."};
    }
    return std::nullopt;
}

} // namespace fv::llm
