// Checks the M10 sweep (auditor AU7) added for API & MCP rows of
// docs/port/map/a21-actions-automation-ui.md: the connection card's words, tooltips and names,
// and what the page shows over the running app's session-only listener.

#include <catch2/catch_test_macros.hpp>

#include <QLineEdit>
#include <QPlainTextEdit>

#include "agentapi/setup.h"
#include "base/identity.h"
#include "mcp_support.h"
#include "ui_automation/live_automation_section.h"
#include "ui_automation/mcp_page.h"
#include "ui_automation/mcp_widgets.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using namespace fvtest;

namespace {

const char* const kTags = "[widget]";

// The running app's control (session_mcp.cpp SessionControl): a local session only.
class SessionOnlyControl final : public fv::agentapi::McpServerControl {
public:
    fv::agentapi::ServerStatus status() override { return current; }
    std::string token() override { return current.state == "running" ? kMcpToken : ""; }
    void start(int, bool, std::function<bool()> = {}) override {}
    void stop(bool) override {}
    bool session_only() const override { return true; }

    fv::agentapi::ServerStatus current{"stopped", "MCP is off.", fv::agentapi::DEFAULT_PORT, ""};
};

} // namespace

// A21-51, A21-74, A21-75: the connection card's words, tooltips and accessible names.
TEST_CASE("a21 audit: the MCP connection card names and explains its controls", kTags) {
    McpRig rig;
    auto page = rig.makePage();
    CHECK(page->statusLabel()->accessibleName() == QStringLiteral("MCP server status"));
    CHECK(page->enabled()->accessibleName() == QStringLiteral("Enable MCP"));
    CHECK(page->enabled()->toolTip() ==
          QStringLiteral("Starts with %1 when enabled. Quitting the app stops access.")
              .arg(qs(fv::identity().product)));
    CHECK(page->agentLocation()->accessibleName() == QStringLiteral("Agent computer"));
    CHECK(page->url()->accessibleName() == QStringLiteral("MCP server address"));
    CHECK(page->url()->toolTip() ==
          QStringLiteral("The address appears when this connection is available."));
    CHECK(page->copyUrl()->accessibleName() == QStringLiteral("Copy MCP server address"));
    CHECK(page->setupKind()->accessibleName() == QStringLiteral("Agent setup method"));
    CHECK(page->setupText()->accessibleName() == QStringLiteral("MCP setup instructions"));
    CHECK(page->copySetup()->toolTip() ==
          QStringLiteral("Includes the access token, which grants saved-history access and the "
                         "permissions you choose. Share only with trusted assistants."));
    CHECK(page->token()->accessibleName() == QStringLiteral("MCP access token"));
    CHECK(page->token()->placeholderText() == QStringLiteral("Available when MCP is running"));
    CHECK(page->token()->toolTip() == QStringLiteral("Saved in your system credential store. Copy "
                                                     "it here if an assistant asks for it."));
    CHECK(page->copyToken()->accessibleName() == QStringLiteral("Copy access token"));
    CHECK(page->copyToken()->toolTip() ==
          QStringLiteral("Grants saved-history access and the permissions you choose. Share only "
                         "with trusted assistants."));
    CHECK(page->port()->accessibleName() == QStringLiteral("MCP local port"));
    CHECK(page->port()->minimum() == 1);
    CHECK(page->port()->maximum() == 65535);
    CHECK_FALSE(page->port()->keyboardTracking());
    // Stopped: nothing to copy, and the box says how to get an address.
    CHECK(page->url()->text() == QStringLiteral("Enable MCP to get your connection URL"));
    CHECK(page->setupHint()->text() ==
          QStringLiteral("Connection instructions appear when MCP is running."));
    CHECK_FALSE(page->copySetup()->isEnabled());

    rig.server.current = fv::agentapi::ServerStatus{"running", "", 8767, ""};
    page->refresh();
    const QStringList hints{
        QStringLiteral("Paste the setup prompt into your assistant."),
        QStringLiteral("Paste the command into your terminal."),
        QStringLiteral("Merge this JSON into Cursor's MCP configuration."),
        QStringLiteral("For the desktop app, merge this into ~/.codex/config.toml.")};
    const QStringList copies{
        QStringLiteral("Copy setup prompt"), QStringLiteral("Copy Claude Code command"),
        QStringLiteral("Copy Cursor JSON"), QStringLiteral("Copy ChatGPT config")};
    for (int kind = 0; kind < 4; ++kind) {
        page->setupKind()->buttons()[static_cast<std::size_t>(kind)]->click();
        CHECK(page->setupHint()->text() ==
              hints[kind] + QStringLiteral(" Your access token is already filled in."));
        CHECK(page->copySetup()->text() == copies[kind]);
    }
    CHECK(page->urlLabel()->text() == QStringLiteral("Local MCP address"));
    rig.server.tokenOverride = [] {
        return std::string();
    };
    page->setupKind()->buttons()[0]->click();
    CHECK(page->setupHint()->text().endsWith(
        QStringLiteral(" Copy the access token separately when asked.")));
    page->setupKind()->buttons()[1]->click();
    CHECK(page->setupHint()->text().endsWith(QStringLiteral(" Replace %1 with the access token.")
                                                 .arg(qs(fv::agentapi::TOKEN_PLACEHOLDER))));

    // Another computer while Tailscale access is off (A21-76).
    page->agentLocation()->buttons()[1]->click();
    CHECK(page->urlLabel()->text() == QStringLiteral("Tailscale MCP address"));
    CHECK(page->url()->text() == QStringLiteral("Tailscale access is not enabled"));
    CHECK(page->setupHint()->text() ==
          QStringLiteral("To connect an agent on another computer, turn MCP off, enable Allow "
                         "agents over Tailscale, then turn MCP on again."));
    CHECK_FALSE(page->copySetup()->isEnabled());
}

// A21-50, A21-52, A21-55: over the running app's local session the page leaves out what that
// session does not serve (a20-listener, a20-listener-interface, a19-live-api-not-served).
TEST_CASE("a21 audit: over a local session the page hides Tailscale and the live cards", kTags) {
    fv::MemorySettingsStore store(Json{{"mcp_tailscale_enabled", true}});
    SessionOnlyControl server;
    fv::McpSettingsView view(store, server, fv::LiveServices{});
    view.resize(900, 900);
    view.show();
    pump();
    fv::McpSettingsPage& page = *view.local();
    CHECK(page.enabled()->toolTip() ==
          QStringLiteral("Review access to start a local session. Quitting %1 stops sharing.")
              .arg(qs(fv::identity().product)));
    CHECK_FALSE(page.agentLocation()->isVisible());
    CHECK_FALSE(page.tailscaleTile()->isVisibleTo(&page));
    CHECK_FALSE(view.live()->isVisible());
    CHECK(page.urlLabel()->text() == QStringLiteral("Local MCP address"));
    // The switch shows whether this session runs, not a saved preference.
    CHECK_FALSE(page.enabled()->isChecked());
    server.current = fv::agentapi::ServerStatus{"running", "", 8767, ""};
    page.refresh();
    CHECK(page.enabled()->isChecked());
}
