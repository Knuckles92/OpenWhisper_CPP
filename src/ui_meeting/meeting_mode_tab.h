#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QWidget>

#include "appkit/qt_bridge.h"
#include "base/clock.h"
#include "base/executor.h"
#include "base/json.h"
#include "meeting/platform_policy.h"
#include "ui_shell/page_fit.h"

class QFrame;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace fv {

class BriefEdit;
class Button;
class DangerButton;
class FinalizationCard;
class LiveDot;
class RecentMeetingRow;
class SettingsSwitch;
class Stage;
class StudioSplit;
class SuccessButton;
class WrappedLabel;

namespace meeting_tab {
// Sizes at 100% type; each grows with the font scale (ui::scaled).
// The stage: the width it asks for beside the column, and its height, which holds the empty
// state with three recent meetings and sets the page's.
inline constexpr int kStageWidth = 560;
inline constexpr int kStageHeight = 540;
// Under the column's top edge: Start, or the two dashboard actions.
inline constexpr int kTopSlotHeight = 48;
// The on-air card at its widest, and its Pause and End buttons.
inline constexpr int kOnAirWidth = 520;
inline constexpr int kOnAirButtonWidth = 150;
} // namespace meeting_tab

// The AI insights switch's hover hint: four short lines.
QString aiInsightsTooltip();

// What the tab reads when it is made.
struct MeetingModeTabOptions {
    // A settings snapshot: developer_mode shows the demo controls, meeting_cloud_last_enabled
    // sets the AI insights switch.
    Json settings = Json::object();
    // Elapsed time (steady) and the zone dates are shown in; system_clock() when null.
    const Clock* clock = nullptr;
    // Which platform's capture copy and setup link to show.
    meeting::PlatformInfo platform = meeting::current_platform();
    // The first speech note and AI insights lines (setSpeechNote, setAiDestination), as the
    // tab shows them before anything asks: Python reads them while it builds the column.
    QString speechNote;
    QString aiDestination;
    QString aiCompactDestination;
};

// The Meeting Mode tab (ui_qt/widgets/meeting_mode_tab.py), objectName meetingModeTab: a control
// column (Start Meeting, the speech note, AI insights and the meeting brief) beside a stage that
// shows no meeting (what Meeting Mode does and the newest meetings), the live meeting (the
// on-air card with its clock, Pause and End) or the meeting that just ended (FinalizationCard).
// The stage changing never resizes the window. User intent leaves through the signals; state
// comes back through setMeetingState, partial payloads whose absent keys leave the state alone.
//
// Where it differs from Python, which reads settings and services itself: the speech note and
// the AI insights line are given (setSpeechNote, setAiDestination) when aiDestinationWanted asks
// for them, and the recent meetings are read through an injected reader on an executor.
// Affinity: GUI thread.
class MeetingModeTab : public QWidget, public FittedPage {
    Q_OBJECT
public:
    using RecentReader = std::function<std::vector<Json>()>;

    explicit MeetingModeTab(QWidget* parent = nullptr);
    explicit MeetingModeTab(const MeetingModeTabOptions& options, QWidget* parent = nullptr);
    ~MeetingModeTab() override;

    // set_meeting_state: a (possibly partial) payload with any of active, paused, status,
    // cloud_enabled, elapsed_s (meeting seconds, pauses excluded) with elapsed_at (seconds on the
    // clock's steady() when elapsed_s was read, so a retained payload replayed later does not
    // rewind the clock), finalization (null clears it), dashboard_available, meeting_id, title,
    // display_title, started_at, ended_at, paused_total_s, speech, background_available and
    // background_message. A finalization whose current_step or total_steps int() refuses, whose
    // steps list() refuses or whose summary_stats or content_summary is not a dict throws
    // Python's py::ValueError or py::TypeError, with the keys before it applied and the previous
    // finalization kept, as in Python.
    void setMeetingState(const QJsonObject& patch);
    void setMeetingState(const Json& patch);
    // Another mode (Quick Record, a walkthrough) holds the microphone: Start and the demo wait.
    // Invokable: the main window calls it by name on the meeting screen.
    Q_INVOKABLE void setRecordingActive(bool active);
    void setDeveloperMode(bool enabled);
    // The status pill's text ("Active", "Paused"); empty text is ignored.
    void setStatusText(const QString& text);
    // Open dashboard stays enabled while the runtime still holds a dashboard URL.
    void setDashboardAvailable(bool available);
    // set_recent_meetings: the rows the empty stage opens, newest first (at most three shown).
    void setRecentMeetings(const QList<QJsonObject>& meetings);
    void showRecentMeetings(const std::vector<Json>& meetings);
    // The speech line: "{note} Speech recognition runs on this computer." (stripped). Ignored
    // while a meeting is active: it keeps the engine it started with.
    void setSpeechNote(const QString& note);
    // The speech line when meeting speech goes to a paired remote host (refresh_ai_destination's
    // remote branch): "Remote speech · microphone and system audio sent to {host}. Recordings
    // saved here.", with "a host (not paired yet)" when there is no pairing (nullopt). Ignored
    // while a meeting is active.
    void setRemoteSpeechHost(const std::optional<QString>& host);
    // The AI insights line ("{where} — {privacy}.") and its compact form ({where}) shown during a
    // meeting. Ignored while a meeting is active.
    void setAiDestination(const QString& full, const QString& compact);
    // False hides the AI INSIGHTS section and the brief, and the switch's choice is sent as off.
    void setAiInsightsAvailable(bool available);
    bool aiInsightsAvailable() const { return aiAvailable_; }
    // Reads the recent meetings with reader on io whenever the empty stage shows; the newest
    // read wins. Without a reader the tab only emits recentMeetingsWanted.
    void setRecentMeetingsReader(Executor& io, RecentReader reader);

    // The host's pre-meeting brief, stripped and capped at MAX_INTENT_LEN code points; "" while
    // AI insights are off (the field is hidden then, so text left in it must not be sent).
    QString meetingBrief() const;
    // "empty", "live" or "result".
    QString stageState() const;
    bool isMeetingActive() const { return active_; }
    // The finalization status on the stage; nullopt when none.
    std::optional<QString> finalizationStatus() const;

    // FittedPage: room for the column beside the stage, at any type size.
    int preferredWidth() const override;
    void prepareForWidthChange(int grow) override;

    // The parts tests and the presenter reach.
    QScrollArea* scrollArea() const { return scrollArea_; }
    QWidget* contentWidget() const { return content_; }
    StudioSplit* split() const { return split_; }
    Stage* stage() const { return stage_; }
    QFrame* column() const { return column_; }
    QLabel* betaTag() const { return betaTag_; }
    QWidget* topSlot() const { return topSlot_; }
    QWidget* idleCard() const { return idleCard_; }
    SuccessButton* startButton() const { return startButton_; }
    Button* demoButton() const { return demoButton_; }
    WrappedLabel* demoHint() const { return demoHint_; }
    QWidget* liveActions() const { return liveActions_; }
    Button* dashboardButton() const { return dashboardButton_; }
    Button* guestLinkButton() const { return guestLinkButton_; }
    WrappedLabel* slotNote() const { return slotNote_; }
    WrappedLabel* speechDestination() const { return speechDestination_; }
    QWidget* aiPanel() const { return aiPanel_; }
    QLabel* aiLabel() const { return aiLabel_; }
    SettingsSwitch* cloudCheckbox() const { return cloudCheckbox_; }
    WrappedLabel* aiCompactDestination() const { return aiCompactDestination_; }
    QWidget* aiDetails() const { return aiDetails_; }
    WrappedLabel* aiDestination() const { return aiDestination_; }
    QPushButton* aiSettingsButton() const { return aiSettingsButton_; }
    QWidget* briefPanel() const { return briefPanel_; }
    QLabel* briefLabel() const { return briefLabel_; }
    BriefEdit* briefInput() const { return briefInput_; }
    WrappedLabel* backgroundNotice() const { return backgroundNotice_; }
    // Null where setup is not offered.
    QPushButton* setupLink() const { return setupLink_; }
    QWidget* emptyPane() const { return emptyPane_; }
    QWidget* livePane() const { return livePane_; }
    QWidget* resultPane() const { return resultPane_; }
    QWidget* intro() const { return intro_; }
    WrappedLabel* subtitle() const { return subtitle_; }
    WrappedLabel* platformHint() const { return platformHint_; }
    QWidget* recentBox() const { return recentBox_; }
    const std::vector<RecentMeetingRow*>& recentRows() const { return recentRows_; }
    QFrame* sessionCard() const { return sessionCard_; }
    LiveDot* liveDot() const { return liveDot_; }
    QLabel* statusPill() const { return statusPill_; }
    QLabel* elapsedLabel() const { return elapsedLabel_; }
    Button* pauseButton() const { return pauseButton_; }
    DangerButton* endButton() const { return endButton_; }
    WrappedLabel* liveHint() const { return liveHint_; }
    QScrollArea* resultScroll() const { return resultScroll_; }
    FinalizationCard* finalizationCard() const { return finalizationCard_; }
    QTimer* elapsedTimer() const { return elapsedTimer_; }
    // Seconds on the clock's steady() since the running clock was last anchored; nullopt while
    // it stands (paused or no meeting).
    std::optional<double> runningSince() const { return runningSince_; }
    // _refresh_elapsed: the elapsed label from the pause-aware clock (the 1 s timer calls it).
    void refreshElapsed();

Q_SIGNALS:
    void startRequested(bool cloud, const QString& brief);
    void setupRequested();
    void demoRequested(bool cloud);
    void pauseRequested();
    void resumeRequested();
    void endRequested();
    void openDashboardRequested();
    void openReportRequested();
    void copyGuestLinkRequested();
    void cloudToggled(bool enabled);
    // "Change…" beside AI insights: open Settings → Meeting Mode → Intelligence.
    void aiSettingsRequested();
    void retryInsightsRequested();
    void retrySpeakersRequested();
    void retryStepRequested(const QString& stepId);
    void backgroundRequested();
    void deferInsightsRequested();
    // A recent meeting on the empty stage was opened: its id.
    void pastMeetingRequested(const QString& meetingId);
    // The empty stage wants its meetings read again: on show, on window activation and when the
    // stage goes back to empty while shown.
    void recentMeetingsWanted();
    // refresh_ai_destination's moments (made, shown, window activation, a meeting starting):
    // the receiver answers with setSpeechNote (or setRemoteSpeechHost) and setAiDestination.
    void aiDestinationWanted();

protected:
    void showEvent(QShowEvent* event) override;
    bool event(QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void setupUi(const MeetingModeTabOptions& options);
    QFrame* buildColumn(const MeetingModeTabOptions& options);
    void addSection(QVBoxLayout* body, const QString& title);
    QWidget* buildAiPanel(const MeetingModeTabOptions& initial);
    QWidget* buildBriefPanel();
    QFrame* buildEmptyPane();
    QFrame* buildLivePane();
    QFrame* buildResultPane();

    void syncBriefAvailability();
    bool settingUp() const { return !(active_ || starting_); }
    bool wrappingUp() const;
    bool aiShown() const;
    void refreshAiDestination();

    void onStartClicked();
    void onDemoClicked();
    void onPauseClicked();

    void applyBackground(const Json& payload);
    void applyCloudChoice(const Json& payload);
    void applySpeechDestination(const Json& speech);
    void restartElapsed(const Json& elapsedS, const Json& elapsedAt);
    void applyStatus(const std::string& status, const Json& payload);
    void applyIdentity(const Json& payload);
    void setFinalization(const Json& value);
    void setActive(bool active);
    void setPaused(bool paused);
    void applyLayoutState();
    void showStage(const QString& state);
    void syncLiveTone();
    void applySizes();
    void renderIdentity();
    bool incompleteFinalization() const;
    void loadRecent();
    double now() const;

    const Clock& clock_;
    meeting::PlatformInfo platform_;

    bool active_ = false;
    bool backgroundAvailable_ = false;
    bool starting_ = false;
    // Quick Record or a walkthrough holds the microphone.
    bool recordingActive_ = false;
    bool paused_ = false;
    double elapsedBaseS_ = 0.0;
    std::optional<double> runningSince_;
    // The normalized finalization payload, or null.
    Json finalization_;
    Json identity_ = Json::object();
    std::optional<QString> meetingId_;
    bool hasDashboard_ = false;
    bool developerMode_ = false;
    bool aiAvailable_ = true;
    bool built_ = false;
    std::optional<QString> stageState_;

    Executor* recentIo_ = nullptr;
    std::shared_ptr<RecentReader> recentReader_;
    // Brings a read back to this tab, and drops it once the tab is gone.
    std::shared_ptr<qt::QtUiExecutor> ui_;
    qt::TokenGate recentGate_;

    QScrollArea* scrollArea_ = nullptr;
    QWidget* content_ = nullptr;
    Stage* stage_ = nullptr;
    StudioSplit* split_ = nullptr;
    QFrame* column_ = nullptr;
    QLabel* betaTag_ = nullptr;
    QWidget* topSlot_ = nullptr;
    QWidget* idleCard_ = nullptr;
    SuccessButton* startButton_ = nullptr;
    Button* demoButton_ = nullptr;
    WrappedLabel* demoHint_ = nullptr;
    QWidget* liveActions_ = nullptr;
    Button* dashboardButton_ = nullptr;
    Button* guestLinkButton_ = nullptr;
    WrappedLabel* slotNote_ = nullptr;
    WrappedLabel* speechDestination_ = nullptr;
    QFrame* aiRule_ = nullptr;
    QLabel* aiOverline_ = nullptr;
    QWidget* aiPanel_ = nullptr;
    QLabel* aiLabel_ = nullptr;
    SettingsSwitch* cloudCheckbox_ = nullptr;
    WrappedLabel* aiCompactDestination_ = nullptr;
    QWidget* aiDetails_ = nullptr;
    WrappedLabel* aiDescription_ = nullptr;
    WrappedLabel* aiDestination_ = nullptr;
    QPushButton* aiSettingsButton_ = nullptr;
    QWidget* briefPanel_ = nullptr;
    QLabel* briefLabel_ = nullptr;
    BriefEdit* briefInput_ = nullptr;
    WrappedLabel* briefHint_ = nullptr;
    WrappedLabel* backgroundNotice_ = nullptr;
    QPushButton* setupLink_ = nullptr;
    QFrame* emptyPane_ = nullptr;
    QFrame* livePane_ = nullptr;
    QFrame* resultPane_ = nullptr;
    QWidget* intro_ = nullptr;
    WrappedLabel* subtitle_ = nullptr;
    WrappedLabel* platformHint_ = nullptr;
    QWidget* recentBox_ = nullptr;
    QVBoxLayout* recentLayout_ = nullptr;
    std::vector<RecentMeetingRow*> recentRows_;
    QFrame* sessionCard_ = nullptr;
    LiveDot* liveDot_ = nullptr;
    QLabel* statusPill_ = nullptr;
    QLabel* elapsedLabel_ = nullptr;
    Button* pauseButton_ = nullptr;
    DangerButton* endButton_ = nullptr;
    WrappedLabel* liveHint_ = nullptr;
    QScrollArea* resultScroll_ = nullptr;
    FinalizationCard* finalizationCard_ = nullptr;
    QTimer* elapsedTimer_ = nullptr;
};

} // namespace fv
