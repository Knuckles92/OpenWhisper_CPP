#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QPointer>
#include <QString>
#include <QWidget>

#include "appkit/settings_page.h"
#include "base/json.h"
#include "hotkeys/capture.h"
#include "ui_hotkeys/hotkey_bindings.h"

class QComboBox;
class QPushButton;

namespace fv {

class Button;
class HotkeyCaptureInput;
class SettingsStore;
class SettingsSwitch;
class WrappedLabel;

namespace qt {
class QtUiExecutor;
}

// What the Hotkeys page needs from the Settings window around it (settings_dialog.py's
// message_label, _persist, select_destination and the on_* callbacks the page calls).
// Affinity: GUI thread.
class HotkeysPageHost {
public:
    virtual ~HotkeysPageHost() = default;

    // message_label.setText: the footer line.
    virtual void showMessage(const QString& message) = 0;
    // on_profile_hotkey_capture: pause the app's global hotkeys while a field captures (true)
    // and resume them (false).
    virtual void setCaptureSuspended(bool suspended) = 0;
    // on_hotkeys_changed: the full standard shortcut map (the defaults with every change over
    // them). The host saves it as the `hotkeys` setting and registers it with the app; what it
    // throws means the shortcuts were not saved, and the page shows "Couldn't save hotkeys:
    // <what()>".
    virtual void hotkeysChanged(const Json& hotkeys) = 0;
    // on_recording_trigger_mode_changed, after the mode was saved.
    virtual void triggerModeChanged(const QString& mode) = 0;
    // select_destination: a group's "Open …" link.
    virtual void openDestination(const QString& destination) = 0;
    // _persist(key, value): saves one setting; false when it was not saved (the host showed
    // why).
    virtual bool persist(std::string_view key, const Json& value) = 0;

protected:
    HotkeysPageHost() = default;
    HotkeysPageHost(const HotkeysPageHost&) = default;
    HotkeysPageHost& operator=(const HotkeysPageHost&) = default;
};

// Settings > Hotkeys (settings_dialog.py _build_hotkeys_page and its capture, conflict, save,
// clear and reset flow): the instruction card, the standard shortcuts by group with the
// record-mode card and the double-tap switch, Reset to defaults, then the profile, transform
// and action shortcuts. Clicking a standard field cancels any capture, shows kCaptureHint,
// pauses the app's hotkeys and starts a global capture; a result is used only when it belongs
// to the capture still running. The page reads the saved values when it is constructed and on
// loadSettings.
// Affinity: GUI thread.
class HotkeysPage : public QWidget, public SettingsPageModule {
    Q_OBJECT

public:
    // The page the standard rows are laid out on unless another asks for them (the Walkthroughs
    // page's, "walkthroughs").
    static constexpr std::string_view kHotkeysPage = "hotkeys";

    // Confirmations the page asks with a message box; tests answer them.
    struct Prompts {
        // "Reset hotkeys?": true for Yes.
        std::function<bool(QWidget* parent)> confirmReset;
        // "Hotkey Capture Failed" with the capture's message.
        std::function<void(QWidget* parent, const QString& message)> captureFailed;
    };

    HotkeysPage(SettingsStore& store, HotkeysPageHost& host, QWidget* parent = nullptr);
    ~HotkeysPage() override;

    // The capture a standard field starts; hotkeys::make_hotkey_capture unless set (tests pass
    // hotkeys::FakeCaptureFactory's).
    void setCaptureFactory(hotkeys::CaptureFactory factory);
    void setPrompts(Prompts prompts);

    // _hotkey_shortcut_row(key, title, description, optional=, page=): one more row for a standard
    // shortcut, on `page`. It shares this page's capture, conflict check, save and Reset to
    // defaults, shows the shortcuts the page holds, and follows every change to them; the caller
    // lays the card out. The Hotkeys page's own rows are the ones hotkeyInput and the other
    // accessors name. Not optional: Clear stays disabled, so the shortcut can be changed but never
    // left empty. The row's widgets may be destroyed before the page.
    QWidget* shortcutRow(const std::string& action, const QString& title,
                         const QString& description, bool optional,
                         std::string_view page = kHotkeysPage);
    // shortcut_input(key, page): the field of the row on `page`; null when there is none.
    HotkeyCaptureInput* shortcutInput(std::string_view action,
                                      std::string_view page = kHotkeysPage) const;
    // _load_hotkey_settings: reads the saved shortcuts again and shows them on every row, for a
    // page that built its rows after this one loaded.
    void reloadShortcuts();

    // set_standard_hotkey: saves one standard shortcut through the page's save path, starting
    // from the shortcuts the page holds (read first when it holds none), so a later edit here
    // keeps it. "" once saved, otherwise the reason it was not (also shown by the host).
    QString setStandardHotkey(const std::string& action, const std::string& hotkey);
    // set_hotkey_capture_suspended.
    void setHotkeyCaptureSuspended(bool suspended);
    // A profile, transform or action source (M4, M5); its rows show on the next load.
    void registerBindingSource(std::string kind, std::shared_ptr<BindingSource> source);

    // The rail's value: the record shortcut as format_hotkey_display shows it, or "Not set".
    static QString hotkeysRailValue(const Json& settings);

    // current_hotkeys: the standard shortcuts the page holds (defaults with the saved ones over
    // them); empty before the first read.
    const Json& currentHotkeys() const { return currentHotkeys_; }
    // The action whose field is capturing, if any.
    const std::optional<std::string>& capturingKey() const { return capturing_; }

    // The controls tests and deep links name: hotkey_inputs, hotkey_clear_buttons (optional
    // rows only), hotkey_row_descriptions, hotkey_row_notes.
    HotkeyCaptureInput* hotkeyInput(std::string_view action) const;
    Button* clearButton(std::string_view action) const;
    WrappedLabel* rowDescription(std::string_view action) const;
    WrappedLabel* rowNote(std::string_view action) const;
    QComboBox* recordModeCombo() const;
    SettingsSwitch* handsFreeLatchSwitch() const { return latchSwitch_; }
    QWidget* handsFreeLatchRow() const { return latchRow_; }
    BindingShortcuts* bindingShortcuts() const { return bindings_; }
    QPushButton* resetButton() const;

    // SettingsPageModule.
    void loadSettings(const Json& settings) override;
    void refreshPage() override;
    void cancelCapture() override;
    QWidget* namedControl(std::string_view name) override;

private:
    struct Row;
    struct CaptureRun;

    void buildInstructionCard();
    QWidget* buildRecordModeCard();

    Row* row(std::string_view action, std::string_view page = kHotkeysPage) const;
    void showRow(const Row& row);
    static void setRowNote(const Row& row, const QString& text, bool warning);
    bool persist(std::string_view key, const Json& value);
    void loadHotkeySettings();
    void updateHotkeyDisplays();
    void showHotkeyNote(const std::string& key, const QString& text, bool warning);
    void updateRecordRowDescription(const QString& mode);
    void loadHandsFreeLatch(const Json& settings);

    void startHotkeyCapture(const std::string& key, HotkeyCaptureInput* field);
    void suspendHotkeysForCapture(bool suspended);
    void onHotkeyCaptured(const CaptureRun* run, const std::string& hotkey);
    void onHotkeyCaptureCancelled(const CaptureRun* run);
    void onHotkeyCaptureFailed(const CaptureRun* run, const std::string& message);
    void onCaptureFinished(const CaptureRun* run);
    void onMouseHotkeyCaptured(const std::string& key, const QString& hotkey);
    void finishHotkeyCapture(const CaptureRun* run);
    void cancelHotkeyCapture();
    bool applyCapturedHotkey(const std::string& key, const std::string& hotkey);
    QString standardHotkeyConflict(const std::string& key, const std::string& hotkey) const;
    QString saveHotkeys(const Json& hotkeys, const QString& message);
    void clearHotkey(const std::string& key);
    void confirmResetHotkeys();
    void onRecordingTriggerModeChanged();
    void onHandsFreeLatchToggled(bool checked);
    void onBindingCaptureChanged(bool capturing);
    void onBindingShortcutSaved(const QString& kind, const QString& message);
    hotkeys::Bindings conflictBindings(const Json& settings) const;

    SettingsStore& store_;
    HotkeysPageHost& host_;
    std::unique_ptr<qt::QtUiExecutor> executor_;
    hotkeys::CaptureFactory captureFactory_;
    Prompts prompts_;

    std::vector<std::unique_ptr<Row>> rows_;
    WrappedLabel* instruction_ = nullptr;
    QComboBox* recordModeCombo_ = nullptr;
    QWidget* latchRow_ = nullptr;
    SettingsSwitch* latchSwitch_ = nullptr;
    Button* resetButton_ = nullptr;
    BindingShortcuts* bindings_ = nullptr;

    Json currentHotkeys_ = Json::object();
    bool loading_ = false;
    bool hotkeysSuspendedForCapture_ = false;
    std::optional<std::string> capturing_;
    QPointer<HotkeyCaptureInput> currentInput_;
    // capture_thread: the capture whose results count. Every capture stays alive until it has
    // finished (its own thread may still be emitting), in runs_.
    CaptureRun* run_ = nullptr;
    std::vector<std::unique_ptr<CaptureRun>> runs_;
};

} // namespace fv
