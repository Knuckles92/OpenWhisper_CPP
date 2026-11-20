#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "agentapi/history_server.h"
#include "automation/control.h"
#include "automation/event_bus.h"
#include "automation/permissions.h"
#include "base/json.h"
#include "net/http_server.h"
#include "settings/testing/memory_store.h"

// Shared by the MCP tests of fv_agentapi.
namespace fv::agentapi::test {

// The FakeAppControl of tests/test_automation_mcp_tools.py: state() and list_actions() answer
// from fields, every call is recorded, and `fail` (a ControlError or a PermissionDenied) is what
// every call but state() and list_actions() raises until it is cleared.
// Affinity: thread-safe.
class LiveControl final : public automation::AppControl {
public:
    struct Failure {
        std::optional<automation::ControlError> control;
        std::optional<std::string> permission; // a PermissionDenied for this settings key
    };

    automation::LiveState state() override;
    Json list_actions() override;
    std::string run_action(std::string_view action_id, std::string_view text,
                           std::string_view source) override;
    std::string start_recording(std::optional<std::string> action_id,
                                std::string_view source) override;
    void stop_recording() override;
    void cancel() override;
    void notify(std::string_view text, std::string_view title) override;

    void fail(std::optional<Failure> failure);
    // ("run_action", action_id, text, source), ("start_recording", action_id, source), ...
    std::vector<std::vector<std::string>> calls() const;

    automation::LiveState live{true, true, false, "action", "summarize", "job_rec", "9.9.9"};
    Json actions = Json::parse(R"([{"id": "summarize", "name": "Summarize", "kind": "webhook"}])");
    std::string run_job = "job_run";
    std::string start_job = "job_start";

private:
    void record(std::vector<std::string> call);

    mutable std::mutex mutex_;
    std::vector<std::vector<std::string>> calls_;
    std::optional<Failure> failure_;
};

// Registers a LiveControl as the app control, and a fresh event bus numbering from 0, for its
// lifetime; both are restored afterwards.
class LiveScope {
public:
    explicit LiveScope(std::size_t buffer_size = automation::DEFAULT_BUFFER_SIZE);
    ~LiveScope();
    LiveScope(const LiveScope&) = delete;
    LiveScope& operator=(const LiveScope&) = delete;

    std::shared_ptr<LiveControl> control = std::make_shared<LiveControl>();
    std::shared_ptr<automation::EventBus> bus;

private:
    std::shared_ptr<automation::AppControl> previous_control_;
    std::shared_ptr<automation::EventBus> previous_bus_;
};

// An MCP POST as tests/test_automation_mcp_tools.py's rpc() sends it: Host 127.0.0.1, the bearer
// token, Accept "application/json, text/event-stream", Content-Type application/json; `modern`
// adds the 2026-07-28 envelope and its headers.
net::HttpRequest mcp_post(const std::string& token, const std::string& method, Json params,
                          bool modern, Json id = 1);

// One tools/call through the app; the JSON-RPC result object (the test fails without one).
Json call_tool(McpApp& app, const std::string& token, const std::string& name,
               Json arguments = Json::object(), bool modern = true);
// The text of an error result, without "Error executing tool <name>: ".
std::string error_text(const Json& result);

} // namespace fv::agentapi::test
