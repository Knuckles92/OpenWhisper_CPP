#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QPlainTextEdit>
#include <QPushButton>
#include <QWidget>

class QFrame;
class QGridLayout;
class QLayout;
class QLineEdit;

namespace fv {

class McpActionButton;
class McpAdaptiveRow;
class McpDisclosure;
class Executor;
class Paths;
class SettingsStore;
class SettingsSwitch;
class StatusDot;
class WrappedLabel;
struct McpCard;

namespace agentapi {
class McpServerControl;
}

// What Settings > Automate > API & MCP says about the live API (ui_qt/dialogs/settings_mcp.py:
// SSE_LISTENER_LABEL, CHANNEL_LISTENER_LABEL, LIVE_ENDPOINTS and _plural).
namespace live_section {

// The labels the live API gives its event-bus subscriptions: a plain event-stream client, and a
// Claude Code channel session. bus().subscribers(label) counts each.
inline constexpr std::string_view SSE_LISTENER_LABEL = "sse";
inline constexpr std::string_view CHANNEL_LISTENER_LABEL = "claude-channel";

struct Endpoint {
    std::string_view method;
    std::string_view path;
    std::string_view what; // the endpoint's caption
};
// What the live API offers, in the order of the list.
inline constexpr std::array<Endpoint, 7> LIVE_ENDPOINTS = {{
    {"GET", "/v1/events", "Live events as they happen"},
    {"GET", "/v1/live/state", "What's happening right now"},
    {"GET", "/v1/actions", "Your saved actions"},
    {"POST", "/v1/actions/{id}/run", "Run an action with text"},
    {"POST", "/v1/recording/start", "Start dictating; also /stop and /cancel"},
    {"POST", "/v1/filters", "Change the words for a minute; renew or DELETE /v1/filters/{id}"},
    {"POST", "/v1/notify", "Show a short notice"},
}};

// "<count> <noun>", with an "s" unless the count is one.
std::string plural(std::size_t count, std::string_view noun);

} // namespace live_section

// What the section reaches besides the settings and the MCP server. Everything may be left out.
struct LiveServices {
    // Where the Clear button's blocking work runs (the app's io executor). Null runs it at once
    // on the GUI thread, which only a context without blocking work (a test's) does.
    Executor* io = nullptr;
    // McpEvents::subscription_count: in memory, polled every 500 ms. Null counts none.
    std::function<std::size_t()> subscriptionCount;
    // McpEvents::clear_subscriptions: how many were forgotten. It blocks on the credential store.
    // Null forgets none.
    std::function<std::size_t()> clearSubscriptions;
    // Where the Claude Code channel's .mcp.json entry takes its folders from. Null is this
    // process's own (Paths::resolve).
    const Paths* paths = nullptr;
    // The executable the entry starts. Empty is this process's.
    std::filesystem::path executable;
};

// Live API, MCP Events and the Claude Code channel, for this computer only (LiveAutomationSection,
// object name "mcpLiveSection"). They share the MCP listener, so their addresses come from the
// server the page controls. refreshLive reads counters only (in memory) and runs on the page's
// 500 ms timer; saved choices are read again when shown and after a failed write.
// Affinity: GUI thread.
class LiveAutomationSection : public QWidget {
    Q_OBJECT
public:
    // `settings` and `server` outlive the section.
    LiveAutomationSection(SettingsStore& settings, agentapi::McpServerControl& server,
                          LiveServices services = {}, QWidget* parent = nullptr);
    ~LiveAutomationSection() override;

    QFrame* liveApiCard() const { return liveApiCard_; }
    QFrame* mcpEventsCard() const { return mcpEventsCard_; }
    QFrame* channelCard() const { return channelCard_; }
    QGridLayout* columns() const { return columns_; }

    StatusDot* listenersDot() const { return listenersDot_; }
    WrappedLabel* listenersLabel() const { return listenersLabel_; }
    // The five permissions in permissions::ALL's order, by settings key.
    const std::vector<std::pair<std::string, SettingsSwitch*>>& apiPermissionChecks() const {
        return apiChecks_;
    }
    SettingsSwitch* apiPermission(std::string_view key) const;
    QLineEdit* apiUrl() const { return apiUrl_; }
    McpActionButton* copyApiUrl() const { return copyApiUrl_; }
    McpDisclosure* endpoints() const { return endpoints_; }
    const std::vector<QPushButton*>& endpointButtons() const { return endpointButtons_; }
    McpActionButton* copyExample() const { return copyExample_; }

    SettingsSwitch* mcpEventsSwitch() const { return mcpEventsSwitch_; }
    WrappedLabel* subscriptionsLabel() const { return subscriptionsLabel_; }
    McpActionButton* clearSubscriptions() const { return clearSubscriptions_; }

    StatusDot* sessionsDot() const { return sessionsDot_; }
    WrappedLabel* sessionsLabel() const { return sessionsLabel_; }
    WrappedLabel* channelScope() const { return channelScope_; }
    QPlainTextEdit* channelConfig() const { return channelConfig_; }
    McpActionButton* copyChannelConfig() const { return copyChannelConfig_; }
    McpDisclosure* channelPreview() const { return channelPreview_; }
    WrappedLabel* channelCommand() const { return channelCommand_; }
    McpActionButton* copyChannelCommand() const { return copyChannelCommand_; }
    WrappedLabel* channelPrograms() const { return channelPrograms_; }
    WrappedLabel* notice() const { return notice_; }

    // The saved choices, then the live counters.
    void refresh();
    // Counters and the address; cheap enough for the page's 500 ms timer.
    void refreshLive();

Q_SIGNALS:
    // A setting the app caches changed: "mcp_events".
    void settingsChanged(const QString& kind);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    struct Lifeline;

    McpCard card(const QString& title, const QString& description, const QString& icon,
                 const QString& chip = {});
    std::pair<StatusDot*, WrappedLabel*> statusLine(QLayout* layout, const QString& accessibleName);
    McpActionButton* copyButton(const QString& text);
    void buildLiveApi();
    void buildMcpEvents();
    void buildChannel();
    void arrangeColumns();

    QString baseUrl() const;
    void showCount(StatusDot* dot, WrappedLabel* label, bool running, std::size_t count,
                   const QString& text, const QString& offText);
    void write(std::string_view key, bool value);
    void save(const std::string& key, bool checked);
    void saveMcpEvents(bool checked);
    void clearAllSubscriptions();
    void onAnswer(int token, std::size_t cleared, const std::string& error);
    void showNotice(const QString& text);
    void copy(const QString& text, const QString& done);
    void copyEndpoint(std::string_view path);
    void copyExampleCommand();

    SettingsStore& settings_;
    agentapi::McpServerControl& server_;
    LiveServices services_;
    std::shared_ptr<Lifeline> lifeline_;
    std::optional<bool> wide_;
    std::map<int, std::string> waiting_;
    int tokens_ = 0;

    QGridLayout* columns_ = nullptr;
    QFrame* liveApiCard_ = nullptr;
    QFrame* mcpEventsCard_ = nullptr;
    QFrame* channelCard_ = nullptr;
    StatusDot* listenersDot_ = nullptr;
    WrappedLabel* listenersLabel_ = nullptr;
    std::vector<std::pair<std::string, SettingsSwitch*>> apiChecks_;
    QLineEdit* apiUrl_ = nullptr;
    McpActionButton* copyApiUrl_ = nullptr;
    McpDisclosure* endpoints_ = nullptr;
    std::vector<QPushButton*> endpointButtons_;
    McpActionButton* copyExample_ = nullptr;
    SettingsSwitch* mcpEventsSwitch_ = nullptr;
    WrappedLabel* subscriptionsLabel_ = nullptr;
    McpActionButton* clearSubscriptions_ = nullptr;
    StatusDot* sessionsDot_ = nullptr;
    WrappedLabel* sessionsLabel_ = nullptr;
    WrappedLabel* channelScope_ = nullptr;
    QPlainTextEdit* channelConfig_ = nullptr;
    McpActionButton* copyChannelConfig_ = nullptr;
    McpDisclosure* channelPreview_ = nullptr;
    WrappedLabel* channelCommand_ = nullptr;
    McpActionButton* copyChannelCommand_ = nullptr;
    WrappedLabel* channelPrograms_ = nullptr;
    WrappedLabel* notice_ = nullptr;
};

} // namespace fv
