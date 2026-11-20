#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <QFrame>
#include <QString>
#include <QWidget>

#include "base/executor.h"
#include "base/json.h"
#include "base/signal.h"
#include "base/subscription.h"
#include "remote/control.h"
#include "ui_remote/host_text.h"

class QBoxLayout;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;
class QVariantAnimation;

namespace fv {

class Button;
class Clock;
class ElidingLabel;
class HostBeacon;
class NoWheelComboBox;
class RemoteLinkGlyph;
class SettingsStore;
class StatusDot;
class WrappedLabel;

namespace agentapi {
class McpServerControl;
}
namespace remote {
class ClientHistorySource;
}

namespace host_dashboard {

// A number with what it counts, and a line under it (_StatTile).
class StatTile final : public QFrame {
    Q_OBJECT
public:
    StatTile(const QString& caption, const QString& tone, QWidget* parent = nullptr);
    void set(const QString& value, const QString& detail = {});
    QLabel* caption = nullptr;
    QLabel* value = nullptr;
    ElidingLabel* detail = nullptr;
};

// A titled section of the page (_Card).
class Card final : public QFrame {
    Q_OBJECT
public:
    Card(const QString& title, QWidget* parent = nullptr);
    void setBadge(const QString& text);
    QLabel* title = nullptr;
    QLabel* badge = nullptr;
    QVBoxLayout* body = nullptr;
};

// One connected computer: its link, name, address and what it asked for (_ClientRow).
class ClientRow final : public QFrame {
    Q_OBJECT
public:
    explicit ClientRow(QWidget* parent = nullptr);
    void showClient(const host_text::GroupedClient& client, const Json& stats, double now,
                    const std::chrono::time_zone& zone);
    // The serving chip: this computer on the left, the paired computer on the right; a packet
    // runs home with each transcription it served.
    RemoteLinkGlyph* glyph = nullptr;
    ElidingLabel* name = nullptr;
    ElidingLabel* address = nullptr;
    ElidingLabel* stats = nullptr;
    QLabel* state = nullptr;
};

// One Recent activity line; a new one glows as it arrives (_EventRow), 1,600 ms.
class EventRow final : public QFrame {
    Q_OBJECT
public:
    EventRow(const Json& event, bool fresh, double now, const std::chrono::time_zone& zone,
             QWidget* parent = nullptr);
    double glow() const { return glow_; }
    long long serial() const { return serial_; }
    WrappedLabel* text = nullptr;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    long long serial_ = 0;
    double glow_ = 0.0;
};

// A paired computer's row: the frame, its status label ("Connected", "Seen 3 h ago") and the
// line under its name.
struct DeviceRow {
    QFrame* row = nullptr;
    QLabel* status = nullptr;
    ElidingLabel* detail = nullptr;
    remote::PairedDevice device;
};

} // namespace host_dashboard

// View → Host Mode: what someone running the app only as a host needs to see
// (ui_qt/widgets/host_dashboard.py). It reads RemoteEngineControl off the GUI thread on `io`
// and hears its events the way Settings → Remote engine does: they arrive from any thread, are
// posted to `ui`, then coalesced for 40 ms, so a live preview's stream of requests redraws the
// page once per burst. Nothing is read until the page is shown (host_state() asks Tailscale), and
// every timer stops while it is hidden. Motion answers real events only: the beacon's waves run
// while a paired computer's audio is being decoded, it ripples once when a transcription comes
// back, and a new line in Recent activity glows as it arrives.
// Affinity: GUI thread.
class HostDashboard final : public QWidget {
    Q_OBJECT
public:
    // The page's widgets, for the window and for tests (the Python test reads the same ones).
    struct Parts {
        QScrollArea* scroll = nullptr;
        QFrame* hero = nullptr;
        HostBeacon* beacon = nullptr;
        WrappedLabel* heroTitle = nullptr;
        WrappedLabel* heroDetail = nullptr;
        WrappedLabel* heroIdentity = nullptr;
        Button* startButton = nullptr;
        Button* stopButton = nullptr;
        QWidget* stats = nullptr;
        host_dashboard::StatTile* statConnected = nullptr;
        host_dashboard::StatTile* statTranscriptions = nullptr;
        host_dashboard::StatTile* statAudio = nullptr;
        host_dashboard::StatTile* statSpeed = nullptr;
        host_dashboard::Card* engineCard = nullptr;
        StatusDot* engineDot = nullptr;
        QLabel* engineName = nullptr;
        WrappedLabel* engineDetail = nullptr;
        WrappedLabel* engineStatus = nullptr;
        NoWheelComboBox* modelCombo = nullptr;
        WrappedLabel* engineHint = nullptr;
        QPushButton* modelsLink = nullptr;
        host_dashboard::Card* clientsCard = nullptr;
        QVBoxLayout* clientsLayout = nullptr;
        WrappedLabel* clientsEmpty = nullptr;
        host_dashboard::Card* devicesCard = nullptr;
        QFrame* pairingBox = nullptr;
        QLabel* pairingCode = nullptr;
        WrappedLabel* pairingExpiry = nullptr;
        Button* cancelPairingButton = nullptr;
        Button* pairButton = nullptr;
        WrappedLabel* pairNote = nullptr;
        QVBoxLayout* devicesLayout = nullptr;
        QPushButton* manageLink = nullptr;
        host_dashboard::Card* mcpCard = nullptr;
        WrappedLabel* mcpIntro = nullptr;
        StatusDot* mcpDot = nullptr;
        WrappedLabel* mcpStatus = nullptr;
        Button* mcpButton = nullptr;
        WrappedLabel* mcpDetail = nullptr;
        QBoxLayout* mcpActions = nullptr;
        QPushButton* mcpLink = nullptr;
        Button* mcpCopyToken = nullptr;
        Button* mcpCopyPrompt = nullptr;
        host_dashboard::Card* activityCard = nullptr;
        QVBoxLayout* activityLayout = nullptr;
        WrappedLabel* activityEmpty = nullptr;
    };

    // `service` may be null (the unbound page); bind() attaches one later.
    HostDashboard(remote::RemoteEngineControl* service, Executor& io, Executor& ui,
                  QWidget* parent = nullptr);
    ~HostDashboard() override;

    const Parts& parts() const { return parts_; }
    const std::map<std::string, host_dashboard::ClientRow*>& clientRows() const {
        return clientRows_;
    }
    const std::map<std::string, host_dashboard::DeviceRow>& deviceRows() const {
        return deviceRows_;
    }
    // The (label, family, model) of a switch asked for here, until it lands.
    const std::optional<std::tuple<QString, QString, QString>>& switching() const {
        return switching_;
    }

    // Attaches the controller's service (created after the window); reads when shown.
    void bind(remote::RemoteEngineControl* service);
    // Shows the MCP server's card; null hides it (no MCP control in this build or window).
    void bindMcp(agentapi::McpServerControl* server);
    // Where bindMcp finds the app's MCP server the first time the page reads everything, when
    // nothing was bound before.
    void setMcpLookup(std::function<agentapi::McpServerControl*()> lookup);
    // The settings Turn on and Turn off save mcp_enabled to and read the MCP port from.
    void setSettings(SettingsStore* settings);
    // utc() dates relative times, zone() shows them, steady() runs the pairing countdown.
    void setClock(const Clock& clock);
    // Writes the copy buttons' text; QGuiApplication's clipboard by default.
    void setClipboardWriter(std::function<void(const QString&)> write);
    // Asks before stopping while someone is connected: (text, informative) → stop.
    void setStopConfirmation(std::function<bool(const QString&, const QString&)> confirm);
    void setClientHistorySource(remote::ClientHistorySource& source);

    // Reads everything again now, shown or not.
    void refresh();
    // Draws what is waiting now, shown or not (_flush(force=True)).
    void flush();
    // This computer's engine started or finished loading.
    void setEngineBusy(bool busy);
    // Follows the controller's engine_busy_changed (delivered on `ui`) with setEngineBusy.
    void followEngineBusy(Signal<bool>& busy);
    // The engine readout changed (a load finished, or failed).
    void setDeviceInfo(const QString& deviceInfo, bool ready);
    // A switch asked for here couldn't start: says why under the picker.
    void showEngineError(const QString& message);

Q_SIGNALS:
    // Serve another of this computer's ready models.
    void modelRequested(const QString& family, const QString& model);
    // Open Settings on a destination key.
    void settingsRequested(const QString& destination);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct Read;

    void build();
    void buildHero(QVBoxLayout& page);
    void buildStats(QVBoxLayout& page);
    void buildEngine(QVBoxLayout& page);
    void buildClients(QVBoxLayout& page);
    void buildDevices(QVBoxLayout& page);
    void buildMcp(QVBoxLayout& page);
    void buildActivity(QVBoxLayout& page);
    void arrangeStats(int columns);
    void arrangeMcpActions(int width);

    void onServiceEvent(const std::string& kind);
    void schedule(std::initializer_list<const char*> parts);
    void flushPending(bool force);
    void apply(Read read);
    void showUnbound();

    void renderHero(const remote::HostState& state);
    QString where(const remote::HostState& state) const;
    void setSharing(bool enabled);
    void act(std::function<void(remote::RemoteEngineControl&)> task);

    void renderPairing(const remote::HostState& state);
    void showPairing();
    void renderDevices(const remote::HostState& state, const std::map<std::string, Json>& stored);
    void updateDeviceStatus();

    void renderEngine();
    void loadModels();
    void syncModelCombo();
    void onModelActivated(int index);

    void refreshMcp();
    void toggleMcp();
    void copyMcpAccessToken();
    void copyMcpAgentPrompt();

    void refreshLive(std::vector<remote::ConnectedClient> clients, Json activity);
    void renderStats(const std::vector<host_text::GroupedClient>& grouped, const Json& activity,
                     bool running);
    void renderClients(const std::vector<host_text::GroupedClient>& grouped, const Json& activity,
                       double now, bool running);
    void renderActivity(const Json& activity);

    double now() const;
    const std::chrono::time_zone& zone() const;

    remote::RemoteEngineControl* service_ = nullptr;
    Executor& io_;
    Executor& ui_;
    Subscription events_;
    Subscription engineBusyEvents_;
    Parts parts_;
    QVBoxLayout* page_ = nullptr;
    QGridLayout* statsGrid_ = nullptr;
    int statColumns_ = 0;
    QTimer* flushTimer_ = nullptr;
    QTimer* clockTimer_ = nullptr;
    QTimer* countdown_ = nullptr;
    QTimer* mcpTimer_ = nullptr;
    QTimer* tokenFeedback_ = nullptr;
    QTimer* promptFeedback_ = nullptr;

    std::set<std::string> pending_;
    bool reading_ = false;
    bool changing_ = false;
    std::optional<remote::HostState> state_;
    std::map<std::string, Json> stored_;
    Json engine_ = Json::object();
    bool engineBusy_ = false;
    std::optional<std::tuple<QString, QString, QString>> switching_;
    QString engineError_;
    std::optional<std::vector<remote::HostModel>> models_;
    unsigned modelsToken_ = 0;
    std::vector<remote::ConnectedClient> clients_;
    std::optional<std::pair<double, long long>> seenActivity_;
    std::optional<long long> seenTranscriptions_;
    std::map<std::string, host_dashboard::ClientRow*> clientRows_;
    std::map<std::string, host_dashboard::DeviceRow> deviceRows_;
    QString pairingCode_;
    std::chrono::steady_clock::time_point pairingDeadline_;

    agentapi::McpServerControl* mcp_ = nullptr;
    bool mcpBound_ = false;
    QString mcpNotice_;
    std::function<agentapi::McpServerControl*()> mcpLookup_;
    SettingsStore* settings_ = nullptr;
    const Clock* clock_ = nullptr;
    std::function<void(const QString&)> writeClipboard_;
    std::function<bool(const QString&, const QString&)> confirmStop_;
    QPushButton* history_ = nullptr;
};

} // namespace fv
