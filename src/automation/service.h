#pragma once

#include <functional>
#include <string_view>

#include "automation/programs.h"
#include "automation/webhooks.h"

// Starts and stops the background parts of automation with the app
// (services/automation/service.py): control for the live API, then delivery to webhooks,
// program hooks and MCP Events. A part that fails to start, reload or stop is logged and the
// others go on.
namespace fv::automation {

// MCP Events delivery (agentapi's events_ext), which fv_automation cannot reach itself; an
// empty member does nothing.
struct McpEventsHooks {
    std::function<void()> start_delivery;
    std::function<void()> reload;
    std::function<void()> stop_delivery;
};

// Affinity: UI actor.
class AutomationService {
public:
    // `install_control` registers the live API's AppControl (set_app_control); empty registers
    // nothing. The engines must outlive the service.
    AutomationService(WebhookEngine& webhooks, ProgramEngine& programs,
                      McpEventsHooks mcp_events = {}, std::function<void()> install_control = {});
    AutomationService(const AutomationService&) = delete;
    AutomationService& operator=(const AutomationService&) = delete;

    // After the window shows; a second call does nothing until stop().
    void start();
    // Settings changed `kind`: "webhooks", "programs" or "mcp_events"; any other does nothing.
    void reload(std::string_view kind);
    // Stops delivery and clears the live API's control; nothing when not started.
    void stop();
    bool started() const { return started_; }

private:
    WebhookEngine& webhooks_;
    ProgramEngine& programs_;
    McpEventsHooks mcp_events_;
    std::function<void()> install_control_;
    bool started_ = false;
};

} // namespace fv::automation
