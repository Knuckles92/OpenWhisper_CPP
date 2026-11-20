#pragma once

// Shared pieces of the generation and cleanup tests: the canned replies and scenarios of
// tests/fixtures/llm/generation/, requests recorded the way the vectors record them, and results
// and failures as the vectors spell them.

#include <cmath>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

#include "golden/golden.h"
#include "http/client.h"
#include "http/testing/fake_http_client.h"
#include "http/user_agent.h"
#include "llm/generation.h"
#include "llm/model_catalog.h"
#include "llm/openai_client.h"
#include "llm_test_support.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "testkit/testkit.h"

namespace fv::llm::test {

inline constexpr std::string_view kSecret = "sk-test-SECRET-0123456789";

inline const Json& generation_fixture(std::string_view name) {
    static const Json replies = [] {
        const auto path =
            testkit::source_root() / "tests" / "fixtures" / "llm" / "generation" / "replies.json";
        std::ifstream file(path, std::ios::binary);
        return py::loads(
            std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()));
    }();
    static const Json scenarios = [] {
        const auto path =
            testkit::source_root() / "tests" / "fixtures" / "llm" / "generation" / "scenarios.json";
        std::ifstream file(path, std::ios::binary);
        return py::loads(
            std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()));
    }();
    return name == "replies" ? replies : scenarios;
}

// A named reply of replies.json.
inline const Json& reply_body(std::string_view name) {
    return generation_fixture("replies").at(std::string(name));
}

// The tool schema the scenarios use.
inline Json tool_schema() {
    return generation_fixture("scenarios").at("tool");
}

// A scenario's reply entry as the fake provider answers it.
inline http::ScriptedResponse scripted_reply(const Json& entry) {
    if (entry.is_string()) {
        return json_reply(200, reply_body(entry.get<std::string>()));
    }
    http::ScriptedResponse reply;
    if (entry.contains("error")) {
        if (entry.at("error") == "timeout") {
            reply.error = http::Error{http::ErrorKind::Timeout, "timed out"};
        } else {
            reply.error = http::Error{http::ErrorKind::Connect, "[WinError 10061] refused"};
        }
        return reply;
    }
    if (entry.contains("json")) {
        reply = json_reply(entry.at("status").get<int>(), entry.at("json"));
    } else {
        reply = text_reply(entry.at("status").get<int>(), entry.value("text", std::string()));
    }
    if (entry.contains("headers")) {
        for (const auto& [name, value] : entry.at("headers").items()) {
            reply.headers.emplace_back(name, value.get<std::string>());
        }
    }
    return reply;
}

inline std::string lower(std::string text) {
    for (auto& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// The JSON body a request carried, or null.
inline Json request_body(const http::Request& request) {
    if (const auto* text = std::get_if<std::string>(&request.body)) {
        return text->empty() ? Json() : py::loads(*text);
    }
    return Json();
}

// A request as the vectors record it. FioriVoce's own User-Agent stands where the SDK sends its
// own, so it is left out as the SDK's is; a uuid4 OpenCode session becomes "<uuid4>" with
// `uuid_session`.
inline Json recorded_request(const http::Request& request, bool uuid_session = false,
                             bool with_query = true) {
    Json headers = Json::object();
    bool bearer = false;
    for (const auto& [name, value] : request.headers) {
        const std::string key = lower(name);
        if (key == "authorization") {
            bearer = value == "Bearer " + std::string(kSecret);
            continue;
        }
        if (key == "user-agent" && value == http::user_agent()) {
            continue;
        }
        if (key == "x-opencode-session" && uuid_session && value.size() == 36 && value[14] == '4') {
            headers[key] = "<uuid4>";
            continue;
        }
        headers[key] = value;
    }
    Json out = Json::object();
    out["method"] = request.method;
    out["path"] = url_path(request.url);
    if (with_query) {
        const auto mark = request.url.find('?');
        out["query"] = mark == std::string::npos ? "" : request.url.substr(mark + 1);
    }
    out["bearer"] = bearer;
    out["timeout"] = static_cast<double>(request.idle_timeout.count()) / 1000.0;
    out["headers"] = std::move(headers);
    out["body"] = request_body(request);
    return out;
}

// A vector's recorded request compared with ours: headers rebranded, the timeout to the
// millisecond.
inline bool same_request(const Json& expected, const Json& actual) {
    Json want = expected;
    for (auto& [name, value] : want["headers"].items()) {
        value = golden::rebrand(value.get<std::string>());
    }
    const double timeout = want.at("timeout").get<double>();
    want["timeout"] = std::ceil(timeout * 1000.0) / 1000.0;
    return golden::equal(want, actual);
}

inline Json result_json(const TextResult& result) {
    Json calls = Json::array();
    for (const auto& call : result.tool_calls) {
        calls.push_back(Json{
            {"id", call.id}, {"name", call.function.name}, {"arguments", call.function.arguments}});
    }
    Json out = Json::object();
    out["text"] = result.text;
    out["tool_calls"] = std::move(calls);
    out["assistant_message"] = result.assistant_message;
    out["usage"] = result.usage;
    out["finish_reason"] = result.finish_reason;
    return out;
}

// {"raises": <Python class>, "message": str(exc)} of a failure, as the vectors record it.
inline Json failure_json(const std::exception_ptr& failure) {
    try {
        std::rethrow_exception(failure);
    } catch (const ApiError& error) {
        return Json{{"raises", error.kind}, {"message", error.what()}};
    } catch (const py::PyError& error) {
        return Json{{"raises", std::string(error.type_name())}, {"message", error.what()}};
    } catch (const std::invalid_argument& error) {
        return Json{{"raises", "ValueError"}, {"message", error.what()}};
    } catch (const std::exception& error) {
        return Json{{"raises", "Exception"}, {"message", error.what()}};
    }
}

// TextModelSpec from the fields the scenarios give it.
inline TextModelSpec spec_from_fields(const Json& fields) {
    TextModelSpec spec;
    spec.protocol = fields.value("protocol", std::string("chat"));
    spec.tools = fields.value("tools", true);
    spec.context_window = fields.value("context_window", std::int64_t{32768});
    spec.max_output_tokens = fields.value("max_output_tokens", std::int64_t{4096});
    spec.reasoning = fields.value("reasoning", false);
    spec.reasoning_format = fields.value("reasoning_format", std::string());
    if (fields.contains("thinking_levels") && !fields.at("thinking_levels").is_null()) {
        spec.thinking_levels = fields.at("thinking_levels");
    }
    if (fields.contains("pi_compat") && !fields.at("pi_compat").is_null()) {
        spec.pi_compat = fields.at("pi_compat");
    }
    return spec;
}

// A client over `http` that never sleeps between attempts.
inline OpenAiCompatClient quick_client(std::shared_ptr<http::Client> http,
                                       const TextLLMProfile& profile, int max_retries = 0,
                                       std::string_view key = kSecret) {
    auto client = create_openai_client(std::move(http), profile, 15.0, key, {}, max_retries);
    client.set_hooks({[](std::chrono::milliseconds, const CancelToken&) {}, nullptr, nullptr});
    return client;
}

} // namespace fv::llm::test
