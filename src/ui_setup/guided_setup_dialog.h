#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QDialog>
#include <QPointer>
#include <QSize>
#include <QString>

#include "base/json.h"
#include "ui_record/microphone_check.h"
#include "ui_widgets/segmented_bar.h"

class QBoxLayout;
class QFrame;
class QHBoxLayout;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTimer;
class QVBoxLayout;

namespace fv {

class ElidingComboBox;
class ProfileHotkeyInput;
class SettingsDialog;
class SettingsStore;
class SettingsSwitch;
class WrappedLabel;

// --- The pure parts of ui_qt/dialogs/guided_setup.py ---

// _on_number_pad: the shortcut's main key is on the number pad ("kp *", "ctrl+kp 1").
bool on_number_pad(std::string_view hotkey);

// shortcut_help: how the recording shortcut is used in the saved trigger mode ("Hold it while
// you speak, then let go." with the double-tap sentence when the hands-free latch is on, else
// "Press it to start dictating, and again to stop."), then " It's on the number pad." for a
// number-pad key, then the side-button note (hotkeys::mouse_shortcut_note), joined by spaces.
std::string shortcut_help(std::string_view hotkey, const Json& settings);

// A speech download this window started: what it says ("Downloading…"), bytes done and the
// total (0 when unknown).
struct SpeechProgress {
    std::string text;
    std::int64_t done = 0;
    std::int64_t total = 0;
};

// model_download_progress's text: "Downloading… <done> of <total>" (format_size_bytes) when the
// total is known, else "Downloading…".
std::string download_progress_text(std::int64_t done, std::int64_t total);

// What _refresh_speech shows on the Record and Ready pages for one state.
struct SpeechStatus {
    // The Record page's "Speech engine" line.
    std::string summary_line;
    // The Ready page's note; nullopt where _refresh_speech leaves it as it was.
    std::optional<std::string> ready_note;
    // "", "ready" or "attention", on both lines.
    std::string tone;
    // The 8 px bars: shown only while a download runs; the range's maximum (100, or 0 for
    // indeterminate) and the value set when the total is known.
    bool progress_visible = false;
    int bar_maximum = 0;
    std::optional<int> bar_value;
    // The Record page's Download button, the Ready page's Download link and its row.
    bool record_download_visible = false;
    bool ready_download_visible = false;
    bool ready_row_visible = false;
    std::string ready_title;
    std::string ready_lede;
};

// _refresh_speech: `summary` and `detail` are the voice summary and detail (ModelAssignments'
// voice_summary and voice_detail); `ready` is speech_model_downloaded's answer (nullopt:
// dictation runs no local model); `error` is the last download's failure ("" for none).
SpeechStatus speech_status(std::string_view summary, std::string_view detail,
                           std::optional<bool> ready, const std::optional<SpeechProgress>& progress,
                           std::string_view error);

// --- The window ---

// The AI cleanup choices the Clean up step projects. M2 saves through the Settings window's
// persist path; M4 replaces these with its AI cleanup page's handlers (the cleanup switch,
// choose_cleanup_level, _update_cleanup_prompt_ui and cleanup_prompts.custom_prompt).
struct CleanupPort {
    // The switch: saves transcript_cleanup_enabled; false when it was not saved (the window's
    // message line then says why).
    std::function<bool(SettingsDialog&, bool enabled)> setEnabled;
    // choose_cleanup_level: saves transcript_cleanup_level; a saved custom prompt replaces every
    // preset, so it is dropped after asking. Returns whether the level was saved.
    std::function<bool(SettingsDialog&, const QString& level)> chooseLevel;
    // cleanup_prompts.custom_prompt: the user's saved prompt, or "" when none is saved.
    std::function<std::string(const Json& settings)> customPrompt;
    // _update_cleanup_prompt_ui, after the switch changed; empty does nothing.
    std::function<void(SettingsDialog&)> updatePromptUi;

    // Through CleanupSettings (the AI cleanup page's switch, choose_cleanup_level and
    // _update_cleanup_prompt_ui) when the Settings window's context has it, else
    // SettingsDialog::persist and persistMany; customPrompt is text::custom_prompt (a blank
    // prompt, or a saved copy of a built-in one, is none). Without CleanupSettings the question
    // "Remove your custom prompt?" is asked through `confirm` (a QMessageBox, default No, when
    // empty).
    static CleanupPort
    settingsDefault(std::function<bool(QWidget* parent, const QString& level)> confirm = {});
};

// provider_summary's answer (M4): the cleanup provider's name and status, the privacy
// sentence, whether text leaves this computer, and whether it still needs an API key.
struct ProviderSummary {
    QString name;
    QString status;
    QString privacy;
    bool remote = false;
    bool needsKey = false;
};

// Everything the window reaches outside itself; each a seam a test replaces.
struct GuidedSetupPorts {
    // get_settings: the configured Settings window, asked once, when setup first needs it
    // (never for the Welcome page alone).
    std::function<SettingsDialog&()> settings;
    // open_destination: Settings on a destination key.
    std::function<void(const QString& destination)> openDestination;
    // open_all_settings: Settings on Home.
    std::function<void()> openAllSettings;
    // download_model: the app's own download of a model by name, its consent included. Empty
    // sends the user to the Voice model page instead.
    std::function<void(const QString& model)> downloadModel;
    // The microphone test's recorder on the saved microphone (_recording_look_recorder).
    MicrophoneCheck::RecorderFactory recorder;
    // setup_speech's speech_model_name and speech_model_downloaded over a settings snapshot.
    std::function<std::optional<std::string>(const Json& settings)> speechModelName;
    std::function<std::optional<bool>(const Json& settings)> speechModelDownloaded;
    // RUNTIME_IDS: whether a component is a speech runtime; models::runtime_ids() when empty.
    std::function<bool(const QString& componentId)> isRuntime;
    CleanupPort cleanup;
    // provider_summary (M4); while empty the provider row and the privacy text stay hidden.
    std::function<ProviderSummary(const Json& settings)> providerSummary;
    // OpenWhisperImport::offered: an OpenWhisper data folder was found and nothing was imported
    // yet. While empty or false the Welcome page has no import row.
    std::function<bool()> importOffered;
    // OpenWhisperImport::open, from the import row's button.
    std::function<void(QWidget* parent)> openImport;
};

// The Clean up step's three preset tiles (Light, Medium, High), with no tile selected while a
// custom prompt wins (guided_setup.py CleanupChoices).
// Affinity: GUI thread.
class CleanupChoices : public SegmentedBar {
    Q_OBJECT
public:
    // cleanup_levels.SEGMENTS: (value, title, detail), Custom last.
    struct Segment {
        const char* value;
        const char* title;
        const char* detail;
    };
    static const std::vector<Segment>& segments();
    // cleanup_levels.segment_index: the segment of a level or "custom"; Medium's for anything
    // else.
    static int segmentIndex(std::string_view level);

    explicit CleanupChoices(QWidget* parent = nullptr);

    // set_choice: the level's tile, or none while a custom prompt wins.
    void setChoice(const QString& level, bool custom);
    bool isCustom() const { return custom_; }

private:
    bool custom_ = false;
};

// Optional first-run setup: a welcome, Record → Clean up → Output, then a try-it page
// (ui_qt/dialogs/guided_setup.py GuidedSetupDialog, object name "guidedSetupDialog"). Every
// control is a projection of a Settings control: the window re-syncs that control from the
// saved settings, sets it, shows the Settings window's message line when it reports a failure,
// then re-syncs again, so it always shows what was saved. Settings is built only once the user
// leaves Welcome. Non-modal; the try-it box takes hotkey dictation (openwhisperCppHotkeyWindow).
// Affinity: GUI thread.
class GuidedSetupDialog : public QDialog {
    Q_OBJECT
public:
    enum Page { Welcome = 0, Record = 1, CleanUp = 2, Output = 3, Ready = 4 };

    static constexpr QSize kMinimumSize{540, 460};
    static constexpr QSize kDefaultSize{720, 690};
    // Rows stack below this width at 100 % text.
    static constexpr int kStackBelow = 620;
    // The try box fills this long after a dictation (auto-paste on), or kTryFillQuickMs.
    static constexpr int kTryFillMs = 1500;
    static constexpr int kTryFillQuickMs = 300;

    GuidedSetupDialog(SettingsStore& store, GuidedSetupPorts ports, QWidget* parent = nullptr);
    ~GuidedSetupDialog() override;

    // begin: a fresh pass, on Welcome when offered, else on Record (saving "started" unless the
    // setup was completed before).
    void begin(bool welcome);
    // show_page: clamps to Welcome..Ready, cancels a capture, stops the microphone check,
    // resets the scroll, refreshes speech status on Record and Ready, and builds Settings on the
    // first page after Welcome.
    void showPage(int page);
    int page() const { return page_; }
    // refresh: every projection from the saved settings (nothing before Settings is built).
    void refresh();
    void showError(const QString& text);

    // The Settings window, built on first use (_ensure_settings); null before.
    SettingsDialog& ensureSettings();
    SettingsDialog* settingsDialog() const;

    // --- The speech download ---
    bool isExpectingDownload() const { return expectingDownload_; }
    // shows_model_download: the model this window's download is running for.
    bool showsModelDownload(const QString& model) const;
    // Whether this window shows `model`'s download once it starts: the running one, or the one
    // it asked for and still expects (its model_download_started handler claims that one).
    bool claimsModelDownload(const QString& model) const;
    // Whether this window shows the install the user started here: visible, expecting a
    // download, and a runtime component.
    bool showsRuntimeInstall(const QString& componentId) const;
    void modelDownloadStarted(const QString& model);
    void modelDownloadProgress(const QString& model, std::int64_t done, std::int64_t total);
    void modelDownloadFinished(const QString& model, bool success);
    void runtimeInstallProgress(const QString& componentId, std::int64_t done, std::int64_t total);
    void runtimeInstallFinished(const QString& componentId, bool success, const QString& message);

    // --- Trying it out ---
    // A dictation finished: only on a visible Ready page does it count.
    void dictationReceived(const QString& text);
    void dictationFailed(const QString& message);

    void setDownloadModel(std::function<void(const QString&)> download);
    void setProviderSummary(std::function<ProviderSummary(const Json&)> summary);

    // The empty layout that ends the Welcome page's body (no margins, no spacing): the
    // OpenWhisper import row goes here (M3.7).
    QVBoxLayout* welcomeExtras() const { return welcomeExtras_; }
    // The Welcome page's "Bring over your OpenWhisper data" row, built the first time the import
    // is offered (until then the page is exactly as without it), shown while it is offered.
    // begin() calls it; call it again when the offer changes.
    void refreshImportRow();
    // The row, null until first offered, and its "Import…" button.
    QFrame* importRow() const { return importRow_; }
    QPushButton* importButton() const { return importButton_; }

    // --- Parts, for tests ---
    QWidget* stepRow() const { return stepRow_; }
    const std::vector<QPushButton*>& stepButtons() const { return stepButtons_; }
    QPushButton* allSettingsButton() const { return allSettingsButton_; }
    QPushButton* skipButton() const { return skipButton_; }
    QPushButton* backButton() const { return backButton_; }
    QPushButton* nextButton() const { return nextButton_; }
    WrappedLabel* errorLabel() const { return error_; }
    QScrollArea* scrollArea() const { return scroll_; }
    QStackedWidget* pages() const { return pages_; }
    ElidingComboBox* microphoneCombo() const { return microphone_; }
    WrappedLabel* microphoneDetail() const { return microphoneDetail_; }
    QPushButton* micTestButton() const { return micTestButton_; }
    MicrophoneCheck* micCheck() const { return micCheck_; }
    ProfileHotkeyInput* shortcutInput() const { return shortcut_; }
    WrappedLabel* shortcutDetail() const { return shortcutDetail_; }
    ElidingComboBox* triggerCombo() const { return trigger_; }
    QPushButton* speechDownloadButton() const { return speechDownloadButton_; }
    QPushButton* speechChangeButton() const { return speechButton_; }
    WrappedLabel* speechSummary() const { return speechSummary_; }
    QProgressBar* speechProgress() const { return speechProgress_; }
    SettingsSwitch* cleanupSwitch() const { return cleanupSwitch_; }
    WrappedLabel* cleanupDetail() const { return cleanupDetail_; }
    QWidget* cleanupOptions() const { return cleanupOptions_; }
    WrappedLabel* privacyLabel() const { return privacy_; }
    QPushButton* providerButton() const { return providerButton_; }
    WrappedLabel* providerDetail() const { return providerDetail_; }
    CleanupChoices* levels() const { return levels_; }
    WrappedLabel* levelNote() const { return levelNote_; }
    QFrame* preview() const { return preview_; }
    WrappedLabel* sampleResult() const { return sampleResult_; }
    WrappedLabel* sampleNote() const { return sampleNote_; }
    SettingsSwitch* pasteSwitch() const { return pasteSwitch_; }
    WrappedLabel* pasteDetail() const { return pasteDetail_; }
    SettingsSwitch* copySwitch() const { return copySwitch_; }
    SettingsSwitch* liveSwitch() const { return liveSwitch_; }
    WrappedLabel* readyTitle() const { return readyTitle_; }
    WrappedLabel* readyLede() const { return readyLede_; }
    QWidget* readySpeech() const { return readySpeech_; }
    WrappedLabel* readySpeechNote() const { return readySpeechNote_; }
    QPushButton* readyDownloadButton() const { return readyDownloadButton_; }
    QProgressBar* readyProgress() const { return readyProgress_; }
    QWidget* readyCleanup() const { return readyCleanup_; }
    WrappedLabel* readyCleanupNote() const { return readyCleanupNote_; }
    QLabel* keycap() const { return keycap_; }
    WrappedLabel* keycapHelp() const { return keycapHelp_; }
    QPlainTextEdit* tryBox() const { return tryBox_; }
    WrappedLabel* tryNote() const { return tryNote_; }
    QTimer* refreshTimer() const { return refreshTimer_; }
    QTimer* tryFillTimer() const { return tryFillTimer_; }

public Q_SLOTS:
    // Skip setup, Not now, Close, Escape and the window's close: saves "skipped" (never over a
    // finished setup) and closes; a failed save keeps the window open with the reason.
    void reject() override;

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    struct Row {
        QBoxLayout* layout;
        QWidget* control; // null for the preview card, which only changes direction
    };

    void setupUi();
    QVBoxLayout* addPage(const QString& title, const QString& lede, const char* titleName,
                         WrappedLabel** titleLabel = nullptr, WrappedLabel** ledeLabel = nullptr);
    QVBoxLayout* addCard(QVBoxLayout* column);
    WrappedLabel* addRow(QBoxLayout* column, const QString& title, const QString& detail,
                         QWidget* control);
    QProgressBar* makeProgressBar();
    QPushButton* addLinkRow(QVBoxLayout* column, const QString& text, std::function<void()> action);
    void buildWelcomePage();
    void buildRecordPage();
    void buildCleanUpPage();
    void buildOutputPage();
    void buildReadyPage();

    void queueRefresh();
    void refreshCleanup(const Json& settings);
    // The provider row and the privacy text show only with a provider summary (M4); returns
    // whether there is one.
    bool showProviderRow();
    void refreshSpeech();
    void refreshReady(const Json& settings, const QString& hotkey);
    void updateSpeechStatus();

    void sourceResult();
    void applySwitch(std::string_view destination, std::string_view control, std::string_view key,
                     bool value);
    void applyCleanupSwitch(bool value);
    void revealCleanupPrivacy();
    void chooseMicrophone();
    void toggleMicCheck();
    void chooseTrigger();
    void captureShortcut();
    void shortcutCaptured(const QString& hotkey);
    void chooseLevel(int index);
    void providerAction();
    void downloadSpeech();
    void downloadSettled();
    void fillTryBox();

    void focusPage();
    void back();
    void advance();
    bool saveOutcome(std::string_view state);
    bool saveProgress(std::string_view state);
    void allSettings();
    void open(std::string_view destination);
    void reflow();

    SettingsStore& store_;
    GuidedSetupPorts ports_;
    QPointer<SettingsDialog> settings_;

    // The speech download this window started: the model while it downloads, whether its
    // runtime is installing, and what the bars show.
    bool expectingDownload_ = false;
    QString requestedModel_;
    std::optional<QString> modelBusy_;
    bool runtimeBusy_ = false;
    std::optional<SpeechProgress> downloadProgress_;
    QString downloadError_;
    int page_ = Welcome;
    bool finished_ = false;
    std::optional<bool> speechReady_;
    bool providerNeedsKey_ = false;
    // (tone, text) once a dictation was tried on the Ready page.
    std::optional<std::pair<QString, QString>> tryResult_;
    QString triedText_;
    std::vector<Row> rows_;

    QTimer* refreshTimer_ = nullptr;
    QTimer* privacyRevealTimer_ = nullptr;
    QTimer* tryFillTimer_ = nullptr;

    QPushButton* allSettingsButton_ = nullptr;
    QWidget* stepRow_ = nullptr;
    std::vector<QPushButton*> stepButtons_;
    QScrollArea* scroll_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    WrappedLabel* error_ = nullptr;
    QPushButton* skipButton_ = nullptr;
    QPushButton* backButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    QVBoxLayout* welcomeExtras_ = nullptr;
    QFrame* importRow_ = nullptr;
    QPushButton* importButton_ = nullptr;

    ElidingComboBox* microphone_ = nullptr;
    QPushButton* micTestButton_ = nullptr;
    WrappedLabel* microphoneDetail_ = nullptr;
    MicrophoneCheck* micCheck_ = nullptr;
    ProfileHotkeyInput* shortcut_ = nullptr;
    WrappedLabel* shortcutDetail_ = nullptr;
    ElidingComboBox* trigger_ = nullptr;
    QPushButton* speechDownloadButton_ = nullptr;
    QPushButton* speechButton_ = nullptr;
    WrappedLabel* speechSummary_ = nullptr;
    QProgressBar* speechProgress_ = nullptr;

    SettingsSwitch* cleanupSwitch_ = nullptr;
    WrappedLabel* cleanupDetail_ = nullptr;
    QWidget* cleanupOptions_ = nullptr;
    WrappedLabel* privacy_ = nullptr;
    WrappedLabel* providerTitle_ = nullptr;
    QPushButton* providerButton_ = nullptr;
    WrappedLabel* providerDetail_ = nullptr;
    CleanupChoices* levels_ = nullptr;
    WrappedLabel* levelNote_ = nullptr;
    QFrame* preview_ = nullptr;
    WrappedLabel* sampleResult_ = nullptr;
    WrappedLabel* sampleNote_ = nullptr;

    SettingsSwitch* pasteSwitch_ = nullptr;
    WrappedLabel* pasteDetail_ = nullptr;
    SettingsSwitch* copySwitch_ = nullptr;
    SettingsSwitch* liveSwitch_ = nullptr;

    WrappedLabel* readyTitle_ = nullptr;
    WrappedLabel* readyLede_ = nullptr;
    QWidget* readySpeech_ = nullptr;
    WrappedLabel* readySpeechNote_ = nullptr;
    QPushButton* readyDownloadButton_ = nullptr;
    QProgressBar* readyProgress_ = nullptr;
    QWidget* readyCleanup_ = nullptr;
    WrappedLabel* readyCleanupNote_ = nullptr;
    QLabel* keycap_ = nullptr;
    WrappedLabel* keycapHelp_ = nullptr;
    QPlainTextEdit* tryBox_ = nullptr;
    WrappedLabel* tryNote_ = nullptr;
};

} // namespace fv
