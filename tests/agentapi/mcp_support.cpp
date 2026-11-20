#include "mcp_support.h"

#include <catch2/catch_test_macros.hpp>

#include <utility>

#include "agentapi/mcp_server.h"
#include "pyport/json.h"

namespace fv::agentapi::test {

automation::LiveState LiveControl::state() {
    std::lock_guard lock(mutex_);
    return live;
}

Json LiveControl::list_actions() {
    std::lock_guard lock(mutex_);
    return actions;
}

void LiveControl::record(std::vector<std::string> call) {
    std::lock_guard lock(mutex_);
    calls_.push_back(std::move(call));
    if (failure_) {
        if (failure_->permission) {
            throw automation::permissions::PermissionDenied(*failure_->permission);
        }
        throw *failure_->control;
    }
}

std::string LiveControl::run_action(std::string_view action_id, std::string_view text,
                                    std::string_view source) {
    record({"run_action", std::string(action_id), std::string(text), std::string(source)});
    return run_job;
}

std::string LiveControl::start_recording(std::optional<std::string> action_id,
                                         std::string_view source) {
    record({"start_recording", action_id.value_or("<none>"), std::string(source)});
    return start_job;
}

void LiveControl::stop_recording() {
    record({"stop_recording"});
}

void LiveControl::cancel() {
    record({"cancel"});
}

void LiveControl::notify(std::string_view text, std::string_view title) {
    record({"notify", std::string(text), std::string(title)});
}

void LiveControl::fail(std::optional<Failure> failure) {
    std::lock_guard lock(mutex_);
    failure_ = std::move(failure);
}

std::vector<std::vector<std::string>> LiveControl::calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
}

LiveScope::LiveScope(std::size_t buffer_size)
    : bus(std::make_shared<automation::EventBus>(buffer_size)) {
    previous_control_ = automation::app_control();
    automation::set_app_control(control);
    previous_bus_ = automation::set_bus(bus);
}

LiveScope::~LiveScope() {
    automation::set_app_control(previous_control_);
    automation::set_bus(previous_bus_);
    bus->close();
}

net::HttpRequest mcp_post(const std::string& token, const std::string& method, Json params,
                          bool modern, Json id) {
    net::HttpRequest request;
    request.method = "POST";
    request.path = "/mcp";
    request.tls = false;
    request.peer = {"127.0.0.1", 50000};
    request.headers = {{"host", "127.0.0.1"},
                       {"authorization", "Bearer " + token},
                       {"accept", "application/json, text/event-stream"},
                       {"content-type", "application/json"}};
    if (!params.is_object()) {
        params = Json::object();
    }
    if (modern) {
        Json meta = Json::object();
        meta[std::string(PROTOCOL_VERSION_META_KEY)] = std::string(MODERN_VERSION);
        meta[std::string(CLIENT_CAPABILITIES_META_KEY)] = Json::object();
        params["_meta"] = std::move(meta);
        request.headers.emplace_back("mcp-protocol-version", std::string(MODERN_VERSION));
        request.headers.emplace_back("mcp-method", method);
        if (method == "tools/call") {
            request.headers.emplace_back("mcp-name", params.at("name").get<std::string>());
        }
    }
    Json message = Json::object();
    message["jsonrpc"] = "2.0";
    message["id"] = std::move(id);
    message["method"] = method;
    message["params"] = std::move(params);
    request.body = py::dumps(message);
    return request;
}

Json call_tool(McpApp& app, const std::string& token, const std::string& name, Json arguments,
               bool modern) {
    Json params = Json::object();
    params["name"] = name;
    params["arguments"] = std::move(arguments);
    const McpHttpAnswer answer = app.answer(mcp_post(token, "tools/call", params, modern));
    INFO(answer.response.body);
    const Json body = py::loads(answer.response.body);
    REQUIRE(body.contains("result"));
    return body.at("result");
}

std::string error_text(const Json& result) {
    REQUIRE(result.at("isError") == true);
    const std::string text = result.at("content")[0].at("text").get<std::string>();
    REQUIRE(text.starts_with("Error executing tool "));
    return text.substr(text.find(": ") + 2);
}

} // namespace fv::agentapi::test
