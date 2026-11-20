#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "automation/control.h"
#include "automation/event_bus.h"
#include "automation/programs.h"
#include "automation/service.h"
#include "automation/testing/fake_app_control.h"
#include "automation/webhooks.h"
#include "credentials/testing/memory_backend.h"
#include "process/run_once.h"
#include "settings/testing/memory_store.h"
#include "support/hook_support.h"

// AutomationService (services/automation/service.py): live API control, then delivery to
// webhooks, program hooks and MCP Events, each started, reloaded and stopped on its own, a failure
// of one logged without stopping the others. OpenWhisper has no test of its own for it.

using namespace fv;
using namespace fv::automation;

namespace {

Json hooks_settings(bool enabled) {
    return Json{{"automation_webhooks",
                 Json::array({Json{{"id", "w"},
                                   {"name", "Notes"},
                                   {"url", "http://127.0.0.1:9/hook"},
                                   {"events", Json::array({"transcription.completed"})},
                                   {"enabled", enabled}}})},
                {"automation_programs",
                 Json::array({Json{{"id", "p"},
                                   {"name", "Log"},
                                   {"program", test::hook_child()},
                                   {"args", Json::array()},
                                   {"events", Json::array({"transcription.completed"})},
                                   {"enabled", enabled},
                                   {"timeout_s", 30}}})}};
}

struct Service {
    Service()
        : settings(hooks_settings(true)),
          webhooks(settings, credentials, test::loopback_client(), bus),
          programs(
              settings, bus,
              ProgramOptions{.run_once =
                                 [](const std::vector<std::string>&, std::string_view, double,
                                    const CancelToken&, const std::map<std::string, std::string>&) {
                                     return RunOnceResult{};
                                 }}),
          service(webhooks, programs,
                  McpEventsHooks{.start_delivery = [this] { ++mcp_started; },
                                 .reload = [this] { ++mcp_reloaded; },
                                 .stop_delivery =
                                     [this] {
                                         ++mcp_stopped;
                                     }},
                  [this] {
                      ++installs;
                      set_app_control(control);
                  }) {}
    ~Service() {
        service.stop();
        set_app_control(nullptr);
        bus.close();
    }

    MemorySettingsStore settings;
    CredentialStore credentials{std::make_shared<MemoryCredentialBackend>(), identity()};
    EventBus bus;
    WebhookEngine webhooks;
    ProgramEngine programs;
    std::shared_ptr<FakeAppControl> control = std::make_shared<FakeAppControl>();
    int mcp_started = 0;
    int mcp_reloaded = 0;
    int mcp_stopped = 0;
    int installs = 0;
    AutomationService service;
};

} // namespace

TEST_CASE("start registers live control, then starts webhooks, programs and MCP Events once",
          "[automation_service]") {
    Service s;
    s.service.start();
    s.service.start();

    CHECK(s.service.started());
    CHECK(s.installs == 1);
    CHECK(app_control() == s.control);
    CHECK(s.bus.subscribers("webhooks") == 1);
    CHECK(s.bus.subscribers("programs") == 1);
    CHECK(s.mcp_started == 1);
}

TEST_CASE("reload applies saved changes to the kind that changed", "[automation_service]") {
    Service s;
    s.service.start();
    s.settings.save_all(hooks_settings(false));

    s.service.reload("webhooks");
    CHECK(s.bus.subscribers("webhooks") == 0);
    CHECK(s.bus.subscribers("programs") == 1);

    s.service.reload("programs");
    CHECK(s.bus.subscribers("programs") == 0);

    s.service.reload("mcp_events");
    CHECK(s.mcp_reloaded == 1);
    s.service.reload("filters"); // nothing to reload for it
    CHECK(s.mcp_reloaded == 1);
}

TEST_CASE("a part that fails to start is logged and the others still start",
          "[automation_service]") {
    test::ScopedLogCapture log;
    MemorySettingsStore settings(hooks_settings(true));
    CredentialStore credentials{std::make_shared<MemoryCredentialBackend>(), identity()};
    EventBus bus;
    WebhookEngine webhooks(settings, credentials, test::loopback_client(), bus);
    ProgramEngine programs(settings, bus);
    AutomationService service(webhooks, programs,
                              McpEventsHooks{.start_delivery =
                                                 [] {
                                                     throw std::runtime_error("no MCP server");
                                                 }},
                              [] { throw std::runtime_error("no controller"); });

    service.start();

    CHECK(bus.subscribers("webhooks") == 1);
    CHECK(bus.subscribers("programs") == 1);
    const std::string text = log.text();
    CHECK(text.find("Could not register live API control") != std::string::npos);
    CHECK(text.find("Could not start MCP Events") != std::string::npos);
    CHECK(text.find("no MCP server") == std::string::npos); // the type only, never the text
    service.stop();
    bus.close();
}

TEST_CASE("stop ends delivery and clears live control, once", "[automation_service]") {
    Service s;
    s.service.start();

    s.service.stop();
    s.service.stop();

    CHECK_FALSE(s.service.started());
    CHECK(s.bus.subscribers("webhooks") == 0);
    CHECK(s.bus.subscribers("programs") == 0);
    CHECK(s.mcp_stopped == 1);
    CHECK(app_control() == nullptr);

    // A later start starts everything again.
    s.service.start();
    CHECK(s.bus.subscribers("webhooks") == 1);
    CHECK(s.installs == 2);
}

TEST_CASE("stop before start does nothing", "[automation_service]") {
    Service s;
    s.service.stop();
    CHECK(s.mcp_stopped == 0);
}
