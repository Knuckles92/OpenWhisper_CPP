#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QObject>
#include <QPointer>

#include "base/json.h"
#include "base/subscription.h"
#include "meeting/platform_policy.h"
#include "meeting/setup_check.h"
#include "meeting_agent/route.h"

class QMessageBox;

namespace fv {

class AppContext;
class MainWindow;
class MeetingModeTab;
class MeetingConsentGrants;
class MeetingControls;
class MeetingModeIntroDialog;
class PastMeetingsPanel;
class SystemTray;
namespace meeting {
class MeetingRuntime;
}
namespace meeting_state {
class SqlMeetingRepository;
}

// The controller side of Meeting Mode (ui_controller.py's meeting handlers): registers the tab and
// Past Meetings, runs the start readiness gates and forwards the tab, tray and history actions to
// the runtime, and applies the runtime's state to the window. Affinity: GUI thread.
class MeetingPresenter final : public QObject {
public:
    // Seams for the tests; empty members use the real dialogs and the running platform.
    struct Hooks {
        // Replaces MeetingSetupDialog: the session's system-audio policy, nullopt when cancelled.
        std::function<std::optional<std::string>(bool starting)> openSetup;
        // Runs the first-visit intro instead of exec() (maybeShowMeetingModeIntro's run).
        std::function<void(MeetingModeIntroDialog&)> runIntro;
        std::optional<meeting::PlatformInfo> platform;
    };

    MeetingPresenter(AppContext& context, MainWindow& window, meeting::MeetingRuntime& runtime,
                     meeting::SetupDeps setup,
                     std::shared_ptr<meeting_state::SqlMeetingRepository> repository,
                     std::filesystem::path root, std::shared_ptr<MeetingConsentGrants> grants = {});
    MeetingPresenter(AppContext& context, MainWindow& window,
                     std::shared_ptr<MeetingControls> controls, meeting::SetupDeps setup,
                     std::shared_ptr<meeting_state::SqlMeetingRepository> repository,
                     std::filesystem::path root, std::shared_ptr<MeetingConsentGrants> grants = {},
                     Hooks hooks = {});

    void install();
    void attachTray(SystemTray& tray);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QWidget* buildTab(QWidget* parent);
    void bindTab(MeetingModeTab& tab);
    void bindRuntime();
    void bindHistory();
    void scheduleIntro();
    bool acknowledgePlatform();
    std::optional<std::string> runSetup(bool starting);
    void start(const QString& brief = {}, bool cloud = false);
    bool askIntelligenceConsent();
    bool askIntelligenceConsent(const meeting_agent::MeetingAgentRoute& route);
    std::optional<meeting_agent::MeetingAgentRoute>
    askBackupConsent(const meeting_agent::MeetingAgentRoute& primary);
    void toggleLiveIntelligence(bool enabled);
    void refreshSpeechDestination();
    void refreshIntelligenceDestination();
    void end();
    void identifySavedSpeakers(const QString& id);
    void applyState(Json patch);
    void updateDashboard(bool available);
    void status(const QString& message);
    void showRecovery(const std::vector<Json>& rows);
    void showScanError(const QString& error);
    const meeting::PlatformInfo& platform() const;
    void copy(const std::string& text, const QString& done);
    void copyPastMeeting(const QString& id);
    void exportPastMeetings();

    AppContext& context_;
    MainWindow& window_;
    std::shared_ptr<MeetingControls> controls_;
    MeetingControls& runtime_;
    Hooks hooks_;
    meeting::SetupDeps setup_;
    std::shared_ptr<meeting_state::SqlMeetingRepository> repository_;
    std::filesystem::path root_;
    std::shared_ptr<MeetingConsentGrants> grants_;
    std::vector<Subscription> subscriptions_;
    QPointer<MeetingModeTab> tab_;
    QPointer<PastMeetingsPanel> panel_;
    QPointer<SystemTray> tray_;
    QPointer<QMessageBox> scanError_;
    Json state_ = Json::object();
    QString status_;
    bool setupOpen_ = false;
    bool introScheduled_ = false;
};

} // namespace fv
