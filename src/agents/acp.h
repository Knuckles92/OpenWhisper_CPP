#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agents/agent_config.h"
#include "base/cancel.h"
#include "base/json.h"
#include "process/launcher.h"

// A small Agent Client Protocol client over an agent's stdio (meeting/agent/installed/acp.py).
// ACP is JSON-RPC 2.0, one message per line. The client asks (initialize, session/new,
// session/prompt, ...); the agent streams session/update notifications and may ask the client
// things of its own (session/request_permission, fs/*, terminal/*), which are refused unless a
// handler says otherwise.
namespace fv::agents {

inline constexpr int ACP_PROTOCOL_VERSION = 1;
// {"name": <product in lower case>, "title": <product>, "version": "1"}
Json acp_client_info();
// The app never lets an agent touch files or run commands.
Json acp_client_capabilities();

// An ACP request failed: an error reply, a timeout, or a dead agent.
class AcpError : public std::runtime_error {
public:
    explicit AcpError(const std::string& message, std::optional<std::int64_t> code = std::nullopt,
                      Json data = nullptr)
        : std::runtime_error(message), code_(code), data_(std::move(data)) {}
    std::optional<std::int64_t> code() const { return code_; }
    const Json& data() const { return data_; }

private:
    std::optional<std::int64_t> code_;
    Json data_;
};

// cb(method, params) for an agent notification, on the reader thread.
using AcpNotificationHandler = std::function<void(const std::string& method, const Json& params)>;
// cb(method, params) -> result for an agent request, on the reader thread; throw AcpError to
// refuse. Unhandled requests are refused.
using AcpRequestHandler = std::function<Json(const std::string& method, const Json& params)>;

enum class AcpDiagnostics { standard, private_data };

// One agent process speaking ACP on stdio. Destroying it closes it.
// Affinity: thread-safe; one thread at a time may wait in request() per request.
class AcpConnection {
public:
    AcpConnection(std::shared_ptr<ProcessLauncher> launcher, std::vector<std::string> argv,
                  std::map<std::string, std::string> env, std::filesystem::path cwd,
                  AcpNotificationHandler on_notification = {}, AcpRequestHandler on_request = {},
                  AcpDiagnostics diagnostics = AcpDiagnostics::standard);
    ~AcpConnection();
    AcpConnection(const AcpConnection&) = delete;
    AcpConnection& operator=(const AcpConnection&) = delete;

    // Starts the agent. Throws AcpError when it can't run.
    void start();
    bool alive() const;
    // The agent's last few stderr lines (at most 40, each at most 400 characters).
    std::string stderr_tail() const;

    // Sends a request and waits for its reply's result. Throws AcpError on an error reply, a
    // timeout ("{method} timed out after {timeout_s:.0f}s"), a stall (no progress for stall_s
    // by last_activity, a steady-clock time), `cancel` ("canceled"; the caller sends any
    // protocol-level cancel itself), or the agent's exit.
    Json request(std::string_view method, Json params, double timeout_s,
                 const CancelToken& cancel = {}, std::optional<double> stall_s = std::nullopt,
                 std::function<std::chrono::steady_clock::time_point()> last_activity = {});
    // Sends a notification; a dead agent is ignored.
    void notify(std::string_view method, Json params);
    // Closes stdin, gives the agent `wait_s` to exit, then stops its tree; pending requests
    // fail with "the agent was closed".
    void close(double wait_s = 3.0);

private:
    struct Shared;
    std::shared_ptr<ProcessLauncher> launcher_;
    std::vector<std::string> argv_;
    std::map<std::string, std::string> env_;
    std::filesystem::path cwd_;
    std::shared_ptr<Shared> shared_;
    std::unique_ptr<Process> process_;
};

// Runs the ACP handshake; the agent's initialize result ({} when it is not an object).
Json acp_initialize(AcpConnection& connection, double timeout_s = 30.0,
                    const CancelToken& cancel = {});
// One of a session's configOptions by id, or null.
Json acp_config_option(const Json& session, std::string_view option_id);
// The values of a select config option, grouped or not.
std::vector<std::string> acp_option_values(const Json& session, std::string_view option_id);
// (value, label) for every model a session/new result offers.
std::vector<AgentModel> acp_model_choices(const Json& session);

class InstalledAgents;
// Every model the user's OpenCode can run, through a short ACP handshake in the agent
// workspace: starts `opencode acp`, opens and deletes one session, and exits. OpenCode's
// background service is not started. Throws AcpError (or what a malformed reply makes Python
// raise) when it fails.
std::vector<AgentModel> list_opencode_models(const std::filesystem::path& opencode,
                                             const InstalledAgents& agents,
                                             double timeout_s = 20.0);

} // namespace fv::agents
