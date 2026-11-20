#include "meeting_agent/native_core.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

#include "base/identity.h"
#include "meeting_agent/native_tools.h"
#include "meeting_agent/prompts.h"

namespace fv::meeting_agent {
namespace {

// The probe's wall deadline (sidecar.py gave the probe client a 10 s timeout). Measured on the
// core's clock, so a provider that trickles bytes cannot hold initialization open.
constexpr std::chrono::seconds kProbeDeadline{10};

void probe_tools(llm::OpenAiCompatClient& client, const llm::TextLLMProfile& profile,
                 const AgentConfig& config, const CancelToken& parent, const Clock& clock) {
    CancelSource cancel(parent);
    std::atomic<bool> expired{false};
    const auto started = clock.steady();
    std::jthread watchdog([&](std::stop_token stop) {
        while (!stop.stop_requested() && !cancel.token().cancelled()) {
            if (clock.steady() - started >= kProbeDeadline) {
                expired = true;
                cancel.cancel();
                return;
            }
            cancel.token().wait_for(std::chrono::milliseconds(20));
        }
    });
    llm::GenerateRequest request;
    request.model = config.model;
    request.messages =
        Json::array({{{"role", "user"}, {"content", "Call the noop tool with no arguments."}}});
    request.tools = Json::array({native_probe_tool()});
    request.options["max_tokens"] = 512;
    request.cancel = cancel.token();
    try {
        const auto result = llm::generate(client, profile, std::move(request));
        if (std::ranges::none_of(result.tool_calls,
                                 [](const auto& call) { return call.function.name == "noop"; })) {
            // M8.md change 6 (sidecar.py said "...for Pi.").
            throw std::runtime_error(
                "Model did not call the test tool. Choose a tool-capable model for " +
                std::string(identity().product) + ".");
        }
    } catch (const llm::ApiError& error) {
        if (expired) {
            throw std::runtime_error(std::string(llm::kStreamTimeoutError));
        }
        // generate() exposes SDK response bodies. The capability check keeps only the status.
        throw std::runtime_error(error.status == 0 ? "Meeting model capability check failed."
                                                   : llm::stream_status_message(error.status));
    } catch (const llm::TextGenerationError&) {
        if (expired) {
            throw std::runtime_error(std::string(llm::kStreamTimeoutError));
        }
        throw;
    }
}

} // namespace

NativeAgentCore::NativeAgentCore(CoreDeps deps)
    : deps_(std::move(deps)), clock_(deps_.clock ? *deps_.clock : system_clock()) {}

NativeAgentCore::~NativeAgentCore() {
    shutdown();
    std::lock_guard wait(pass_mutex_);
}

void NativeAgentCore::initialize(const AgentConfig& config, std::shared_ptr<AgentToolHost> tools) {
    if (!deps_.http || !tools) {
        throw std::invalid_argument("The meeting agent needs a text client and a tool host.");
    }
    shutdown();
    std::lock_guard pass(pass_mutex_);
    CancelToken token;
    {
        std::lock_guard state(state_mutex_);
        token = generation_.token();
    }
    const auto settings = deps_.settings ? deps_.settings() : Json::object();
    const auto profile = llm::profile_from_agent_config(config.provider, config.endpoint, settings);
    auto key = config.api_key;
    if (!key || key->empty()) {
        key = llm::resolve_api_key(profile, deps_.keys);
    }
    if (!key || key->empty()) {
        throw std::runtime_error("No " + config.provider + " API key found; meeting agent offline");
    }
    if (config.model.empty()) {
        // runner.ts refused initialize the same way, so the agent went offline at start instead
        // of probing model "" or failing every pass on the provider's 400.
        throw std::runtime_error("no model configured (initialize.model is empty)");
    }
    const auto spec = llm::model_spec(profile, config.model);
    if (!spec.tools) {
        throw std::runtime_error("This model cannot use meeting tools with this meeting engine.");
    }
    auto headers = llm::provider_headers(profile, config.meeting_id).value_or(http::Headers{});
    if (!profile.refuses_unsupported_tools()) {
        llm::OpenAiCompatClient client(deps_.http, profile.base_url.value_or(""), *key, headers,
                                       10.0, 0);
        probe_tools(client, profile, config, token, clock_);
    }
    auto session = std::make_shared<Session>(
        Session{AgentLoop(deps_.http, profile, config.model, spec, std::move(headers),
                          std::move(*key), clock_),
                std::move(tools),
                config.system_prompt.empty() ? build_system_prompt() : config.system_prompt});
    std::lock_guard state(state_mutex_);
    if (token.cancelled()) {
        throw std::runtime_error("Meeting agent initialization canceled.");
    }
    session_ = std::move(session);
}

void NativeAgentCore::cancel() {
    CancelSource previous;
    {
        std::lock_guard state(state_mutex_);
        std::swap(previous, generation_);
        previous.cancel();
    }
    std::lock_guard barrier(authority_);
}

bool NativeAgentCore::is_healthy() const {
    std::lock_guard lock(state_mutex_);
    return session_ != nullptr;
}

void NativeAgentCore::shutdown() {
    {
        std::lock_guard state(state_mutex_);
        session_.reset();
        generation_.cancel();
        generation_ = CancelSource{};
    }
    std::lock_guard barrier(authority_);
}

void NativeAgentCore::set_progress_callback(std::function<void(const std::string&)> callback) {
    std::lock_guard lock(state_mutex_);
    progress_ = std::move(callback);
}

void NativeAgentCore::set_activity_callback(std::function<void(const AgentActivity&)> callback) {
    std::lock_guard lock(state_mutex_);
    activity_ = std::move(callback);
}

} // namespace fv::meeting_agent
