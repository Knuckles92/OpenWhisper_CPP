#include "ui_downloads/meeting_voice_page.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/clock.h"
#include "base/executor.h"
#include "base/log.h"
#include "models/meeting_speech.h"
#include "remote/pairing_settings.h"
#include "settings/enums.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_downloads/model_assignments.h"
#include "ui_downloads/voice_model_page.h"
#include "ui_settings/settings_fields.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/local_model_picker.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_models"};

QString q(std::string_view text) {
    return qt::toQString(text);
}

std::optional<remote::ClientPairing> savedMeetingPeer(const Json& settings) {
    try {
        return remote::load_client_pairing(settings);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

QString errorText(const std::exception& error) {
    return QString::fromUtf8(error.what());
}

// _card: a resting settings card; returns its content column.
QVBoxLayout* addCard(QVBoxLayout* layout) {
    auto* card = new QFrame();
    card->setObjectName(QStringLiteral("settingsTile"));
    ui::setStyleProperty(card, ui::prop::kind, QStringLiteral("field"));
    auto* inner = new QVBoxLayout(card);
    inner->setContentsMargins(16, 14, 16, 14);
    inner->setSpacing(10);
    layout->addWidget(card);
    return inner;
}

ElidingComboBox* fieldCombo(const char* objectName) {
    auto* combo = new ElidingComboBox();
    combo->setObjectName(QString::fromUtf8(objectName));
    combo->setMinimumHeight(40);
    return combo;
}

constexpr std::string_view kSpeakersOff = "off";
constexpr std::string_view kSpeakersLocal = "local";
constexpr std::string_view kSpeakersOpenAi = "openai";

} // namespace

SpeakerModelStatus speaker_model_status(bool downloading, bool ready, std::string_view blocked,
                                        std::string_view error) {
    // The model is not a Downloads component: the first meeting that needs it fetches it into a
    // per-user cache (ensure_speaker_model).
    if (downloading) {
        return {"Downloading on-device WeSpeaker (voxceleb_resnet34_LM.onnx, about 26 MB) from "
                "Hugging Face…",
                false};
    }
    if (ready) {
        return {"On-device WeSpeaker (voxceleb_resnet34_LM.onnx) is ready on this computer.",
                false};
    }
    if (!blocked.empty()) {
        return {std::string(blocked) + " Meetings use Me/Others channel labels.", false};
    }
    if (!error.empty()) {
        return {"The last download failed. " + std::string(error) +
                    " Meetings use Me/Others channel labels until it downloads.",
                true};
    }
    return {"On-device WeSpeaker (voxceleb_resnet34_LM.onnx, about 26 MB) downloads from Hugging "
            "Face when your next meeting starts, or now with Download now.",
            true};
}

// What a worker may touch: never the page itself, which it reaches only through deliver().
struct MeetingVoicePage::Mailbox {
    std::mutex mutex;
    MeetingVoicePage* page = nullptr;
    qt::TokenGate gate;

    // From the worker: runs fn(page) on the GUI thread if the page still lives and token is
    // still the newest download when it runs.
    template <class Fn> void deliver(std::uint64_t token, Fn&& fn) {
        std::lock_guard lock(mutex);
        if (page == nullptr) {
            return;
        }
        // The page cannot be destroyed while the lock is held, so post_task's QPointer is made
        // on a live object; from then on it drops the task if the page goes.
        gate.deliver(page, token,
                     [target = page, fn = std::forward<Fn>(fn)]() mutable { fn(*target); });
    }
};

MeetingVoicePage::MeetingVoicePage(SettingsStore& settings, VoiceModelPageHost& host,
                                   Executor& worker, MeetingVoicePageOptions options,
                                   QWidget* parent)
    : QWidget(parent), settings_(settings), host_(host), worker_(worker),
      options_(std::move(options)), mailbox_(std::make_shared<Mailbox>()),
      ui_(std::make_shared<qt::QtUiExecutor>(this)) {
    mailbox_->page = this;
    runRemoteConfirmation = [](QMessageBox& box) {
        return box.exec();
    };
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    groupTitle(layout, QStringLiteral("Speech"));
    QVBoxLayout* card = addCard(layout);
    // Meetings use the dictation engine unless given one of their own; only then do the source
    // and model below it show.
    engineCombo_ = fieldCombo("meetingSpeechEngineCombo");
    engineCombo_->addItem(QStringLiteral("Same as dictation"), false);
    engineCombo_->addItem(QStringLiteral("A different engine"), true);
    connect(engineCombo_, &QComboBox::currentIndexChanged, this, [this] { onEngineChanged(); });
    card->addWidget(settingsField(QStringLiteral("Speech engine"), engineCombo_));

    sourceCombo_ = fieldCombo("meetingSpeechSourceCombo");
    sourceCombo_->addItem(QStringLiteral("This computer"), QStringLiteral("local"));
    sourceCombo_->addItem(QStringLiteral("Remote computer"), QStringLiteral("remote"));
    connect(sourceCombo_, &QComboBox::currentIndexChanged, this, [this] { onSourceChanged(); });
    sourceField_ = settingsField(QStringLiteral("Runs on"), sourceCombo_);
    card->addWidget(sourceField_);

    picker_ = new LocalModelPicker();
    connect(picker_, &LocalModelPicker::modelChanged, this,
            [this](const QString& model) { onModelChosen(model); });
    connect(picker_, &LocalModelPicker::manageRequested, this,
            [this] { Q_EMIT downloadsRequested(QString()); });
    localModelField_ = settingsField(QStringLiteral("Meeting speech model"), picker_);
    card->addWidget(localModelField_);

    remoteControls_ = new QWidget();
    auto* remoteRow = new QHBoxLayout(remoteControls_);
    remoteRow->setContentsMargins(0, 0, 0, 0);
    auto* configureRemote = new Button(QStringLiteral("Configure remote engine"));
    configureRemote->setObjectName(QStringLiteral("meetingConfigureRemoteButton"));
    connect(configureRemote, &QPushButton::clicked, this, [this] {
        if (options_.openRemoteEngine) {
            options_.openRemoteEngine();
        }
    });
    remoteTest_ = new Button(QStringLiteral("Test connection"));
    remoteTest_->setObjectName(QStringLiteral("meetingTestRemoteButton"));
    connect(remoteTest_, &QPushButton::clicked, this, &MeetingVoicePage::testRemoteSpeech);
    if (options_.remoteMeetingSupported) {
        chooseRemote_ = new Button(tr("Choose paired host"));
        chooseRemote_->setObjectName(QStringLiteral("meetingChoosePairedHost"));
        connect(chooseRemote_, &QPushButton::clicked, this, &MeetingVoicePage::chooseRemoteHost);
        useLocal_ = new Button(tr("Use this computer"));
        useLocal_->setObjectName(QStringLiteral("meetingUseLocalSpeech"));
        connect(useLocal_, &QPushButton::clicked, this, &MeetingVoicePage::useLocalSpeech);
        remoteRow->addWidget(chooseRemote_);
        remoteRow->addWidget(useLocal_);
    }
    remoteRow->addWidget(configureRemote);
    remoteRow->addWidget(remoteTest_);
    remoteRow->addStretch();
    card->addWidget(remoteControls_);
    remoteStatus_ = settingsCaption(QString());
    card->addWidget(remoteStatus_);
    runtimeLabel_ = settingsCaption(QString());
    card->addWidget(runtimeLabel_);

    languageCombo_ = fieldCombo("meetingLanguageCombo");
    for (const enums::Entry& entry : enums::labels(enums::EnumId::MeetingLanguage)) {
        languageCombo_->addItem(q(entry.text), q(entry.value.get<std::string>()));
    }
    languageCombo_->setToolTip(QStringLiteral(
        "Choose the meeting language when known. This avoids unreliable language detection on "
        "short chunks and strong accents."));
    connect(languageCombo_, &QComboBox::currentIndexChanged, this, [this] { onLanguageChanged(); });
    card->addWidget(settingsField(QStringLiteral("Spoken language"), languageCombo_));

    groupTitle(layout, QStringLiteral("Speakers"));
    card = addCard(layout);
    speakerCombo_ = fieldCombo("meetingSpeakerIdCombo");
    speakerCombo_->addItem(QStringLiteral("Off (Me / Others channel labels only)"),
                           q(kSpeakersOff));
    speakerCombo_->addItem(QStringLiteral("On-device (WeSpeaker · Speaker 1, Speaker 2, …)"),
                           q(kSpeakersLocal));
    // OpenWhisper lists its OpenAI pass until the model retires; this build has no such pass, so
    // the choice is listed and cannot be made.
    speakerCombo_->addItem(QStringLiteral("OpenAI (retired)"), q(kSpeakersOpenAi));
    if (auto* model = qobject_cast<QStandardItemModel*>(speakerCombo_->model())) {
        model->item(2)->setEnabled(false);
    }
    connect(speakerCombo_, &QComboBox::currentIndexChanged, this, [this] { onSpeakerChanged(); });
    card->addWidget(settingsField(QStringLiteral("Speaker identification"), speakerCombo_));
    speakerStatus_ = settingsCaption(QString());
    card->addWidget(speakerStatus_);
    speakerDownload_ = neutralButton(new Button(QStringLiteral("Download now")));
    speakerDownload_->setObjectName(QStringLiteral("speakerModelDownloadButton"));
    connect(speakerDownload_, &QPushButton::clicked, this, [this] { downloadSpeakerModel(); });
    auto* downloadRow = new QHBoxLayout();
    downloadRow->addWidget(speakerDownload_);
    downloadRow->addStretch();
    card->addLayout(downloadRow);

    // choose_backend refreshes the runtime sentence wherever the dictation engine changes; here
    // the store says so.
    engineWatch_ = settings_.watch({keys::selected_model.key(), keys::local_asr_models.key(),
                                    keys::local_asr_devices.key(), keys::remote_engine_client.key(),
                                    keys::fv_meeting_remote_fingerprint.key()},
                                   *ui_, [this](const Json&) { refreshSource(); });

    loadSaved();
}

MeetingVoicePage::~MeetingVoicePage() {
    engineWatch_.disconnect();
    // A worker holds the mailbox, never the page: from here nothing is delivered.
    const std::lock_guard lock(mailbox_->mutex);
    mailbox_->page = nullptr;
}

QString MeetingVoicePage::railValue(const Json& settings) {
    return q(meeting_voice_summary(settings));
}

const Clock& MeetingVoicePage::clock() const {
    return options_.clock != nullptr ? *options_.clock : system_clock();
}

void MeetingVoicePage::say(const QString& text) {
    host_.showMessage(text);
}

const std::vector<std::string>& MeetingVoicePage::controlNames() {
    static const std::vector<std::string> names{
        "meeting_engine_combo",   "meeting_source_combo",      "meeting_source_field",
        "meeting_whisper_picker", "meeting_local_model_field", "meeting_remote_controls",
        "meeting_remote_test",    "meeting_remote_status",     "meeting_runtime_label",
        "meeting_language_combo", "meeting_speaker_id_combo",  "speaker_id_status"};
    return names;
}

QWidget* MeetingVoicePage::namedControl(std::string_view name) {
    const std::pair<std::string_view, QWidget*> controls[] = {
        {"meeting_engine_combo", engineCombo_},
        {"meeting_source_combo", sourceCombo_},
        {"meeting_source_field", sourceField_},
        {"meeting_whisper_picker", picker_},
        {"meeting_local_model_field", localModelField_},
        {"meeting_remote_controls", remoteControls_},
        {"meeting_remote_test", remoteTest_},
        {"meeting_remote_status", remoteStatus_},
        {"meeting_runtime_label", runtimeLabel_},
        {"meeting_language_combo", languageCombo_},
        {"meeting_speaker_id_combo", speakerCombo_},
        {"speaker_id_status", speakerStatus_},
        {"speaker_model_download", speakerDownload_},
    };
    for (const auto& [key, widget] : controls) {
        if (key == name) {
            return widget;
        }
    }
    return nullptr;
}

// --- Showing the saved choices ---

void MeetingVoicePage::loadSettings(const Json&) {
    loadSaved();
}

void MeetingVoicePage::refreshPage() {
    loadSaved();
    host_.refreshRailValues();
}

void MeetingVoicePage::loadSaved() {
    const std::shared_ptr<const Json> snapshot = settings_.snapshot();
    const Json& settings = *snapshot;
    {
        const QSignalBlocker blocker(engineCombo_);
        engineCombo_->setCurrentIndex(
            engineCombo_->findData(models::resolve_meeting_speech(settings).separate));
    }
    {
        const QSignalBlocker blocker(sourceCombo_);
        sourceCombo_->setCurrentIndex(
            sourceCombo_->findData(q(models::own_meeting_speech(settings).source)));
    }
    // The picker holds the meetings' own model, shown or not.
    picker_->setModels(LocalModelPicker::catalogModels(true),
                       q(models::own_meeting_speech(settings).model));
    refreshSource();
    {
        const QSignalBlocker blocker(languageCombo_);
        languageCombo_->setCurrentIndex(
            std::max(0, languageCombo_->findData(q(keys::meeting_language.read(settings)))));
    }
    const std::string backend = models::resolve_meeting_speaker_id_backend(settings, clock());
    {
        const QSignalBlocker blocker(speakerCombo_);
        speakerCombo_->setCurrentIndex(std::max(0, speakerCombo_->findData(q(backend))));
    }
    speakerPrevious_ = backend;
    refreshSpeakerStatus();
}

void MeetingVoicePage::refreshSource() {
    const bool separate = engineCombo_->currentData().toBool();
    const bool useRemote =
        separate && sourceCombo_->currentData().toString() == QStringLiteral("remote");
    const auto snapshot = settings_.snapshot();
    const auto peer = savedMeetingPeer(*snapshot);
    const bool bound = useRemote && peer &&
                       keys::fv_meeting_remote_fingerprint.read(*snapshot) == peer->fingerprint;
    sourceField_->setVisible(useRemote && !options_.remoteMeetingSupported);
    localModelField_->setVisible(separate && !useRemote);
    remoteControls_->setVisible(options_.remoteMeetingSupported);
    if (options_.remoteMeetingSupported) {
        chooseRemote_->setVisible(!bound);
        chooseRemote_->setEnabled(peer.has_value());
        useLocal_->setVisible(useRemote);
        remoteTest_->setVisible(bound && static_cast<bool>(options_.testRemoteMeeting));
    }
    if (options_.remoteMeetingSupported && !peer) {
        remoteStatus_->setText(tr("Pair a computer in Remote engine settings to use it for "
                                  "meeting speech."));
    } else if (options_.remoteMeetingSupported && useRemote && !bound) {
        remoteStatus_->setText(tr("Choose and confirm the current paired computer to allow it to "
                                  "receive meeting audio."));
    } else {
        remoteStatus_->setText(QString());
    }
    remoteStatus_->setVisible(!remoteStatus_->text().isEmpty());
    refreshRuntimeLabel();
}

void MeetingVoicePage::refreshRuntimeLabel() {
    runtimeLabel_->setText(q(meeting_runtime_sentence(*settings_.snapshot())));
}

// --- Choosing ---

void MeetingVoicePage::onEngineChanged() {
    const bool separate = engineCombo_->currentData().toBool();
    try {
        settings_.mutate([separate](Json& saved) {
            saved[std::string(keys::meeting_speech_separate.key())] = separate;
            if (!separate) {
                saved[std::string(keys::meeting_asr_source.key())] = "local";
                saved.erase(std::string(keys::fv_meeting_remote_fingerprint.key()));
            }
        });
    } catch (const std::exception& error) {
        kLog.error("Couldn't save the meeting speech engine choice: {}", error.what());
        say(QStringLiteral("Couldn't save the meeting speech engine: %1").arg(errorText(error)));
        loadSaved();
        return;
    }
    loadSaved();
    host_.refreshRailValues();
}

void MeetingVoicePage::onSourceChanged() {
    const QString source = sourceCombo_->currentData().toString();
    try {
        if (source == QStringLiteral("remote")) {
            chooseRemoteHost();
            loadSaved();
            return;
        }
        settings_.mutate([](Json& saved) {
            saved[std::string(keys::meeting_asr_source.key())] = "local";
            saved.erase(std::string(keys::fv_meeting_remote_fingerprint.key()));
        });
    } catch (const std::exception& error) {
        // OpenWhisper lets this one leave the slot; a C++ exception must not unwind through Qt.
        kLog.error("Couldn't save where meetings run: {}", error.what());
        say(QStringLiteral("Couldn't save where meetings run: %1").arg(errorText(error)));
        loadSaved();
        return;
    }
    loadSaved();
    host_.refreshRailValues();
}

void MeetingVoicePage::chooseRemoteHost() {
    if (!options_.remoteMeetingSupported) {
        return;
    }
    const auto peer = savedMeetingPeer(*settings_.snapshot());
    if (!peer) {
        say(tr("Pair a computer in Remote engine settings first."));
        return;
    }
    const auto host = q(peer->host_name.empty() ? peer->host : peer->host_name);
    QMessageBox confirm(QMessageBox::Question, tr("Choose paired host for meeting speech"),
                        tr("Save %1 as the preferred host for meeting speech? If you confirm at "
                           "the start of a meeting, captured microphone and system audio will go "
                           "to this computer for transcription.")
                            .arg(host),
                        QMessageBox::Yes | QMessageBox::Cancel, this);
    confirm.setTextFormat(Qt::PlainText);
    confirm.setInformativeText(
        tr("The connection is encrypted. Choose only a computer you trust."));
    confirm.setDefaultButton(QMessageBox::Cancel);
    if (runRemoteConfirmation(confirm) != QMessageBox::Yes) {
        return;
    }
    try {
        settings_.mutate([&](Json& saved) {
            const auto current = remote::load_client_pairing(saved);
            if (!current || *current != *peer) {
                throw std::runtime_error("The paired computer changed. Choose the current host.");
            }
            saved[std::string(keys::meeting_speech_separate.key())] = true;
            saved[std::string(keys::meeting_asr_source.key())] = "remote";
            saved[std::string(keys::fv_meeting_remote_fingerprint.key())] = peer->fingerprint;
        });
    } catch (const std::exception& error) {
        say(errorText(error));
    }
    refreshPage();
}

void MeetingVoicePage::useLocalSpeech() {
    try {
        settings_.mutate([](Json& saved) {
            saved[std::string(keys::meeting_asr_source.key())] = "local";
            saved.erase(std::string(keys::fv_meeting_remote_fingerprint.key()));
        });
    } catch (const std::exception& error) {
        say(errorText(error));
    }
    refreshPage();
}

void MeetingVoicePage::testRemoteSpeech() {
    const auto requested = settings_.snapshot();
    const auto peer = savedMeetingPeer(*requested);
    if (!peer || !options_.testRemoteMeeting) {
        return;
    }
    remoteTest_->setEnabled(false);
    remoteStatus_->setText(tr("Connecting to the paired computer…"));
    remoteStatus_->show();
    worker_.post([this, ui = ui_, test = options_.testRemoteMeeting, requested,
                  fingerprint = peer->fingerprint] {
        QString status;
        try {
            status = q(test(*requested));
        } catch (const std::exception& error) {
            status = errorText(error);
        }
        ui->post([this, status, fingerprint] {
            remoteTest_->setEnabled(true);
            const auto saved = settings_.snapshot();
            const auto current = savedMeetingPeer(*saved);
            if (current && current->fingerprint == fingerprint &&
                keys::fv_meeting_remote_fingerprint.read(*saved) == fingerprint &&
                keys::meeting_speech_separate.read(*saved) &&
                keys::meeting_asr_source.read(*saved) == "remote") {
                remoteStatus_->setText(status);
                remoteStatus_->show();
            }
        });
    });
}

void MeetingVoicePage::onModelChosen(const QString& model) {
    // Every model the picker lists is a catalog model: OpenWhisper's Whisper names, saved under
    // meeting_whisper_model, have no counterpart here.
    try {
        settings_.save_setting(keys::meeting_asr_model.key(), qt::toUtf8(model));
    } catch (const std::exception& error) {
        kLog.error("Couldn't set meeting Whisper model: {}", error.what());
        say(QStringLiteral("Couldn't set meeting transcription model: %1").arg(errorText(error)));
        return;
    }
    say(QStringLiteral("Meeting transcription model set to \"%1\"").arg(model));
    refreshPage();
}

void MeetingVoicePage::onLanguageChanged() {
    const QVariant language = languageCombo_->currentData();
    if (!language.isValid()) {
        return;
    }
    try {
        settings_.save_setting(keys::meeting_language.key(), qt::toUtf8(language.toString()));
    } catch (const std::exception& error) {
        kLog.error("Couldn't save meeting language: {}", error.what());
        say(QStringLiteral("Couldn't save spoken language: %1").arg(errorText(error)));
        return;
    }
    host_.refreshRailValues();
}

void MeetingVoicePage::onSpeakerChanged() {
    const QVariant chosen = speakerCombo_->currentData();
    if (!chosen.isValid()) {
        return;
    }
    const std::string backend = qt::toUtf8(chosen.toString());
    if (backend == kSpeakersOpenAi) {
        // The retired pass: the choice goes back, as a declined consent puts it back.
        const QSignalBlocker blocker(speakerCombo_);
        speakerCombo_->setCurrentIndex(std::max(0, speakerCombo_->findData(q(speakerPrevious_))));
        return;
    }
    try {
        settings_.save_setting(keys::meeting_speaker_id_backend.key(), backend);
        speakerPrevious_ = backend;
    } catch (const std::exception& error) {
        kLog.error("Couldn't save speaker identification: {}", error.what());
        say(QStringLiteral("Couldn't save speaker identification: %1").arg(errorText(error)));
    }
    refreshSpeakerStatus();
    host_.refreshRailValues();
}

// --- The speaker model ---

void MeetingVoicePage::refreshSpeakerStatus() {
    const std::string backend = qt::toUtf8(speakerCombo_->currentData().toString());
    if (backend != kSpeakersLocal) {
        speakerDownload_->setVisible(false);
    }
    if (backend == kSpeakersOff) {
        speakerStatus_->setText(
            QStringLiteral("Channel labels only: microphone is Me, system audio is Others. No "
                           "on-device model and no audio upload."));
        return;
    }
    if (backend == kSpeakersOpenAi) {
        // Unreachable while a stored "openai" reads as on-device; kept for the pass's return.
        speakerStatus_->setText(QStringLiteral(
            "Uploads system audio after End and relabels speakers on the saved transcript. "
            "Requires an OpenAI API key (Settings → App → API keys). This speaker pass does not "
            "upload microphone audio. OpenAI retires the model this uses "
            "(gpt-4o-transcribe-diarize) on February 26, 2027; after that, on-device labels are "
            "used."));
        return;
    }
    const bool downloading = downloadToken_ != 0;
    // While a download runs only that is said, so nothing else is asked.
    const bool ready = !downloading && options_.speakerModelReady && options_.speakerModelReady();
    const std::string blocked = !downloading && !ready && options_.speakerDownloadBlocked
                                    ? options_.speakerDownloadBlocked()
                                    : std::string();
    const std::string error =
        !downloading && !ready && blocked.empty() && options_.speakerModelError
            ? options_.speakerModelError()
            : std::string();
    const SpeakerModelStatus status = speaker_model_status(downloading, ready, blocked, error);
    speakerStatus_->setText(q(status.text) +
                            QStringLiteral(" Speakers are told apart on system audio only, so "
                                           "everyone sharing your microphone is Me."));
    speakerDownload_->setVisible(status.offerDownload);
}

void MeetingVoicePage::downloadSpeakerModel() {
    const std::uint64_t token = mailbox_->gate.next();
    downloadToken_ = token;
    refreshSpeakerStatus();
    // Bound here: the worker reaches the page only through the mailbox.
    worker_.post([mailbox = mailbox_, ensure = options_.ensureSpeakerModel, token] {
        try {
            if (ensure) {
                ensure();
            }
        } catch (const std::exception& error) {
            // ensure_speaker_model keeps the reason for the status line.
            kLog.debug("Speaker model download failed: {}", error.what());
        }
        mailbox->deliver(token,
                         [token](MeetingVoicePage& page) { page.onSpeakerModelDownloaded(token); });
    });
}

void MeetingVoicePage::onSpeakerModelDownloaded(std::uint64_t token) {
    if (token != downloadToken_) {
        return;
    }
    downloadToken_ = 0;
    refreshSpeakerStatus();
}

} // namespace fv
