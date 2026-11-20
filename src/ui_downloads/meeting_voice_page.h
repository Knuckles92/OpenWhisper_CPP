#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QString>
#include <QWidget>

#include "appkit/settings_page.h"
#include "base/json.h"
#include "base/subscription.h"

class QComboBox;
class QMessageBox;

namespace fv {

class AppContext;
class Button;
class Clock;
class Executor;
class LocalModelPicker;
class SettingsStore;
class VoiceModelPageHost;
class WrappedLabel;

namespace qt {
class QtUiExecutor;
}

// What the Voice & speakers page says about the on-device speaker model
// (_local_speaker_model_status): the status sentence, and whether "Download now" is offered.
struct SpeakerModelStatus {
    std::string text;
    bool offerDownload = false;
};
// downloading: a download this page started has not answered yet; ready: a speaker model is on
// this computer; blocked: why a download is not allowed now ("" when it is); error: why the last
// download failed ("" when none did).
SpeakerModelStatus speaker_model_status(bool downloading, bool ready, std::string_view blocked,
                                        std::string_view error);

// What the page reads of this computer and how it fetches the speaker model, so a test answers
// them without a disk or a network. Unset members read as: no model, no block, no failure, a
// download that does nothing.
struct MeetingVoicePageOptions {
    // speaker_model_path() found a model. Cheap; asked on the GUI thread, as Python does.
    std::function<bool()> speakerModelReady;
    // speaker_model_download_blocked(): why a download may not run now, or "".
    std::function<std::string()> speakerDownloadBlocked;
    // speaker_model_error(): why this process's last download failed, or "".
    std::function<std::string()> speakerModelError;
    // ensure_speaker_model(): blocking; runs on the page's worker, never on the GUI thread. What
    // it throws is kept by the model module for speakerModelError.
    std::function<void()> ensureSpeakerModel;
    // "Configure remote engine": Settings → Remote engine.
    std::function<void()> openRemoteEngine;
    // The clock the speaker backend's OpenAI retirement is judged by; the system clock when
    // unset.
    const Clock* clock = nullptr;
    bool remoteMeetingSupported = false;
    // Authenticated handshake and capability check on the worker. Receives the settings as
    // they were when Test connection was clicked.
    std::function<std::string(const Json&)> testRemoteMeeting;
};

// Settings → Meeting Mode → Voice & speakers (settings_models.py build_meeting_voice_page and its
// handlers). Speech: "Same as dictation" or "A different engine", the meeting's own model, the
// sentence saying what the meeting runs on, and the spoken language. Speakers: Off, On-device
// (WeSpeaker) or OpenAI, the status of the on-device model and "Download now".
//
// Every choice is saved as it is made, straight to the settings store, as ModelAssignments saves
// its own; a save that fails says so on the window's message line. The OpenAI speaker pass is not
// part of this build: its item reads "OpenAI (retired)" and cannot be chosen, and a stored
// "openai" shows as On-device. An available remote service exposes explicit actions to choose a
// paired host or return meeting speech to this computer. "Runs on" is available for recovering an
// imported remote setting when the service is unavailable. The page follows a change of the
// dictation engine made elsewhere through the store's watcher. "Download now" fetches the model on
// the worker; the answer comes back through a TokenGate on the page, so a closed page is never held
// by the worker. Affinity: GUI thread.
class MeetingVoicePage : public QWidget, public SettingsPageModule {
    Q_OBJECT

public:
    // settings and host outlive the page; worker outlives every download the page started.
    MeetingVoicePage(SettingsStore& settings, VoiceModelPageHost& host, Executor& worker,
                     MeetingVoicePageOptions options = {}, QWidget* parent = nullptr);
    ~MeetingVoicePage() override;

    // The rail's value: the meeting model and language (meeting_voice_summary).
    static QString railValue(const Json& settings);

    // _load_meeting_settings and the picker's options: shows the saved choices.
    void loadSaved();
    // refresh_component_state: the speaker model's status again.
    void refreshSpeakerStatus();
    // _download_speaker_model: fetches the speaker model on the worker.
    void downloadSpeakerModel();
    // A download this page started has not answered yet.
    bool isDownloading() const { return downloadToken_ != 0; }

    QComboBox* engineCombo() const { return engineCombo_; }
    QComboBox* sourceCombo() const { return sourceCombo_; }
    QWidget* sourceField() const { return sourceField_; }
    LocalModelPicker* modelPicker() const { return picker_; }
    QWidget* localModelField() const { return localModelField_; }
    QWidget* remoteControls() const { return remoteControls_; }
    Button* remoteTestButton() const { return remoteTest_; }
    WrappedLabel* remoteStatus() const { return remoteStatus_; }
    WrappedLabel* runtimeLabel() const { return runtimeLabel_; }
    QComboBox* languageCombo() const { return languageCombo_; }
    QComboBox* speakerCombo() const { return speakerCombo_; }
    WrappedLabel* speakerStatus() const { return speakerStatus_; }
    Button* speakerDownloadButton() const { return speakerDownload_; }

    std::function<int(QMessageBox&)> runRemoteConfirmation;

    // SettingsPageModule.
    void loadSettings(const Json& settings) override;
    void refreshPage() override;
    void onShown() override { refreshPage(); }
    QWidget* namedControl(std::string_view name) override;

    // MODEL_CONTROL_DESTINATIONS' meeting_voice entries, less _speaker_id_backend_previous.
    static const std::vector<std::string>& controlNames();

Q_SIGNALS:
    // "Manage downloads" under the meeting model: Downloads, unfiltered.
    void downloadsRequested(const QString& filter);

private:
    struct Mailbox;

    void onEngineChanged();
    void onSourceChanged();
    void chooseRemoteHost();
    void useLocalSpeech();
    void testRemoteSpeech();
    void onModelChosen(const QString& model);
    void onLanguageChanged();
    void onSpeakerChanged();
    void refreshSource();
    void refreshRuntimeLabel();
    void onSpeakerModelDownloaded(std::uint64_t token);
    const Clock& clock() const;
    void say(const QString& text);

    SettingsStore& settings_;
    VoiceModelPageHost& host_;
    Executor& worker_;
    MeetingVoicePageOptions options_;

    QComboBox* engineCombo_ = nullptr;
    QComboBox* sourceCombo_ = nullptr;
    QWidget* sourceField_ = nullptr;
    LocalModelPicker* picker_ = nullptr;
    QWidget* localModelField_ = nullptr;
    QWidget* remoteControls_ = nullptr;
    Button* remoteTest_ = nullptr;
    Button* chooseRemote_ = nullptr;
    Button* useLocal_ = nullptr;
    WrappedLabel* remoteStatus_ = nullptr;
    WrappedLabel* runtimeLabel_ = nullptr;
    QComboBox* languageCombo_ = nullptr;
    QComboBox* speakerCombo_ = nullptr;
    WrappedLabel* speakerStatus_ = nullptr;
    Button* speakerDownload_ = nullptr;

    // The backend the combo goes back to when a choice is refused (_speaker_id_backend_previous).
    std::string speakerPrevious_ = "local";
    // The token of the download under way, or 0.
    std::uint64_t downloadToken_ = 0;
    std::shared_ptr<Mailbox> mailbox_;
    std::shared_ptr<qt::QtUiExecutor> ui_;
    Subscription engineWatch_;
};

// Registers destinations::meeting_voice ("Voice & speakers") with its rail value, search fields
// and control names, as registerVoiceModelPage does for voice_model. `options` makes the page's
// options (a test's); unset, they are read from the context's paths, settings and HTTP client.
void registerMeetingVoicePage(AppContext& context,
                              std::function<MeetingVoicePageOptions()> options = {});

} // namespace fv
