#pragma once

// Shared fakes for the fv_llm tests: in-memory key sources, profiles as the vectors spell them,
// fixture-backed local servers on http::FakeHttpClient, and a log sink that keeps what it sees.

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "base/identity.h"
#include "base/log.h"
#include "base/testing/map_env_reader.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "http/testing/fake_http_client.h"
#include "llm/credentials.h"
#include "llm/profiles.h"
#include "pyport/json.h"
#include "testkit/testkit.h"

namespace fv::llm::test {

// Keys from an in-memory store, a map environment and a .env that does not exist.
struct TestKeys {
    std::shared_ptr<MemoryCredentialBackend> backend = std::make_shared<MemoryCredentialBackend>();
    CredentialStore store{backend, identity()};
    MapEnvReader env;
    DotEnv dotenv{std::filesystem::path("fv-llm-tests-no-such-dir") / ".env"};
    KeySources sources{store, env, dotenv};
};

// A vector's profile: a built-in id (get_profile(id, {})) or TextLLMProfile's fields.
inline TextLLMProfile profile_from(const Json& spec) {
    if (spec.is_string()) {
        return *get_profile(spec.get<std::string>(), Json::object());
    }
    TextLLMProfile profile;
    profile.id = spec.at("id").get<std::string>();
    profile.name = spec.at("name").get<std::string>();
    profile.kind = spec.at("kind").get<std::string>();
    if (spec.contains("base_url") && !spec.at("base_url").is_null()) {
        profile.base_url = spec.at("base_url").get<std::string>();
    }
    profile.api_key_env = spec.value("api_key_env", std::string());
    profile.builtin = spec.value("builtin", false);
    if (spec.contains("model_metadata") && !spec.at("model_metadata").is_null()) {
        profile.model_metadata = spec.at("model_metadata");
    }
    profile.protocol = spec.value("protocol", std::string("chat"));
    return profile;
}

// The case files' _fields(profile).
inline Json profile_fields(const std::optional<TextLLMProfile>& profile) {
    if (!profile) {
        return nullptr;
    }
    Json out = Json::object();
    out["id"] = profile->id;
    out["name"] = profile->name;
    out["kind"] = profile->kind;
    out["base_url"] = profile->base_url ? Json(*profile->base_url) : Json(nullptr);
    out["api_key_env"] = profile->api_key_env;
    out["builtin"] = profile->builtin;
    out["model_metadata"] = profile->model_metadata ? *profile->model_metadata : Json(nullptr);
    out["protocol"] = profile->protocol;
    out["requires_api_key"] = profile->requires_api_key();
    out["is_local"] = profile->is_local();
    out["self_hosted"] = profile->self_hosted();
    out["refuses_unsupported_tools"] = profile->refuses_unsupported_tools();
    return out;
}

inline std::string standard_reason(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 429:
        return "Too Many Requests";
    case 500:
        return "Internal Server Error";
    case 502:
        return "Bad Gateway";
    default:
        return "";
    }
}

inline http::ScriptedResponse json_reply(int status, const Json& body) {
    http::ScriptedResponse reply;
    reply.status = status;
    reply.reason = standard_reason(status);
    reply.headers = {{"Content-Type", "application/json"}};
    reply.body = py::dumps(body);
    return reply;
}

inline http::ScriptedResponse text_reply(int status, std::string body) {
    http::ScriptedResponse reply;
    reply.status = status;
    reply.reason = standard_reason(status);
    reply.headers = {{"Content-Type", "text/plain; charset=utf-8"}};
    reply.body = std::move(body);
    return reply;
}

inline Json load_fixture(std::string_view name) {
    const auto path = testkit::source_root() / "tests" / "fixtures" / "llm" / "servers" /
                      (std::string(name) + ".json");
    std::ifstream file(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return py::loads(text);
}

// The path of a request URL ("http://h:1/v1/models?x" -> "/v1/models").
inline std::string url_path(const std::string& url) {
    const auto scheme = url.find("://");
    const auto start = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    if (start == std::string::npos) {
        return "/";
    }
    const auto end = url.find_first_of("?#", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// A local server answering from a fixture's routes: "METHOD /path" -> {"status", "json" |
// "text" | "json_by_model"}; any other route is a 404 "Not Found".
inline http::FakeHttpClient::Handler fixture_server(const Json& fixture) {
    const Json routes = fixture.at("routes");
    return [routes](const http::Request& request) {
        const std::string key = request.method + " " + url_path(request.url);
        if (!routes.contains(key)) {
            return text_reply(404, "Not Found");
        }
        const Json& route = routes.at(key);
        const int status = route.at("status").get<int>();
        if (route.contains("json_by_model")) {
            const auto& body = std::get<std::string>(request.body);
            const auto model = py::loads(body).at("model").get<std::string>();
            return json_reply(status, route.at("json_by_model").at(model));
        }
        if (route.contains("json")) {
            return json_reply(status, route.at("json"));
        }
        return text_reply(status, route.at("text").get<std::string>());
    };
}

// Keeps every record logged while it is installed.
class CapturedLog : public log::Sink {
public:
    CapturedLog() {
        previous_ = log::level();
        log::set_level(log::Level::Debug);
    }
    ~CapturedLog() override { log::set_level(previous_); }
    CapturedLog(const CapturedLog&) = delete;
    CapturedLog& operator=(const CapturedLog&) = delete;

    void write(const log::Record& record) override {
        std::lock_guard lock(mutex_);
        messages_.emplace_back(record.message);
    }
    std::string all() const {
        std::lock_guard lock(mutex_);
        std::string out;
        for (const auto& message : messages_) {
            out += message + "\n";
        }
        return out;
    }

private:
    log::Level previous_;
    mutable std::mutex mutex_;
    std::vector<std::string> messages_;
};

// Installs a CapturedLog for one scope.
struct LogScope {
    std::shared_ptr<CapturedLog> sink = std::make_shared<CapturedLog>();
    LogScope() { log::add_sink(sink); }
    ~LogScope() { log::remove_sink(sink); }
    LogScope(const LogScope&) = delete;
    LogScope& operator=(const LogScope&) = delete;
};

} // namespace fv::llm::test
