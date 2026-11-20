#include "automation/service.h"

#include <exception>
#include <utility>

#include "automation/control.h"
#include "automation/detail/pyvalue.h"
#include "base/log.h"

namespace fv::automation {

namespace {

constexpr log::Logger kLog{"services.automation.service"};

// Runs `step`; a failure is logged with `what` and its exception's type, never its text.
template <class Step> void guarded(Step&& step, log::Level level, std::string_view what) {
    try {
        step();
    } catch (const std::exception& error) {
        if (level == log::Level::Debug) {
            kLog.debug("{} ({})", what, detail::error_type(error));
        } else {
            kLog.error("{} ({})", what, detail::error_type(error));
        }
    } catch (...) {
        if (level == log::Level::Debug) {
            kLog.debug("{}", what);
        } else {
            kLog.error("{}", what);
        }
    }
}

void call(const std::function<void()>& hook) {
    if (hook) {
        hook();
    }
}

} // namespace

AutomationService::AutomationService(WebhookEngine& webhooks, ProgramEngine& programs,
                                     McpEventsHooks mcp_events,
                                     std::function<void()> install_control)
    : webhooks_(webhooks), programs_(programs), mcp_events_(std::move(mcp_events)),
      install_control_(std::move(install_control)) {}

void AutomationService::start() {
    if (started_) {
        return;
    }
    started_ = true;
    guarded([&] { call(install_control_); }, log::Level::Error,
            "Could not register live API control");
    guarded([&] { webhooks_.start(); }, log::Level::Error, "Could not start webhooks");
    guarded([&] { programs_.start(); }, log::Level::Error, "Could not start programs");
    guarded([&] { call(mcp_events_.start_delivery); }, log::Level::Error,
            "Could not start MCP Events");
}

void AutomationService::reload(std::string_view kind) {
    if (kind == "webhooks") {
        guarded([&] { webhooks_.reload(); }, log::Level::Error, "Could not apply webhooks changes");
    } else if (kind == "programs") {
        guarded([&] { programs_.reload(); }, log::Level::Error, "Could not apply programs changes");
    } else if (kind == "mcp_events") {
        guarded([&] { call(mcp_events_.reload); }, log::Level::Error,
                "Could not apply mcp_events changes");
    }
}

void AutomationService::stop() {
    if (!started_) {
        return;
    }
    started_ = false;
    guarded([&] { webhooks_.stop(); }, log::Level::Debug, "Could not stop webhooks");
    guarded([&] { programs_.stop(); }, log::Level::Debug, "Could not stop programs");
    guarded([&] { call(mcp_events_.stop_delivery); }, log::Level::Debug,
            "Could not stop MCP Events");
    guarded([] { set_app_control(nullptr); }, log::Level::Debug,
            "Could not clear live API control");
}

} // namespace fv::automation
