#include "agents/agent_tasks.h"

#include <format>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "agents/detail/long_path.h"
#include "agents/loopback_mcp.h"
#include "base/log.h"
#include "pyport/text.h"

namespace fv::agents {

namespace {

constexpr log::Logger logger("meeting.agent.installed.core");

struct Services {
    std::mutex mutex;
    net::Reactor* reactor = nullptr;
    std::shared_ptr<ProcessLauncher> launcher;
    DriverFactory make;
};

Services& services() {
    static auto* value = new Services();
    return *value;
}

struct TaskParts {
    net::Reactor* reactor;
    DriverFactory make;
};

TaskParts task_parts() {
    auto& shared = services();
    std::lock_guard lock(shared.mutex);
    if (shared.reactor == nullptr) {
        throw std::logic_error("The agent task services are not set.");
    }
    if (shared.make) {
        return {shared.reactor, shared.make};
    }
    return {shared.reactor, [launcher = shared.launcher](const InstalledAgent& agent) {
                return make_driver(agent, launcher);
            }};
}

ToolHandler no_tools() {
    return [](std::string_view, const Json&, const Json&) {
        return std::pair<std::string, bool>("No tools here.", true);
    };
}

// driver.close(), then server.stop(), whatever happened. The driver is destroyed before the
// server it may still name.
struct Running {
    LoopbackMcpServer server;
    std::unique_ptr<Driver> driver;
    explicit Running(net::Reactor& reactor) : server(reactor) {}
    ~Running() {
        if (driver) {
            try {
                driver->close();
            } catch (const std::exception& error) {
                logger.warning("Could not close the agent: {}", error.what());
            }
        }
        server.stop();
    }
    Running(const Running&) = delete;
    Running& operator=(const Running&) = delete;
};

} // namespace

std::string_view meeting_agent_setting() {
    return "Settings \xE2\x86\x92 Meeting Mode \xE2\x86\x92 Intelligence";
}

InstalledAgent installed_agent(std::string_view agent_id) {
    const auto* spec = find_agent_spec(agent_id);
    if (spec == nullptr) {
        throw AgentUnavailable(std::format("Unknown meeting agent {}.", py::repr(agent_id)));
    }
    auto agent = installed_agents().resolve_agent(agent_id);
    if (!agent) {
        throw AgentUnavailable(std::format("{} is not installed on this computer. Choose another "
                                           "agent in {}.",
                                           spec->name, meeting_agent_setting()));
    }
    if (!agent->problem.empty()) {
        throw AgentUnavailable(agent->problem);
    }
    if (agent->signed_in == false) {
        throw AgentUnavailable(
            std::format("{} is not signed in. {}", spec->name, sign_in_hint(agent_id)));
    }
    return std::move(*agent);
}

void set_agent_task_services(net::Reactor& reactor, std::shared_ptr<ProcessLauncher> launcher,
                             DriverFactory make) {
    auto& shared = services();
    std::lock_guard lock(shared.mutex);
    shared.reactor = &reactor;
    shared.launcher = std::move(launcher);
    shared.make = std::move(make);
}

void clear_agent_task_services() {
    auto& shared = services();
    std::lock_guard lock(shared.mutex);
    shared.reactor = nullptr;
    shared.launcher.reset();
    shared.make = {};
}

PassOutcome run_agent_task(std::string_view agent_id, std::string_view model,
                           std::string_view system_prompt, std::string_view user_prompt,
                           const Json& tools, ToolHandler handler, double timeout_s,
                           std::string_view closing, std::string_view effort,
                           const CancelToken& cancel) {
    const auto agent = installed_agent(agent_id);
    const auto name = agent_spec(agent_id).name;
    auto parts = task_parts();
    Running running(*parts.reactor);
    running.server.start();
    running.driver = parts.make(agent);
    const auto charter = std::string(system_prompt) + "\n\n" +
                         tool_contract(tools, name, running.driver->tool_prefix(), closing);
    running.driver->start(running.server, {{"build", charter}}, tools);
    PassRequest request;
    request.system_prompt = charter;
    request.user_prompt = std::string(user_prompt);
    request.tools = tools;
    request.handler = [handler = std::move(handler)](std::string_view tool, const Json& args,
                                                     const Json&) {
        return handler(tool, args, Json::object());
    };
    request.model = std::string(model);
    request.effort = std::string(effort);
    request.timeout_s = timeout_s;
    request.stall_s = CONSOLIDATION_STALL_S;
    request.cancel = cancel;
    return running.driver->run_pass(request);
}

PassOutcome run_reading_task(std::string_view agent_id, std::string_view model,
                             std::string_view system_prompt, std::string_view user_prompt,
                             const std::filesystem::path& folder, double timeout_s, double stall_s,
                             std::string_view effort, const CancelToken& cancel) {
    const auto agent = installed_agent(agent_id);
    auto parts = task_parts();
    auto driver = parts.make(agent);
    if (!driver->reads_folders()) {
        throw AgentUnavailable(std::format("{} can't be limited to one folder, so it can't read "
                                           "the walkthrough. Use Claude Code.",
                                           agent_spec(agent_id).name));
    }
    Running running(*parts.reactor);
    running.driver = std::move(driver);
    running.server.start();
    running.driver->start(running.server);
    if (!running.driver->supports_read_folder()) {
        throw AgentUnavailable(std::format("Claude Code {} is too old to read the walkthrough. Run "
                                           "`claude update`.",
                                           agent.version));
    }
    PassRequest request;
    request.system_prompt = std::string(system_prompt);
    request.user_prompt = std::string(user_prompt);
    request.handler = no_tools();
    request.model = std::string(model);
    request.effort = std::string(effort);
    request.timeout_s = timeout_s;
    request.stall_s = stall_s;
    request.cancel = cancel;
    // Its long form: Claude Code refuses an 8.3 short path.
    request.read_folder = detail::long_real_path(folder);
    return running.driver->run_pass(request);
}

PassOutcome run_viewing_task(std::string_view agent_id, std::string_view model,
                             std::string_view system_prompt, std::string_view user_prompt,
                             const std::vector<std::filesystem::path>& images, double timeout_s,
                             double stall_s, std::string_view effort, const CancelToken& cancel) {
    const auto agent = installed_agent(agent_id);
    auto parts = task_parts();
    auto driver = parts.make(agent);
    if (driver->reads_folders()) {
        throw AgentUnavailable("Claude Code reads the folder instead: use run_reading_task.");
    }
    Running running(*parts.reactor);
    running.driver = std::move(driver);
    running.server.start();
    // OpenCode fixes its system prompt per process, so it is the build persona.
    running.driver->start(running.server, {{"build", std::string(system_prompt)}}, Json::array());
    PassRequest request;
    request.system_prompt = std::string(system_prompt);
    request.user_prompt = std::string(user_prompt);
    request.handler = no_tools();
    request.model = std::string(model);
    request.effort = std::string(effort);
    request.timeout_s = timeout_s;
    request.stall_s = stall_s;
    request.cancel = cancel;
    request.images = images;
    return running.driver->run_pass(request);
}

} // namespace fv::agents
