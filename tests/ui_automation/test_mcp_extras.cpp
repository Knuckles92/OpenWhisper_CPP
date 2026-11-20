// What the API & MCP page does that OpenWhisper's tests leave out: the page's registration, the
// notice that stays until the next write, the controls that wait for a stopped listener, a failed
// port save, the timer that runs while the page shows, and the work of Clear without an executor.
// Each is read off settings_mcp.py.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QTimer>

#include "agentapi/mcp_runtime.h"
#include "appkit/destinations.h"
#include "automation_support.h"
#include "mcp_support.h"
#include "ui_automation/mcp_widgets.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using fv::agentapi::ServerStatus;
using namespace fvtest;

namespace {

const char* const kExtra = "[test_mcp_page_extra][widget]";

bool has(const QString& haystack, const char* needle) {
    return haystack.contains(QString::fromUtf8(needle));
}

} // namespace

TEST_CASE("the page says what the Settings window lists it as", "[test_mcp_page_extra][unit]") {
    CHECK(fv::mcp_page::TITLE == "API & MCP");
    CHECK(fv::mcp_page::SUBTITLE ==
          "Let agents and your own programs read history, follow live events and run actions.");
    CHECK(fv::mcp_page::control_names() == std::vector<std::string>{"mcp_page", "mcp_view"});
    // Twelve bullets, as many code points as the preview masks the token with.
    CHECK(QString::fromUtf8(fv::mcp_page::HIDDEN_TOKEN.data(),
                            static_cast<qsizetype>(fv::mcp_page::HIDDEN_TOKEN.size()))
              .size() == 12);
}

TEST_CASE("the status line escapes what the listener says as Python's html.escape does",
          "[test_mcp_page_extra][unit]") {
    using fv::mcp_page::html_escape;
    CHECK(html_escape("plain") == "plain");
    CHECK(html_escape("<b>\"Port\" & 'host'</b>") ==
          "&lt;b&gt;&quot;Port&quot; &amp; &#x27;host&#x27;&lt;/b&gt;");
}

TEST_CASE("the LIVE_ENDPOINTS captions are the ones the area map lists",
          "[test_mcp_page_extra][unit]") {
    // docs/port/map/a21-actions-automation-ui.md, "Live API endpoint table".
    const std::vector<std::vector<std::string>> map{
        {"GET", "/v1/events", "Live events as they happen"},
        {"GET", "/v1/live/state", "What's happening right now"},
        {"GET", "/v1/actions", "Your saved actions"},
        {"POST", "/v1/actions/{id}/run", "Run an action with text"},
        {"POST", "/v1/recording/start", "Start dictating; also /stop and /cancel"},
        {"POST", "/v1/filters", "Change the words for a minute; renew or DELETE /v1/filters/{id}"},
        {"POST", "/v1/notify", "Show a short notice"},
    };
    REQUIRE(fv::live_section::LIVE_ENDPOINTS.size() == map.size());
    for (std::size_t i = 0; i < map.size(); ++i) {
        CHECK(std::string(fv::live_section::LIVE_ENDPOINTS[i].method) == map[i][0]);
        CHECK(std::string(fv::live_section::LIVE_ENDPOINTS[i].path) == map[i][1]);
        CHECK(std::string(fv::live_section::LIVE_ENDPOINTS[i].what) == map[i][2]);
    }
    CHECK(fv::live_section::plural(1, "program") == "1 program");
    CHECK(fv::live_section::plural(0, "program") == "0 programs");
    CHECK(fv::live_section::plural(2, "session") == "2 sessions");
}

TEST_CASE("the Settings window builds the page lazily and finds its controls", kExtra) {
    ActionsRig rig;
    ScriptedMcpControl server;
    ScopedBus bus;
    rig.context().provide<fv::agentapi::McpServerControl>(
        std::shared_ptr<fv::agentapi::McpServerControl>(&server, [](auto*) {}));
    fv::SettingsDialog& dialog = rig.dialog(false);
    CHECK_FALSE(dialog.isBuilt(dest::mcp));
    QWidget* page = dialog.control("mcp_page");
    REQUIRE(page != nullptr);
    CHECK(dialog.isBuilt(dest::mcp));
    dialog.selectDestination(qs(dest::mcp));
    CHECK(dialog.pageTitleLabel()->text() == qs(fv::mcp_page::TITLE));
    auto* view = dynamic_cast<fv::McpSettingsView*>(dialog.pageContent(dest::mcp));
    REQUIRE(view != nullptr);
    CHECK(dialog.control("mcp_view") == view);
    CHECK(view->namedControl("mcp_page") == view->local());
    CHECK(view->namedControl("no_such_control") == nullptr);
    CHECK(page == view->local());
    for (const std::string& name : fv::mcp_page::control_names()) {
        CHECK(rig.context().settingsPages().controlDestination(name) == dest::mcp);
    }
    view->bindRemote(nullptr);
    view->refreshPage();
}

TEST_CASE("a context without an MCP listener shows MCP as unavailable", kExtra) {
    ActionsRig rig;
    ScopedBus bus;
    fv::SettingsDialog& dialog = rig.dialog(false);
    auto* page = dynamic_cast<fv::McpSettingsPage*>(dialog.control("mcp_page"));
    REQUIRE(page != nullptr);
    CHECK(has(page->statusLabel()->text(), "MCP is not available in this session."));
    CHECK(page->enableTile()->property("state").toString() == "error");
    CHECK_FALSE(page->connection()->isEnabled());
}

TEST_CASE("the notice stays through every refresh until the next write", kExtra) {
    McpRig rig;
    auto page = rig.makePage();
    rig.server.current = ServerStatus{"running", "Ready", 8767, ""};
    page->refresh();
    CHECK_FALSE(page->notice()->isVisible());
    page->copyToken()->click();
    CHECK(text(page->notice()->text()) == "Copied to clipboard.");
    page->refresh();
    page->refresh();
    CHECK(text(page->notice()->text()) == "Copied to clipboard.");
    // A successful write clears it; a failed one replaces it.
    page->permissionCheck("mcp_retitle_meetings")->setChecked(true);
    CHECK(page->notice()->text().isEmpty());
    rig.store.file().fail_next_replace("read only");
    page->tailscaleEnabled()->setChecked(true);
    CHECK(text(page->notice()->text()) == "Could not save Tailscale access. Try again.");
    CHECK_FALSE(page->tailscaleEnabled()->isChecked());
    page->refresh();
    CHECK(text(page->notice()->text()) == "Could not save Tailscale access. Try again.");
}

TEST_CASE("a copy is refused unless the listener runs and there is something to copy", kExtra) {
    McpRig rig;
    auto page = rig.makePage();
    page->copyText(QStringLiteral("text"));
    CHECK(rig.clipboard.copied.isEmpty());
    rig.server.current = ServerStatus{"running", "Ready", 8767, ""};
    page->copyText(QString());
    CHECK(rig.clipboard.copied.isEmpty());
    page->copyText(QStringLiteral("text"));
    CHECK(rig.clipboard.copied == QStringList{QStringLiteral("text")});
}

TEST_CASE("the port and Tailscale wait for a stopped listener and the switch off", kExtra) {
    McpRig rig;
    auto page = rig.makePage();
    for (const char* state : {"stopped", "error"}) {
        rig.server.current = ServerStatus{state, "x", 8767, ""};
        page->refresh();
        CHECK(page->port()->isEnabled());
        CHECK(page->tailscaleTile()->isEnabled());
    }
    for (const char* state : {"starting", "running", "stopping"}) {
        rig.server.current = ServerStatus{state, "x", 8767, ""};
        page->refresh();
        INFO(state);
        CHECK_FALSE(page->port()->isEnabled());
        CHECK_FALSE(page->tailscaleTile()->isEnabled());
    }
    // The switch saved on holds them too, whatever the listener says.
    rig.server.current = ServerStatus{"error", "x", 8767, ""};
    page->enabled()->setChecked(true);
    CHECK_FALSE(page->port()->isEnabled());
    CHECK_FALSE(page->tailscaleTile()->isEnabled());
}

TEST_CASE("Retry shows only for an error while the switch is saved on", kExtra) {
    McpRig rig;
    auto page = rig.makePage();
    rig.server.current = ServerStatus{"error", "x", 8767, ""};
    page->refresh();
    CHECK(page->retry()->isHidden());
    rig.store.save_setting("mcp_enabled", true);
    page->refresh();
    CHECK_FALSE(page->retry()->isHidden());
    rig.server.current = ServerStatus{"running", "x", 8767, ""};
    page->refresh();
    CHECK(page->retry()->isHidden());
}

TEST_CASE("starting passes the port and Tailscale access the page shows", kExtra) {
    McpRig rig({{"mcp_port", 9001}, {"mcp_tailscale_enabled", true}});
    auto page = rig.makePage();
    CHECK(page->port()->value() == 9001);
    CHECK(page->tailscaleEnabled()->isChecked());
    CHECK(page->agentLocation()->currentIndex() == 1);
    page->enabled()->setChecked(true);
    CHECK(rig.server.starts == std::vector<int>{9001});
    CHECK(rig.server.tailscaleStarts == std::vector<bool>{true});
    page->enabled()->setChecked(false);
    CHECK(rig.server.current.state == "stopped");
}

TEST_CASE("a failed port save shows the saved port again and says so", kExtra) {
    McpRig rig({{"mcp_port", 9001}});
    auto page = rig.makePage();
    rig.store.file().fail_next_replace("read only");
    page->port()->setValue(9100);
    CHECK(page->port()->value() == 9001);
    CHECK(text(page->notice()->text()) == "Could not save the MCP port. Try again.");
    page->port()->setValue(9100);
    CHECK(rig.saved("mcp_port") == Json(9100));
    CHECK(page->notice()->text().isEmpty());
}

TEST_CASE("a failed save of the switch leaves the listener alone", kExtra) {
    McpRig rig;
    auto page = rig.makePage();
    rig.server.current = ServerStatus{"running", "x", 8767, ""};
    rig.store.save_setting("mcp_enabled", true);
    page->refresh();
    CHECK(page->enabled()->isChecked());
    rig.store.file().fail_next_replace("read only");
    page->enabled()->setChecked(false);
    CHECK(rig.server.current.state == "running");
    CHECK(page->enabled()->isChecked());
    CHECK(text(page->notice()->text()) == "Could not save the MCP setting. Try again.");
}

TEST_CASE("the page refreshes every 500 ms while it is shown and not otherwise", kExtra) {
    McpRig rig;
    auto page = rig.makePage();
    CHECK(page->timer()->interval() == 500);
    CHECK_FALSE(page->timer()->isActive());
    page->show();
    CHECK(page->timer()->isActive());
    // The listener changes under the page; the next tick shows it.
    rig.server.current = ServerStatus{"running", "x", 8767, ""};
    QMetaObject::invokeMethod(page->timer(), "timeout");
    CHECK(has(page->statusLabel()->text(), "Running"));
    page->hide();
    CHECK_FALSE(page->timer()->isActive());
}

TEST_CASE("the columns arrange again only when the width crosses 740 px", kExtra) {
    AppTheme theme;
    McpRig rig;
    auto page = rig.makePage();
    auto* grid = page->columns();
    page->resize(900, 900);
    page->show();
    pump();
    CHECK(grid->itemAtPosition(0, 1)->widget() == page->permissionsCard());
    CHECK(grid->columnStretch(0) == 6);
    CHECK(grid->columnStretch(1) == 5);
    page->resize(700, 900);
    pump();
    CHECK(grid->itemAtPosition(1, 0)->widget() == page->permissionsCard());
    CHECK(grid->columnStretch(0) == 1);
    CHECK(grid->columnStretch(1) == 0);
    page->resize(900, 900);
    pump();
    CHECK(grid->itemAtPosition(0, 1)->widget() == page->permissionsCard());
    theme.setScale(130);
    page->resize(900, 900);
    pump();
    // 740 x 1.3 = 962: 900 px is narrow at that scale.
    CHECK(grid->itemAtPosition(1, 0)->widget() == page->permissionsCard());
}

TEST_CASE("Clear runs on the executor it is given and answers on the GUI thread", kExtra) {
    // Declared before the rig: the rig's pool is joined first.
    std::atomic<bool> offGuiThread{false};
    const auto gui = std::this_thread::get_id();
    McpRig rig;
    rig.events.count = 2;
    rig.events.onClear = [&]() -> std::size_t {
        offGuiThread = std::this_thread::get_id() != gui;
        return 2;
    };
    auto section = rig.makeSection();
    section->refreshLive();
    section->clearSubscriptions()->click();
    REQUIRE(waitFor([&] { return text(section->notice()->text()) == "Cleared 2 subscriptions."; }));
    CHECK(offGuiThread);
}

TEST_CASE("Clear with nothing to clear says so, and without an executor it runs at once", kExtra) {
    McpRig rig;
    rig.events.count = 1;
    rig.events.onClear = []() -> std::size_t {
        return 0;
    };
    fv::LiveServices services = rig.events.services(nullptr);
    fv::LiveAutomationSection section(rig.store, rig.server, services);
    section.refreshLive();
    section.clearSubscriptions()->click();
    REQUIRE(waitFor([&] { return text(section.notice()->text()) == "Nothing to clear."; }));
}

TEST_CASE("a section deleted while Clear works drops the answer", kExtra) {
    // Declared before the rig: the rig's pool is joined first.
    std::atomic<bool> release{false};
    std::atomic<bool> started{false};
    McpRig rig;
    rig.events.count = 1;
    rig.events.onClear = [&]() -> std::size_t {
        started = true;
        while (!release) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return 1;
    };
    {
        auto section = rig.makeSection();
        section->refreshLive();
        section->clearSubscriptions()->click();
        REQUIRE(waitFor([&] { return started.load(); }));
    }
    release = true;
    // The worker finishes and finds nothing to answer.
    pump(20);
    SUCCEED();
}

TEST_CASE("the live section counts what the bus counts and nothing else", kExtra) {
    McpRig rig;
    auto section = rig.makeSection();
    rig.server.current = ServerStatus{"running", "x", 8767, ""};
    auto other = rig.bus.listen("webhooks");
    auto mcp = rig.bus.listen("mcp-events");
    section->refreshLive();
    CHECK(text(section->listenersLabel()->text()) == "No programs listening yet");
    CHECK(text(section->sessionsLabel()->text()) == "No sessions connected");
    // MCP going off puts both lines back to their off wording.
    rig.server.current = ServerStatus{"stopped", "x", 8767, ""};
    section->refreshLive();
    CHECK(text(section->listenersLabel()->text()) == "Off while MCP is off");
    CHECK(text(section->sessionsLabel()->text()) == "Needs MCP on");
}

TEST_CASE("the endpoint buttons name their address and copy it whole", kExtra) {
    McpRig rig;
    auto section = rig.makeSection();
    rig.server.current = ServerStatus{"running", "x", 8123, ""};
    section->refreshLive();
    const auto& buttons = section->endpointButtons();
    REQUIRE(buttons.size() == fv::live_section::LIVE_ENDPOINTS.size());
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        const auto& endpoint = fv::live_section::LIVE_ENDPOINTS[i];
        CHECK(text(buttons[i]->text()) ==
              std::string(endpoint.method) + "  " + std::string(endpoint.path));
        CHECK(text(buttons[i]->accessibleName()) ==
              "Copy the " + std::string(endpoint.path) + " address");
        buttons[i]->click();
        CHECK(text(rig.clipboard.last()) == "http://127.0.0.1:8123" + std::string(endpoint.path));
        CHECK(text(section->notice()->text()) ==
              "Copied http://127.0.0.1:8123" + std::string(endpoint.path));
    }
}
