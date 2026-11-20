// Host Mode's controller side (test_host_mode.py, "the controllers" and the main window's MCP
// controls): the dashboard the window builds reads the context's service and MCP control, its
// picker switches this computer's engine the way a paired computer's request does, and a refusal
// is said under the picker.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>

#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QPointer>

#include "agentapi/testing/fake_mcp_control.h"
#include "appkit/app_context.h"
#include "base/testing/manual_executor.h"
#include "models/testing/fake_engine_control.h"
#include "remote/testing/fake_remote_engine.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "ui_remote/host_dashboard.h"
#include "ui_remote/host_model_switch.h"
#include "ui_remote/remote_feature.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/wrapped_label.h"

namespace {

using fv::Json;

// An AppContext holding what the window's dashboard factory looks up.
struct HostContext {
    fv::MemorySettingsStore settings{Json::object()};
    fv::ManualExecutor io;
    fv::ManualExecutor ui;
    std::shared_ptr<fv::remote::testing::FakeRemoteEngine> engine =
        std::make_shared<fv::remote::testing::FakeRemoteEngine>();
    std::shared_ptr<fv::models::FakeEngineControl> control =
        std::make_shared<fv::models::FakeEngineControl>();
    std::shared_ptr<fv::agentapi::FakeMcpControl> mcp =
        std::make_shared<fv::agentapi::FakeMcpControl>();
    fv::AppContext context{services()};

    HostContext() {
        engine->state.enabled = true;
        engine->state.running = true;
        engine->state.host_name = "devbox";
        engine->engine = {{"family", "parakeet"},
                          {"model", "parakeet-v3"},
                          {"label", "Parakeet TDT 0.6B v3"},
                          {"available", true}};
        engine->state.engine = engine->engine;
        engine->models = {{"parakeet", "parakeet-v3", "Parakeet TDT 0.6B v3"},
                          {"parakeet", "orukeet-v0.1", "Orukeet TDT 0.6B"}};
        mcp->current.state = "running";
        mcp->running_token = "test-mcp-access-token";
    }

    fv::AppServices services() {
        fv::AppServices made;
        made.settings = &settings;
        made.io = &io;
        made.ui = &ui;
        return made;
    }

    void provideAll() {
        context.provide<fv::remote::RemoteEngineControl>(engine);
        context.provide<fv::models::EngineControl>(control);
        context.provide<fv::agentapi::McpServerControl>(mcp);
    }

    void pump() {
        for (int round = 0; round < 20; ++round) {
            const auto ran = io.run_all() + ui.run_all();
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            if (ran == 0) {
                return;
            }
        }
    }
};

} // namespace

TEST_CASE("test_the_dashboard_reads_the_service_the_controller_is_given", "[widget]") {
    HostContext host;
    SECTION("with a service") {
        host.provideAll();
        std::unique_ptr<fv::HostDashboard> page(fv::buildHostDashboard(host.context));
        CHECK(host.engine->calls().empty());
        page->refresh();
        host.pump();
        CHECK(page->parts().heroTitle->text() == QStringLiteral("Sharing Parakeet TDT 0.6B v3"));
    }
    SECTION("without one") {
        std::unique_ptr<fv::HostDashboard> page(fv::buildHostDashboard(host.context));
        page->refresh();
        host.pump();
        CHECK(page->parts().heroTitle->text() == QStringLiteral("Host Mode isn't available here"));
    }
}

TEST_CASE("test_main_host_window_has_the_shared_mcp_copy_controls", "[widget]") {
    HostContext host;
    host.provideAll();
    std::unique_ptr<fv::HostDashboard> page(fv::buildHostDashboard(host.context));
    std::optional<QString> clipboard;
    page->setClipboardWriter([&](const QString& text) { clipboard = text; });
    page->refresh();
    host.pump();
    // The window's dashboard finds the app's MCP control in the context.
    CHECK(page->parts().mcpCard->isVisibleTo(page.get()));
    CHECK(page->parts().mcpCopyToken->isVisibleTo(page.get()));
    CHECK(page->parts().mcpCopyPrompt->isVisibleTo(page.get()));
    page->parts().mcpCopyToken->click();
    CHECK(clipboard == QStringLiteral("test-mcp-access-token"));
}

TEST_CASE("test_a_refused_model_switch_is_shown_on_the_dashboard", "[widget]") {
    HostContext host;
    host.provideAll();
    std::unique_ptr<fv::HostDashboard> page(fv::buildHostDashboard(host.context));
    page->refresh();
    host.pump();
    auto* combo = page->parts().modelCombo;
    REQUIRE(combo->count() == 2);

    host.control->set_job_active(true);
    Q_EMIT combo->activated(1);
    CHECK(page->parts().engineStatus->text() ==
          QStringLiteral("This computer is transcribing right now. Try again in a moment."));
    CHECK(host.control->calls().empty());

    host.control->set_job_active(false);
    Q_EMIT combo->activated(1);
    CHECK(page->parts().engineStatus->text().isEmpty());
    CHECK(page->parts().engineDetail->text() == QStringLiteral("Switching to Orukeet TDT 0.6B…"));
    CHECK(host.control->calls() == std::vector<std::string>{"reload_engine"});
}

TEST_CASE("Host Mode's picker says when this window cannot switch models", "[widget]") {
    HostContext host;
    host.context.provide<fv::remote::RemoteEngineControl>(host.engine);
    std::unique_ptr<fv::HostDashboard> page(fv::buildHostDashboard(host.context));
    page->refresh();
    host.pump();
    Q_EMIT page->parts().modelCombo->activated(1);
    CHECK(page->parts().engineStatus->text() ==
          QStringLiteral("Switching models isn't available in this window."));
}

TEST_CASE("test_host_mode_switches_models_the_way_a_paired_computer_does", "[unit]") {
    fv::MemorySettingsStore settings(Json::object());
    fv::models::FakeEngineControl engine;
    engine.set_selected_backend("parakeet");
    engine.set_loaded_model("parakeet-v3");

    CHECK_FALSE(fv::select_host_model(engine, settings, "parakeet", "orukeet-v0.1").has_value());
    CHECK(settings.get(fv::keys::local_asr_models).at("parakeet") == "orukeet-v0.1");
    CHECK(settings.get(fv::keys::selected_model) == "parakeet");
    CHECK(engine.calls() == std::vector<std::string>{"reload_engine"});

    // The model already served needs no reload.
    CHECK_FALSE(fv::select_host_model(engine, settings, "parakeet", "parakeet-v3").has_value());
    CHECK(engine.calls().size() == 1);

    CHECK(fv::select_host_model(engine, settings, "parakeet", "no-such-model") ==
          "This computer can't run no-such-model.");
    CHECK(fv::select_host_model(engine, settings, "local_whisper", "turbo") ==
          "This computer can't run turbo.");

    engine.set_meeting_active(true);
    CHECK(fv::select_host_model(engine, settings, "parakeet", "parakeet-v3") ==
          "This computer is running a meeting. Change its model after the meeting ends.");
    engine.set_meeting_active(false);
    engine.set_job_active(true);
    CHECK(fv::select_host_model(engine, settings, "parakeet", "parakeet-v3") ==
          "This computer is transcribing right now. Try again in a moment.");
    CHECK(engine.calls().size() == 1);
}
