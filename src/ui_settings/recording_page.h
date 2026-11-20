#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <QString>
#include <QWidget>

#include "appkit/qt_bridge.h"
#include "appkit/settings_page.h"
#include "base/json.h"
#include "ui_settings/microphone_section.h"

class QLabel;
class QTimer;

namespace fv {

class ElidingComboBox;
class Executor;
class FieldTile;
class NoWheelSpinBox;
class SettingTile;
class SettingsDialog;
class SettingsPage;
class SettingsPagesSignals;
class SettingsSwitch;
class WrappedLabel;
struct RecordingInfo;

namespace detail {
template <class Widget> class Mailbox;
}

// Settings → Dictation → Recording (settings_dialog.py _build_recording_page and its retention
// and live-preview handlers): the Microphone group (MicrophoneSection), "Keep recordings" with
// its mode, count and folder-size limits and the usage caption, and the live-preview switch.
//
// Retention deletes audio, so typed limits commit on Enter or focus-out, never per keystroke,
// and chevron or arrow steps settle for kRetentionSettleMs first. A commit that lowers a limit
// asks History (on the worker) what it would delete and, when anything, asks "Delete N saved
// recording(s)?" (default Cancel): Cancel puts the saved values back, Yes saves the three values
// together and applies them. While a commit is being checked or asked about, another is not
// started; one asked for meanwhile runs once the first is done.
// Affinity: GUI thread.
class RecordingPage : public QWidget, public SettingsPageModule {
    Q_OBJECT
public:
    static constexpr int kRetentionSettleMs = 800;

    struct Ports {
        MicrophoneSection::Ports microphones;
        // HistoryManager's get_recordings_usage, recordings_over_limit and set_retention; on
        // `worker`.
        std::function<std::pair<std::int64_t, std::int64_t>()> usage;
        std::function<std::vector<RecordingInfo>(std::optional<std::int64_t>,
                                                 std::optional<std::int64_t>)>
            overLimit;
        std::function<void(std::optional<std::int64_t>, std::optional<std::int64_t>)> setRetention;
        Executor* worker = nullptr;
        // "Delete N saved recording(s)?" (Yes/Cancel, default Cancel); null asks with a
        // QMessageBox.
        std::function<bool(QWidget* parent, const QString& title, const QString& text)> confirm;
    };

    RecordingPage(SettingsPage& page, SettingsDialog& dialog, SettingsPagesSignals& signals,
                  Ports ports);
    ~RecordingPage() override;

    MicrophoneSection& microphones() const { return *microphones_; }
    ElidingComboBox* retentionCombo() const { return retentionCombo_; }
    FieldTile* retentionTile() const { return retentionTile_; }
    QLabel* maxRecordingsLabel() const { return maxRecordingsLabel_; }
    NoWheelSpinBox* maxRecordingsSpin() const { return maxRecordingsSpin_; }
    QLabel* maxRecordingsMbLabel() const { return maxRecordingsMbLabel_; }
    NoWheelSpinBox* maxRecordingsMbSpin() const { return maxRecordingsMbSpin_; }
    WrappedLabel* usageLabel() const { return usageLabel_; }
    SettingTile* streamingTile() const { return streamingTile_; }
    SettingsSwitch* streamingCheck() const;
    QTimer& retentionTimer() const { return *retentionTimer_; }
    // A retention commit is being checked or asked about.
    bool confirmingRetention() const { return confirming_; }

    // _commit_retention: saves finished retention edits, asking first if they delete audio.
    void commitRetention();
    // _refresh_recordings_usage: "Currently N recording(s) using X." from the worker.
    void refreshRecordingsUsage();

    // SettingsPageModule.
    void loadSettings(const Json& settings) override;
    QWidget* namedControl(std::string_view name) override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildRetention(SettingsPage& page, QVBoxLayout* layout);
    void scheduleRetentionCommit();
    void onRetentionModeChanged();
    void updateRetentionUi();
    void loadRetentionSettings(const Json& settings);
    void finishCommit(const Json& edits, bool apply);
    void applyRetentionLimit();
    void onStreamingToggled(bool checked);
    bool loading() const;

    SettingsDialog& dialog_;
    SettingsPagesSignals& signals_;
    Ports ports_;
    std::shared_ptr<detail::Mailbox<RecordingPage>> mailbox_;
    qt::TokenGate usageGate_;

    MicrophoneSection* microphones_ = nullptr;
    ElidingComboBox* retentionCombo_ = nullptr;
    FieldTile* retentionTile_ = nullptr;
    QLabel* maxRecordingsLabel_ = nullptr;
    NoWheelSpinBox* maxRecordingsSpin_ = nullptr;
    QLabel* maxRecordingsMbLabel_ = nullptr;
    NoWheelSpinBox* maxRecordingsMbSpin_ = nullptr;
    WrappedLabel* usageLabel_ = nullptr;
    SettingTile* streamingTile_ = nullptr;
    QTimer* retentionTimer_ = nullptr;

    bool loading_ = false;
    bool confirming_ = false;
    bool recommit_ = false;
};

} // namespace fv
