#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QFrame>
#include <QPointer>
#include <QWidget>

#include "agentapi/mcp_runtime.h"
#include "appkit/settings_page.h"
#include "base/subscription.h"
#include "ui_automation/live_automation_section.h"

class QCheckBox;
class QGridLayout;
class QLabel;
class QPushButton;
class QLineEdit;
class QPlainTextEdit;
class QTimer;

namespace fv {

class McpActionButton;
class McpAdaptiveRow;
class AppContext;
class McpDisclosure;
class NoWheelSpinBox;
class SegmentedBar;
class SettingTile;
class SettingsPageHost;
class SettingsStore;
class SettingsSwitch;
class StatusDot;
class WrappedLabel;
class Executor;
namespace remote {
class RemoteEngineControl;
}
namespace qt {
class QtUiExecutor;
}

// What Settings > Automate > API & MCP says about itself (ui_qt/dialogs/settings_mcp.py and the
// page's registration in settings_dialog.py).
namespace mcp_page {

inline constexpr std::string_view TITLE = "API & MCP";
inline constexpr std::string_view SUBTITLE =
    "Let agents and your own programs read history, follow live events and run actions.";

// The access token's stand-in in the on-screen preview (_HIDDEN_TOKEN): twelve bullets. Copies
// are built at click time with the real token.
inline constexpr std::string_view HIDDEN_TOKEN = "••••••••••••";

// CONTROL_DESTINATIONS' names for this page: mcp_page, then mcp_view.
const std::vector<std::string>& control_names();

// html.escape(text): & < > " and ' made safe for the status line's rich text.
std::string html_escape(std::string_view text);

} // namespace mcp_page

// The MCP controls for this computer (McpSettingsPage, object name "mcpSettingsPage"): the server
// bar with its switch, the Connection card (where the assistant is, the address, the setup
// prompt, command or configuration, the access token, the port and Tailscale access), the
// Permissions card, and a notice under them. It reads what the listener says, not what is saved:
// a 500 ms timer refreshes it while it is shown. Nothing here saves through a Settings window; a
// failed write puts its notice under the cards and shows the saved value again.
//
// `server` is the in-app MCP listener (agentapi::McpServerControl); the token is read from it
// when a copy is made, never kept, and the preview shows twelve bullets in its place.
// Affinity: GUI thread.
class McpSettingsPage : public QWidget {
    Q_OBJECT
public:
    // `settings` and `server` outlive the page.
    McpSettingsPage(SettingsStore& settings, agentapi::McpServerControl& server,
                    QWidget* parent = nullptr);
    ~McpSettingsPage() override;

    QFrame* enableTile() const { return enableTile_; }
    StatusDot* statusDot() const { return statusDot_; }
    WrappedLabel* statusLabel() const { return statusLabel_; }
    McpActionButton* retry() const { return retry_; }
    SettingsSwitch* enabled() const { return enabled_; }

    QGridLayout* columns() const { return columns_; }
    QFrame* connectionCard() const { return connectionCard_; }
    QFrame* permissionsCard() const { return permissionsCard_; }
    QWidget* connection() const { return connection_; }
    SegmentedBar* agentLocation() const { return agentLocation_; }
    WrappedLabel* urlLabel() const { return urlLabel_; }
    QLineEdit* url() const { return url_; }
    McpActionButton* copyUrl() const { return copyUrl_; }
    SegmentedBar* setupKind() const { return setupKind_; }
    WrappedLabel* setupHint() const { return setupHint_; }
    McpActionButton* copySetup() const { return copySetup_; }
    McpDisclosure* preview() const { return preview_; }
    McpAdaptiveRow* copyActions() const { return copyActions_; }
    QPlainTextEdit* setupText() const { return setupText_; }
    QLineEdit* token() const { return token_; }
    McpActionButton* copyToken() const { return copyToken_; }

    McpDisclosure* advanced() const { return advanced_; }
    NoWheelSpinBox* port() const { return port_; }
    SettingTile* tailscaleTile() const { return tailscaleTile_; }
    SettingsSwitch* tailscaleEnabled() const;

    // The three baseline-plus-optional switches, by settings key (mcp_retitle_transcriptions,
    // mcp_retitle_meetings, mcp_settings_access), in page order.
    const std::vector<std::pair<std::string, SettingsSwitch*>>& permissionChecks() const {
        return permissionChecks_;
    }
    SettingsSwitch* permissionCheck(std::string_view key) const;
    QLabel* permissionSummary() const { return permissionSummary_; }
    McpDisclosure* preferences() const { return preferences_; }
    QWidget* settingsPermissions() const { return settingsPermissions_; }
    // One checkbox per SETTING_CONTROLS entry, by key, in the controls' order.
    const std::vector<std::pair<std::string, QCheckBox*>>& settingChecks() const {
        return settingChecks_;
    }
    QCheckBox* settingCheck(std::string_view key) const;
    // The groups' disclosures, in the order their first control appears.
    const std::vector<std::pair<std::string, McpDisclosure*>>& settingGroups() const {
        return settingGroups_;
    }

    WrappedLabel* notice() const { return notice_; }
    QTimer* timer() const { return timer_; }
    agentapi::McpServerControl& server() const { return server_; }

    // _copy: puts text on the clipboard, unless the server is not running or the text is empty.
    void copyText(const QString& text);

public Q_SLOTS:
    // Reads the listener and the saved settings again and shows them (every 500 ms while shown).
    void refresh();

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    McpActionButton* copyRow(QLayout* layout, QWidget* field, const QString& buttonText,
                             std::function<QString()> value);
    void arrangeColumns();
    void toggle(bool checked);
    void start();
    void saveTailscale(bool checked);
    void savePermission(const std::string& key, bool checked);
    void saveSettingPermission(const std::string& key, bool checked);
    void portChanged(int value);
    QString setupUrl(const agentapi::ServerStatus& status) const;
    QString setupFor(const QString& url, const std::string& token) const;
    void copySetupText();
    void renderSetup();

    SettingsStore& settings_;
    agentapi::McpServerControl& server_;
    // Index of the chooser option that uses the Tailscale address, and the other one.
    int remoteIndex_ = 1;
    int localIndex_ = 0;
    QString noticeText_;
    std::optional<bool> wide_;

    QFrame* enableTile_ = nullptr;
    StatusDot* statusDot_ = nullptr;
    WrappedLabel* statusLabel_ = nullptr;
    McpActionButton* retry_ = nullptr;
    SettingsSwitch* enabled_ = nullptr;
    QGridLayout* columns_ = nullptr;
    QFrame* connectionCard_ = nullptr;
    QFrame* permissionsCard_ = nullptr;
    QWidget* connection_ = nullptr;
    SegmentedBar* agentLocation_ = nullptr;
    WrappedLabel* urlLabel_ = nullptr;
    QLineEdit* url_ = nullptr;
    McpActionButton* copyUrl_ = nullptr;
    SegmentedBar* setupKind_ = nullptr;
    WrappedLabel* setupHint_ = nullptr;
    McpActionButton* copySetup_ = nullptr;
    McpDisclosure* preview_ = nullptr;
    McpAdaptiveRow* copyActions_ = nullptr;
    QPlainTextEdit* setupText_ = nullptr;
    QLineEdit* token_ = nullptr;
    McpActionButton* copyToken_ = nullptr;
    McpDisclosure* advanced_ = nullptr;
    NoWheelSpinBox* port_ = nullptr;
    SettingTile* tailscaleTile_ = nullptr;
    std::vector<std::pair<std::string, SettingsSwitch*>> permissionChecks_;
    QLabel* permissionSummary_ = nullptr;
    McpDisclosure* preferences_ = nullptr;
    QWidget* settingsPermissions_ = nullptr;
    std::vector<std::pair<std::string, QCheckBox*>> settingChecks_;
    std::vector<std::pair<std::string, McpDisclosure*>> settingGroups_;
    WrappedLabel* notice_ = nullptr;
    QTimer* timer_ = nullptr;
};

// Settings > Automate > API & MCP: this computer's MCP page and, under it, the live API, MCP
// Events and the Claude Code channel (McpSettingsView, object name "mcpSettingsView"). The
// counters of the live section refresh on the local page's timer.
//
// bindRemote adds the paired host's frame ("mcpPairedHost") below them, in place of
// OpenWhisper's This computer / host tabs, host page and gate: a host only grants one paired
// computer a read-only loopback session for 15 minutes (differences a17-paired-mcp), so the frame
// can check that session's state, ask the host to start it and end it, and never changes or
// shows the host's settings or token. It shows only while this computer is paired, naming the
// host. History is shared from Remote engine's "Share history for this session…"
// (a17-direct-history), so there is no history tile here. Affinity: GUI thread.
class McpSettingsView : public QWidget, public SettingsPageModule {
    Q_OBJECT
public:
    McpSettingsView(SettingsStore& settings, agentapi::McpServerControl& server,
                    LiveServices services = {}, QWidget* parent = nullptr);

    McpSettingsPage* local() const { return local_; }
    LiveAutomationSection* live() const { return live_; }

    // Attaches the paired-host frame once; nothing without a service and an io executor, where
    // every call to the service runs.
    void bindRemote(remote::RemoteEngineControl*, Executor* io = nullptr);
    // The frame and its parts; null until bindRemote attached them.
    QWidget* pairedHost() const { return pairedHost_; }
    QLabel* pairedHostTitle() const { return hostTitle_; }
    QPushButton* checkHostButton() const { return checkHost_; }
    QPushButton* startHostButton() const { return startHost_; }
    QPushButton* endHostButton() const { return endHost_; }
    QLabel* hostStatus() const { return hostStatus_; }

    // SettingsPageModule: both parts read their state again.
    void refreshPage() override;
    QWidget* namedControl(std::string_view name) override;

Q_SIGNALS:
    // A setting the app caches changed (SettingsDialog::notifyChanged's kinds): "mcp_events".
    void settingsChanged(const QString& kind);

private:
    // Reads the pairing off the GUI thread, then shows or hides the frame.
    void readPairing();
    void setHostBusy(bool busy);

    McpSettingsPage* local_;
    QWidget* pairedHost_ = nullptr;
    QLabel* hostTitle_ = nullptr;
    QPushButton* checkHost_ = nullptr;
    QPushButton* startHost_ = nullptr;
    QPushButton* endHost_ = nullptr;
    QLabel* hostStatus_ = nullptr;
    remote::RemoteEngineControl* remote_ = nullptr;
    Executor* remoteIo_ = nullptr;
    std::shared_ptr<qt::QtUiExecutor> remoteUi_;
    Subscription remoteEvents_;
    LiveAutomationSection* live_;
};

// Registers the page for destinations::mcp in the context's Settings pages, with its controls.
// It is built on first selection over the context's settings store, its McpServerControl (a
// stand-in that says MCP is not available while none is provided), and its McpEvents and io
// executor when it has them, with a DialogPageHost of the Settings window it is built in;
// building it without one throws std::logic_error.
// Affinity: UI actor.
void registerMcpPage(AppContext& context);

} // namespace fv
