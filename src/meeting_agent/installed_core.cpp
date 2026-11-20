#include "meeting_agent/installed_core.h"

#include <algorithm>
#include <exception>
#include <utility>
#include <vector>

#include "base/log.h"
#include "base/utf8.h"
#include "llm/thinking_levels.h"
#include "meeting_agent/agent_settings.h"
#include "meeting_agent/progress_labels.h"
#include "meeting_agent/prompts.h"
#include "meeting_agent/scheduler.h"
#include "meeting_agent/tool_policy.h"
#include "meeting_agent/tool_specs.h"

namespace fv::meeting_agent {

namespace {

constexpr log::Logger logger("meeting.agent.installed.core");

// The polish budget of meeting/finalization.py, in seconds.
constexpr double kPolishTimeoutS = 180.0;

// What one pass's tool handler shares with the pass: the handler may be called on the tool
// server's thread, during the pass or (refused) after it.
struct PassState {
    std::mutex mutex;
    std::vector<meeting_state::OpResult> results;
};

} // namespace

InstalledAgentCore::InstalledAgentCore(std::string agent_id, std::function<Json()> settings,
                                       net::Reactor& reactor,
                                       std::shared_ptr<ProcessLauncher> launcher,
                                       agents::DriverFactory make, const Clock& clock)
    : agent_id_(std::move(agent_id)), settings_(std::move(settings)), reactor_(reactor),
      launcher_(std::move(launcher)), make_(std::move(make)), clock_(clock) {}

InstalledAgentCore::~InstalledAgentCore() {
    try {
        shutdown();
    } catch (const std::exception& error) {
        logger.warning("Agent shutdown failed: {}", error.what());
    }
}

std::string InstalledAgentCore::name() const {
    return std::string(agents::agent_spec(agent_id_).name);
}

void InstalledAgentCore::initialize(const AgentConfig& config,
                                    std::shared_ptr<AgentToolHost> tools) {
    const Json settings = settings_ ? settings_() : Json::object();
    // Live passes favour speed; the final report may think harder.
    const auto offered = llm::agent_levels(agent_id_, config.model);
    const std::string live = llm::effective_level(resolve_meeting_live_thinking(settings), offered);
    const std::string final_level =
        llm::effective_level(resolve_meeting_final_thinking(settings), offered);
    {
        std::lock_guard lock(mutex_);
        config_ = config;
        tools_ = std::move(tools);
        shut_down_ = false;
        live_effort_ = live;
        final_effort_ = final_level;
    }
    const agents::InstalledAgent agent = agents::installed_agent(agent_id_);
    auto server = std::make_shared<agents::LoopbackMcpServer>(reactor_);
    server->start();
    std::shared_ptr<agents::Driver> driver =
        make_ ? make_(agent) : agents::make_driver(agent, launcher_);
    const Json tool_definitions = mcp_tool_definitions();
    const std::string contract =
        agents::tool_contract(tool_definitions, name(), driver->tool_prefix());
    // "build" writes cards, polish and the final report under the copilot charter; "plan" is
    // the note taker.
    std::map<std::string, std::string> personas{
        {"build", config.system_prompt + "\n\n" + contract},
        {"plan", build_note_taker_system_prompt() + "\n\n" + contract}};
    try {
        driver->start(*server, personas, tool_definitions);
    } catch (...) {
        // A driver may have launched its process before failing to warm up.
        try {
            driver->close();
        } catch (const std::exception& error) {
            logger.warning("Could not close the agent driver: {}", error.what());
        }
        driver.reset();
        server->stop();
        throw;
    }
    // A core initialized again closes the driver it replaces before that driver's server goes:
    // a driver keeps a raw pointer to its server until its first close().
    std::shared_ptr<agents::LoopbackMcpServer> old_server;
    std::shared_ptr<agents::Driver> old_driver;
    {
        std::lock_guard lock(mutex_);
        personas_ = std::move(personas);
        old_server = std::exchange(server_, std::move(server));
        old_driver = std::exchange(driver_, std::move(driver));
        initialized_ = true;
    }
    if (old_driver) {
        try {
            old_driver->close();
        } catch (const std::exception& error) {
            logger.warning("Agent driver close failed: {}", error.what());
        }
        old_driver.reset();
    }
    if (old_server) {
        old_server->stop();
    }
    logger.info("Meeting agent ready: {} {} at {} (model={})", name(), agent.version,
                path_to_utf8(agent.path), config.model.empty() ? "default" : config.model);
}

AgentResult InstalledAgentCore::checkpoint(const CheckpointPayload& payload) {
    if (payload.is_consolidation) {
        return consolidate(payload);
    }
    const double timeout_s = payload.is_polish ? std::max(kPolishTimeoutS, INSTALLED_LIVE_TIMEOUT_S)
                                               : INSTALLED_LIVE_TIMEOUT_S;
    std::string effort;
    {
        std::lock_guard lock(mutex_);
        effort = live_effort_;
    }
    return run(payload, timeout_s, INSTALLED_LIVE_STALL_S, effort);
}

AgentResult InstalledAgentCore::consolidate(const CheckpointPayload& payload) {
    std::string effort;
    {
        std::lock_guard lock(mutex_);
        effort = final_effort_;
    }
    return run(payload, CONSOLIDATION_TIMEOUT_CAP_S, agents::CONSOLIDATION_STALL_S, effort);
}

void InstalledAgentCore::cancel() {
    std::vector<std::shared_ptr<CancelSource>> sources;
    {
        std::lock_guard lock(mutex_);
        sources.assign(active_.begin(), active_.end());
    }
    for (const auto& source : sources) {
        source->cancel();
    }
}

bool InstalledAgentCore::is_healthy() const {
    std::shared_ptr<agents::Driver> driver;
    {
        std::lock_guard lock(mutex_);
        if (!initialized_ || shut_down_ || !driver_) {
            return false;
        }
        driver = driver_;
    }
    return driver->healthy();
}

void InstalledAgentCore::shutdown() {
    {
        std::lock_guard lock(mutex_);
        shut_down_ = true;
    }
    cancel();
    std::shared_ptr<agents::LoopbackMcpServer> server;
    std::shared_ptr<agents::Driver> driver;
    {
        std::lock_guard lock(mutex_);
        driver = std::move(driver_);
        server = std::move(server_);
        initialized_ = false;
    }
    if (driver) {
        try {
            driver->close();
        } catch (const std::exception& error) {
            logger.warning("Agent driver close failed: {}", error.what());
        }
    }
    if (server) {
        server->stop();
    }
}

std::optional<CheckpointCadence> InstalledAgentCore::cadence() const {
    return INSTALLED_AGENT_CADENCE;
}

void InstalledAgentCore::set_progress_callback(std::function<void(const std::string&)> callback) {
    std::lock_guard lock(mutex_);
    progress_cb_ = std::move(callback);
}

void InstalledAgentCore::set_activity_callback(std::function<void(const AgentActivity&)> callback) {
    std::lock_guard lock(mutex_);
    activity_cb_ = std::move(callback);
}

AgentResult InstalledAgentCore::run(const CheckpointPayload& payload, double timeout_s,
                                    double stall_s, const std::string& effort) {
    if (!is_healthy()) {
        AgentResult unavailable;
        unavailable.error = "agent_unavailable";
        return unavailable;
    }
    // Declared before the driver: the server outlives every use the driver makes of it.
    std::shared_ptr<agents::LoopbackMcpServer> server;
    std::shared_ptr<agents::Driver> driver;
    std::shared_ptr<AgentToolHost> tools_host;
    std::string system_prompt;
    std::string model;
    const std::string persona = payload.is_notes ? "plan" : "build";
    {
        std::lock_guard lock(mutex_);
        if (shut_down_ || !driver_ || !config_ || !tools_) {
            AgentResult unavailable;
            unavailable.error = "agent_unavailable";
            return unavailable;
        }
        server = server_;
        driver = driver_;
        tools_host = tools_;
        system_prompt = personas_[persona];
        model = config_->model;
    }
    const ToolScope scope = ToolScope::for_payload(payload);
    auto state = std::make_shared<PassState>();
    auto cancel = std::make_shared<CancelSource>();
    const CancelToken token = cancel->token();

    agents::PassRequest request;
    request.system_prompt = system_prompt;
    request.user_prompt =
        payload.is_notes
            ? build_notes_user_prompt(payload.state_snapshot, payload.new_segments)
            : build_checkpoint_user_prompt(payload.state_snapshot, payload.new_segments,
                                           payload.is_consolidation, payload.is_polish);
    request.tools = mcp_tool_definitions();
    request.handler = [state, token, tools_host,
                       scope](std::string_view tool, const Json& args,
                              const Json&) -> std::pair<std::string, bool> {
        if (token.cancelled()) {
            return {"This meeting pass was canceled.", true};
        }
        auto [out, op_results] = run_tool(*tools_host, tool, args, scope);
        {
            std::lock_guard lock(state->mutex);
            state->results.insert(state->results.end(), op_results.begin(), op_results.end());
        }
        return {tool_result_text(tool, out), false};
    };
    request.model = model;
    request.effort = effort;
    request.persona = persona;
    request.timeout_s = timeout_s;
    request.stall_s = stall_s;
    request.cancel = token;
    request.on_event = [this, pass_kind = scope.pass_kind](std::string_view kind,
                                                           std::string_view tool) {
        on_event(kind, tool, pass_kind);
    };
    {
        std::lock_guard lock(mutex_);
        active_.insert(cancel);
    }
    logger.info("Dispatching {} pass request_id={} to {} ({} segments)", scope.pass_kind,
                payload.request_id, name(), payload.new_segments.size());
    agents::PassOutcome outcome;
    try {
        outcome = driver->run_pass(request);
    } catch (const std::exception& error) {
        logger.error_with(error, name() + " pass failed");
        outcome = agents::PassOutcome{};
        outcome.error = error.what();
    } catch (...) {
        // Whatever a driver throws fails this pass only; the pass's authority still ends below.
        logger.error("{} pass failed", name());
        outcome = agents::PassOutcome{};
        outcome.error = "unknown error";
    }
    // No authority after the run, whatever the agent does.
    cancel->cancel();
    {
        std::lock_guard lock(mutex_);
        active_.erase(cancel);
    }
    std::vector<meeting_state::OpResult> results;
    {
        std::lock_guard lock(state->mutex);
        results = state->results;
    }
    const auto applied = std::count_if(results.begin(), results.end(),
                                       [](const meeting_state::OpResult& r) { return r.ok; });
    logger.info("{} pass request_id={} {}: {}/{} ops applied{}", name(), payload.request_id,
                outcome.ok ? "ok" : "failed", applied, results.size(),
                outcome.error.empty() ? std::string() : " (" + outcome.error + ")");
    AgentResult result;
    result.op_results = std::move(results);
    result.usage = outcome.usage;
    if (outcome.canceled) {
        result.error = "canceled";
        return result;
    }
    if (!outcome.ok) {
        result.error = outcome.error;
        return result;
    }
    result.ok = true;
    return result;
}

void InstalledAgentCore::on_event(std::string_view kind, std::string_view tool,
                                  const std::string& pass_kind) {
    const auto mapped = pi_event(kind);
    const std::string_view event = mapped ? mapped->first : std::string_view("update");
    const std::string_view delta = mapped ? mapped->second : std::string_view();
    const std::string detail = progress_detail(event, delta, pass_kind);

    std::function<void(const std::string&)> progress;
    std::function<void(const AgentActivity&)> activity;
    bool report = false;
    {
        std::lock_guard lock(mutex_);
        progress = progress_cb_;
        activity = activity_cb_;
        // Streaming agents report every token; the finalization card needs a change of wording,
        // or a heartbeat.
        const auto now = clock_.steady();
        if (progress && (detail != last_progress_ || !last_progress_at_ ||
                         now - *last_progress_at_ >=
                             std::chrono::duration<double>(INSTALLED_PROGRESS_MIN_INTERVAL_S))) {
            last_progress_ = detail;
            last_progress_at_ = now;
            report = true;
        }
    }
    if (report) {
        try {
            progress(detail);
        } catch (const std::exception& error) {
            // Called per streamed event: warn once, then stay quiet.
            bool first = false;
            {
                std::lock_guard lock(mutex_);
                first = !progress_cb_warned_;
                progress_cb_warned_ = true;
            }
            if (first) {
                logger.warning("Progress callback failed: {}", error.what());
            } else {
                logger.debug("Progress callback failed: {}", error.what());
            }
        }
    }
    if (activity) {
        try {
            AgentActivity tick;
            tick.kind = activity_kind(event, delta);
            tick.label = detail;
            tick.tool = std::string(tool);
            tick.pass_kind = pass_kind;
            tick.ts = utc_now_iso(clock_);
            activity(tick);
        } catch (const std::exception& error) {
            bool first = false;
            {
                std::lock_guard lock(mutex_);
                first = !activity_cb_warned_;
                activity_cb_warned_ = true;
            }
            if (first) {
                logger.warning("Activity callback failed: {}", error.what());
            } else {
                logger.debug("Activity callback failed: {}", error.what());
            }
        }
    }
}

} // namespace fv::meeting_agent
