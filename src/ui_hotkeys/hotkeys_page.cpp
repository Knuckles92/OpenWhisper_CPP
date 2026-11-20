#include "ui_hotkeys/hotkeys_page.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/platform_info.h"
#include "base/subscription.h"
#include "hotkeys/conflicts.h"
#include "hotkeys/defaults.h"
#include "hotkeys/grammar.h"
#include "hotkeys/mouse_shortcuts.h"
#include "pyport/errors.h"
#include "settings/hotkeys.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_hotkeys/hotkey_capture.h"
#include "ui_hotkeys/hotkey_row.h"
#include "ui_hotkeys/standard_hotkeys.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_dialog"};

constexpr std::string_view kPushHold = "push_hold";

// config.DEFAULT_HOTKEYS for this platform, as a fresh object.
Json defaultHotkeys() {
    Json hotkeys = Json::object();
    for (const auto& [action, hotkey] : hotkeys::default_hotkeys()) {
        hotkeys[action] = hotkey;
    }
    return hotkeys;
}

// A saved shortcut as the page uses it: a string, or "" for anything else.
std::string hotkeyOf(const Json& hotkeys, std::string_view action) {
    if (!hotkeys.is_object()) {
        return "";
    }
    const auto found = hotkeys.find(action);
    if (found == hotkeys.end() || !found->is_string()) {
        return "";
    }
    return found->get<std::string>();
}

QString display(const std::string& hotkey) {
    return qt::toQString(hotkeys::format_hotkey_display(hotkey, hotkeys::platform_vocabulary()));
}

// Python's truth value of a JSON value.
bool truthy(const Json& value) {
    if (value.is_null()) {
        return false;
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number()) {
        return value.get<double>() != 0;
    }
    if (value.is_string() || value.is_array() || value.is_object()) {
        return !value.empty();
    }
    return true;
}

QString instructionText() {
    const std::string_view platform = python_platform();
    if (platform == "darwin") {
        return QStringLiteral(
            "Click a shortcut, hold any modifiers, then press its key. Control+Option "
            "combinations are less likely to conflict with macOS shortcuts.");
    }
    if (hotkeys::platform_vocabulary() == hotkeys::Vocabulary::Pynput) {
        return QStringLiteral(
            "Click a shortcut, hold Ctrl, Alt, Shift, or Super, then press the desired key. A "
            "side mouse button works too: click it on the shortcut.");
    }
    return QStringLiteral(
        "Click a shortcut, then press the desired key combination, or click a side mouse button "
        "on it. Numpad keys are distinct from the matching regular keys.");
}

QLabel* groupTitle(const QString& text) {
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("hotkeyGroupTitle"));
    return label;
}

} // namespace

// _ShortcutRow: a standard shortcut's field on this page, or on another page that asked for the
// row (shortcutRow). The widgets of another page may go before this one, so none is held bare.
struct HotkeysPage::Row {
    std::string key;
    std::string page;
    QPointer<HotkeyCaptureInput> field;
    QPointer<Button> clear; // null for a required row
    QPointer<WrappedLabel> note;
    QPointer<WrappedLabel> description;
};

// One started capture and its connections. The connections end first, then the capture.
struct HotkeysPage::CaptureRun {
    std::unique_ptr<hotkeys::HotkeyCapture> capture;
    Subscription captured;
    Subscription cancelled;
    Subscription failed;
    Subscription finished;
};

HotkeysPage::HotkeysPage(SettingsStore& store, HotkeysPageHost& host, QWidget* parent)
    : QWidget(parent), store_(store), host_(host),
      executor_(std::make_unique<qt::QtUiExecutor>(this)),
      captureFactory_([] { return hotkeys::make_hotkey_capture(); }) {
    prompts_.confirmReset = [](QWidget* parentWidget) {
        const QString text =
            QStringLiteral("Put ") + qt::toQString(identity().product) +
            QStringLiteral("'s own shortcuts back to the platform defaults? Profile, transform "
                           "and action shortcuts stay as they are.");
        return QMessageBox::question(parentWidget, QStringLiteral("Reset hotkeys?"), text,
                                     QMessageBox::Yes | QMessageBox::Cancel,
                                     QMessageBox::Cancel) == QMessageBox::Yes;
    };
    prompts_.captureFailed = [](QWidget* parentWidget, const QString& message) {
        QMessageBox::warning(parentWidget, QStringLiteral("Hotkey Capture Failed"), message);
    };

    // The Settings page body: _add_page's layout, then the page, then the closing stretch.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    buildInstructionCard();
    for (const auto& item : standard_hotkeys::items()) {
        switch (item.kind) {
        case standard_hotkeys::Item::Kind::Group:
            layout->addWidget(groupTitle(qt::toQString(item.title)));
            break;
        case standard_hotkeys::Item::Kind::RecordModeCard:
            layout->addWidget(buildRecordModeCard());
            break;
        case standard_hotkeys::Item::Kind::Row:
            layout->addWidget(shortcutRow(item.key, qt::toQString(item.title),
                                          qt::toQString(item.description), !item.required));
            break;
        }
    }
    resetButton_ = neutralButton(new Button(QStringLiteral("Reset to defaults")));
    resetButton_->setObjectName(QStringLiteral("hotkeyResetButton"));
    resetButton_->setToolTip(
        QStringLiteral("Put the shortcuts above back to the platform defaults"));
    fitCompactButton(resetButton_, 150);
    connect(resetButton_, &QPushButton::clicked, this, [this] { confirmResetHotkeys(); });
    layout->addWidget(resetButton_, 0, Qt::AlignRight);

    // The shortcuts people give their own profiles, transforms and actions, changed here or on
    // their own pages.
    bindings_ = new BindingShortcuts(store_);
    connect(bindings_, &BindingShortcuts::captureChanged, this,
            [this](bool capturing) { onBindingCaptureChanged(capturing); });
    connect(bindings_, &BindingShortcuts::saved, this,
            [this](const QString& kind, const QString& message) {
                onBindingShortcutSaved(kind, message);
            });
    connect(bindings_, &BindingShortcuts::openRequested, this,
            [this](const QString& destination) { host_.openDestination(destination); });
    layout->addWidget(bindings_);
    layout->addStretch();

    loadSettings(*store_.snapshot());
}

HotkeysPage::~HotkeysPage() {
    // Connections end before the captures they point into (CaptureRun's member order); a real
    // capture's destructor stops and joins its thread.
    run_ = nullptr;
    runs_.clear();
}

void HotkeysPage::buildInstructionCard() {
    auto* card = new QFrame();
    card->setObjectName(QStringLiteral("hotkeyInstructionCard"));
    auto* row = new QHBoxLayout(card);
    row->setContentsMargins(14, 9, 14, 9);
    row->setSpacing(12);
    auto* icon = new QLabel();
    icon->setObjectName(QStringLiteral("hotkeyInstructionIcon"));
    icon->setFixedSize(18, 18);
    icon->setPixmap(ui::designIcon(QStringLiteral("info-blue.svg")).pixmap(16, 16));
    row->addWidget(icon, 0, Qt::AlignTop);
    instruction_ = new WrappedLabel(instructionText());
    instruction_->setObjectName(QStringLiteral("hotkeyInstructionText"));
    row->addWidget(instruction_, 1);
    layout()->addWidget(card);
}

QWidget* HotkeysPage::shortcutRow(const std::string& key, const QString& title,
                                  const QString& description, bool optional,
                                  std::string_view page) {
    auto* card = new ReflowCard();
    card->setObjectName(QStringLiteral("hotkeyShortcutCard"));
    auto* name = new WrappedLabel(title);
    name->setObjectName(QStringLiteral("hotkeyShortcutName"));
    card->copyLayout()->addWidget(name);
    auto* detail = new WrappedLabel(description);
    detail->setObjectName(QStringLiteral("hotkeyShortcutDescription"));
    card->copyLayout()->addWidget(detail);
    auto* note = new WrappedLabel(QString());
    note->setObjectName(QStringLiteral("hotkeyShortcutNote"));
    note->setVisible(false);
    card->copyLayout()->addWidget(note);

    auto* field = new HotkeyCaptureInput();
    field->setAccessibleName(title + QStringLiteral(" shortcut"));
    connect(field, &HotkeyCaptureInput::captureRequested, this,
            [this, key, field] { startHotkeyCapture(key, field); });
    connect(field, &HotkeyCaptureInput::captureCancelled, this, [this] { cancelHotkeyCapture(); });
    connect(field, &HotkeyCaptureInput::mouseCaptured, this,
            [this, key](const QString& hotkey) { onMouseHotkeyCaptured(key, hotkey); });
    connect(field, &HotkeyCaptureInput::notice, this,
            [this, key](const QString& text) { showHotkeyNote(key, text, true); });
    // OpenWhisper also sets an `optional` property here, which no theme rule reads.
    field->setMinimumWidth(190);
    field->setMaximumWidth(220);
    card->addControl(field);

    // Every row gets a Clear, so the shortcut column lines up. A required shortcut's stays
    // disabled: it can be changed but never left empty.
    auto* button = new Button(QStringLiteral("Clear"));
    button->setObjectName(QStringLiteral("hotkeyClearButton"));
    button->setAccessibleName(QStringLiteral("Clear ") + title + QStringLiteral(" shortcut"));
    fitCompactButton(button, 68);
    card->addControl(button);
    Button* clear = nullptr;
    if (optional) {
        clear = button;
        connect(button, &QPushButton::clicked, this, [this, key] { clearHotkey(key); });
    } else {
        button->setEnabled(false);
        button->setToolTip(title +
                           QStringLiteral(" can't be empty. Click its shortcut to change it."));
    }
    std::erase_if(rows_, [](const auto& entry) { return entry->field == nullptr; });
    rows_.push_back(std::make_unique<Row>(Row{key, std::string(page), field, clear, note, detail}));
    // A row built after the page was loaded shows the shortcuts the page holds.
    if (!currentHotkeys_.empty()) {
        showRow(*rows_.back());
    }
    return card;
}

QWidget* HotkeysPage::buildRecordModeCard() {
    auto* combo = new ElidingComboBox();
    recordModeCombo_ = combo;
    combo->setObjectName(QStringLiteral("recordModeCombo"));
    combo->addItem(QStringLiteral("Toggle — press to start and stop"), QStringLiteral("toggle"));
    combo->addItem(QStringLiteral("Push and hold — release to stop"), QStringLiteral("push_hold"));
    combo->setMinimumHeight(40);
    combo->setMinimumWidth(ui::scaled(260));
    connect(combo, &QComboBox::currentIndexChanged, this,
            [this](int) { onRecordingTriggerModeChanged(); });
    auto* card = new ReflowCard();
    card->setObjectName(QStringLiteral("hotkeyModeCard"));
    auto* name = new WrappedLabel(QStringLiteral("How the record hotkey activates"));
    name->setObjectName(QStringLiteral("hotkeyShortcutName"));
    card->copyLayout()->addWidget(name);
    auto* detail = new WrappedLabel(QStringLiteral(
        "Toggle starts and stops with each press. Push and hold records while the key is down."));
    detail->setObjectName(QStringLiteral("hotkeyShortcutDescription"));
    card->copyLayout()->addWidget(detail);
    card->addControl(combo);

    latchRow_ = new QWidget();
    latchRow_->setObjectName(QStringLiteral("hotkeyLatchRow"));
    auto* latch = new QHBoxLayout(latchRow_);
    latch->setContentsMargins(0, 8, 0, 0);
    latch->setSpacing(16);
    auto* latchCopy = new QVBoxLayout();
    latchCopy->setSpacing(2);
    auto* latchName = new WrappedLabel(QStringLiteral("Double-tap to keep recording"));
    latchName->setObjectName(QStringLiteral("hotkeyShortcutName"));
    latchCopy->addWidget(latchName);
    auto* latchDetail = new WrappedLabel(
        QStringLiteral("Tap the record hotkey twice quickly to record hands-free, then press it "
                       "once more to stop."));
    latchDetail->setObjectName(QStringLiteral("hotkeyShortcutDescription"));
    latchCopy->addWidget(latchDetail);
    latch->addLayout(latchCopy, 1);
    latchSwitch_ = new SettingsSwitch();
    latchSwitch_->setAccessibleName(QStringLiteral("Double-tap to keep recording"));
    connect(latchSwitch_, &QAbstractButton::toggled, this,
            [this](bool checked) { onHandsFreeLatchToggled(checked); });
    latch->addWidget(latchSwitch_, 0, Qt::AlignVCenter);
    latchRow_->setVisible(false);
    card->addFooter(latchRow_);
    return card;
}

void HotkeysPage::setCaptureFactory(hotkeys::CaptureFactory factory) {
    captureFactory_ = std::move(factory);
}

void HotkeysPage::setPrompts(Prompts prompts) {
    prompts_ = std::move(prompts);
}

void HotkeysPage::registerBindingSource(std::string kind, std::shared_ptr<BindingSource> source) {
    bindings_->registerSource(std::move(kind), std::move(source));
}

hotkeys::Bindings HotkeysPage::conflictBindings(const Json& settings) const {
    return fv::conflictBindings(bindings_->sources(), settings);
}

HotkeysPage::Row* HotkeysPage::row(std::string_view action, std::string_view page) const {
    for (const auto& entry : rows_) {
        if (entry->key == action && entry->page == page && entry->field != nullptr) {
            return entry.get();
        }
    }
    return nullptr;
}

HotkeyCaptureInput* HotkeysPage::shortcutInput(std::string_view action,
                                               std::string_view page) const {
    const Row* entry = row(action, page);
    return entry != nullptr ? entry->field.data() : nullptr;
}

HotkeyCaptureInput* HotkeysPage::hotkeyInput(std::string_view action) const {
    return shortcutInput(action, kHotkeysPage);
}

Button* HotkeysPage::clearButton(std::string_view action) const {
    const Row* entry = row(action);
    return entry != nullptr ? entry->clear.data() : nullptr;
}

WrappedLabel* HotkeysPage::rowDescription(std::string_view action) const {
    const Row* entry = row(action);
    return entry != nullptr ? entry->description.data() : nullptr;
}

WrappedLabel* HotkeysPage::rowNote(std::string_view action) const {
    const Row* entry = row(action);
    return entry != nullptr ? entry->note.data() : nullptr;
}

QComboBox* HotkeysPage::recordModeCombo() const {
    return recordModeCombo_;
}

QPushButton* HotkeysPage::resetButton() const {
    return resetButton_;
}

QWidget* HotkeysPage::namedControl(std::string_view name) {
    if (name == "_hotkey_instruction" || name == "_hotkey_instruction_text") {
        return instruction_;
    }
    if (name == "record_mode_combo") {
        return recordModeCombo_;
    }
    return nullptr;
}

QString HotkeysPage::hotkeysRailValue(const Json& settings) {
    // load_hotkey_settings: the saved value whole, or the defaults when none is saved.
    Json hotkeys = defaultHotkeys();
    if (settings.is_object()) {
        if (const auto found = settings.find(keys::hotkeys.key()); found != settings.end()) {
            hotkeys = *found;
        }
    }
    const QString shown = display(hotkeyOf(hotkeys, "record_toggle"));
    return shown.isEmpty() ? QStringLiteral("Not set") : shown;
}

bool HotkeysPage::persist(std::string_view key, const Json& value) {
    if (loading_) {
        return false;
    }
    return host_.persist(key, value);
}

void HotkeysPage::loadSettings(const Json& settings) {
    const bool loading = loading_;
    loading_ = true;
    try {
        loadHotkeySettings();
        const QString mode = qt::toQString(keys::recording_trigger_mode.read(settings));
        // _select_combo_data: the entry carrying the mode, or the first entry.
        recordModeCombo_->setCurrentIndex(std::max(0, recordModeCombo_->findData(mode)));
        loadHandsFreeLatch(settings);
        bindings_->load(settings);
    } catch (...) {
        loading_ = loading;
        throw;
    }
    loading_ = loading;
}

void HotkeysPage::refreshPage() {
    bindings_->load(*store_.snapshot());
}

void HotkeysPage::cancelCapture() {
    cancelHotkeyCapture();
}

void HotkeysPage::loadHotkeySettings() {
    Json hotkeys = defaultHotkeys();
    try {
        const Json saved = load_hotkey_settings(store_);
        if (truthy(saved)) {
            if (!saved.is_object()) {
                throw py::TypeError("the saved hotkeys are not a mapping");
            }
            for (const auto& [action, value] : saved.items()) {
                hotkeys[action] = value;
            }
        }
    } catch (const std::exception& error) {
        kLog.warning("Couldn't load hotkeys: {}; using the defaults", error.what());
        hotkeys = defaultHotkeys();
    }
    currentHotkeys_ = std::move(hotkeys);
    updateHotkeyDisplays();
}

void HotkeysPage::updateHotkeyDisplays() {
    for (const auto& entry : rows_) {
        showRow(*entry);
    }
}

void HotkeysPage::showRow(const Row& entry) {
    if (entry.field == nullptr) {
        return;
    }
    const std::string hotkey = hotkeyOf(currentHotkeys_, entry.key);
    entry.field->setText(display(hotkey));
    setRowNote(entry, qt::toQString(hotkeys::mouse_shortcut_note(hotkey)), false);
    if (entry.clear != nullptr) {
        entry.clear->setEnabled(!hotkey.empty());
    }
}

void HotkeysPage::setRowNote(const Row& entry, const QString& text, bool warning) {
    if (entry.note == nullptr) {
        return;
    }
    entry.note->setText(text);
    ui::setStyleProperty(entry.note.data(), ui::prop::tone,
                         warning ? QStringLiteral("warning") : QStringLiteral("info"));
    entry.note->setVisible(!text.isEmpty());
}

void HotkeysPage::showHotkeyNote(const std::string& key, const QString& text, bool warning) {
    for (const auto& entry : rows_) {
        if (entry->key == key) {
            setRowNote(*entry, text, warning);
        }
    }
}

void HotkeysPage::reloadShortcuts() {
    loadHotkeySettings();
}

void HotkeysPage::updateRecordRowDescription(const QString& mode) {
    if (WrappedLabel* label = rowDescription("record_toggle")) {
        label->setText(qt::toQString(standard_hotkeys::recordHotkeyDescription(
            qt::toUtf8(mode), latchSwitch_->isChecked())));
    }
}

void HotkeysPage::loadHandsFreeLatch(const Json& settings) {
    const std::string mode = keys::recording_trigger_mode.read(settings);
    {
        const QSignalBlocker blocker(latchSwitch_);
        latchSwitch_->setChecked(keys::recording_hands_free_latch.read(settings));
    }
    latchRow_->setVisible(mode == kPushHold);
    updateRecordRowDescription(qt::toQString(mode));
}

void HotkeysPage::onRecordingTriggerModeChanged() {
    const QVariant current = recordModeCombo_->currentData();
    if (!current.isValid()) {
        return;
    }
    const QString mode = current.toString();
    latchRow_->setVisible(qt::toUtf8(mode) == kPushHold);
    if (!persist(keys::recording_trigger_mode.key(), qt::toUtf8(mode))) {
        return;
    }
    updateRecordRowDescription(mode);
    host_.triggerModeChanged(mode);
}

void HotkeysPage::onHandsFreeLatchToggled(bool checked) {
    // The hotkey runtime reads this at each short tap, so saving is enough.
    if (persist(keys::recording_hands_free_latch.key(), checked)) {
        updateRecordRowDescription(recordModeCombo_->currentData().toString());
    }
}

void HotkeysPage::setHotkeyCaptureSuspended(bool suspended) {
    host_.setCaptureSuspended(suspended);
}

void HotkeysPage::suspendHotkeysForCapture(bool suspended) {
    if (suspended == hotkeysSuspendedForCapture_) {
        return;
    }
    hotkeysSuspendedForCapture_ = suspended;
    setHotkeyCaptureSuspended(suspended);
}

void HotkeysPage::startHotkeyCapture(const std::string& key, HotkeyCaptureInput* field) {
    cancelHotkeyCapture();
    bindings_->cancelCapture();
    capturing_ = key;
    currentInput_ = field;
    field->setText(QString::fromUtf16(kCaptureHint));
    field->setCapturing(true);
    // A key or side button that is already bound would fire its action (or be swallowed by the
    // hook) instead of reaching the capture.
    suspendHotkeysForCapture(true);

    auto run = std::make_unique<CaptureRun>();
    run->capture = captureFactory_();
    const CaptureRun* id = run.get();
    hotkeys::HotkeyCapture& capture = *run->capture;
    run->captured = capture.captured.connect(
        *executor_, [this, id](const std::string& hotkey) { onHotkeyCaptured(id, hotkey); });
    run->cancelled =
        capture.cancelled.connect(*executor_, [this, id] { onHotkeyCaptureCancelled(id); });
    run->failed = capture.failed.connect(
        *executor_, [this, id](const std::string& message) { onHotkeyCaptureFailed(id, message); });
    run->finished = capture.finished.connect(*executor_, [this, id] { onCaptureFinished(id); });
    run_ = run.get();
    runs_.push_back(std::move(run));
    kLog.info("Capturing hotkey for {}", key);
    capture.start();
}

void HotkeysPage::onCaptureFinished(const CaptureRun* run) {
    // thread.finished -> deleteLater: the capture may still be inside its own emit, so it goes
    // once control is back in the event loop.
    qt::post(this, [this, run] {
        if (run_ == run) {
            return; // still the running capture's; a later finish removes it
        }
        std::erase_if(runs_, [run](const auto& entry) { return entry.get() == run; });
    });
}

void HotkeysPage::onHotkeyCaptured(const CaptureRun* run, const std::string& hotkey) {
    if (run != run_ || !capturing_) {
        return;
    }
    const auto parsed = hotkeys::parse_hotkey(hotkey, hotkeys::platform_vocabulary());
    if (hotkeys::is_escape_key(parsed.main_key ? std::optional<std::string_view>(*parsed.main_key)
                                               : std::nullopt) &&
        parsed.modifiers.empty()) {
        cancelHotkeyCapture();
        return;
    }
    const std::string key = *capturing_;
    finishHotkeyCapture(run);
    applyCapturedHotkey(key, hotkey);
}

void HotkeysPage::onHotkeyCaptureCancelled(const CaptureRun* run) {
    if (run != run_) {
        return;
    }
    cancelHotkeyCapture();
}

void HotkeysPage::onMouseHotkeyCaptured(const std::string& key, const QString& hotkey) {
    if (capturing_ != key) {
        return;
    }
    cancelHotkeyCapture();
    applyCapturedHotkey(key, qt::toUtf8(hotkey));
}

void HotkeysPage::onHotkeyCaptureFailed(const CaptureRun* run, const std::string& message) {
    if (run != run_) {
        return;
    }
    kLog.warning("{}", message);
    finishHotkeyCapture(run);
    updateHotkeyDisplays();
    if (prompts_.captureFailed) {
        prompts_.captureFailed(this, qt::toQString(message));
    }
}

void HotkeysPage::finishHotkeyCapture(const CaptureRun* run) {
    if (currentInput_ != nullptr) {
        currentInput_->setCapturing(false);
    }
    capturing_.reset();
    currentInput_ = nullptr;
    if (run == run_) {
        run_ = nullptr;
    }
    suspendHotkeysForCapture(false);
}

void HotkeysPage::cancelHotkeyCapture() {
    if (CaptureRun* run = run_) {
        run->captured.disconnect();
        run->cancelled.disconnect();
        run->failed.disconnect();
        if (run->capture->running()) {
            run->capture->stop();
            run->capture->wait(std::chrono::milliseconds(1000));
        }
    }
    if (currentInput_ != nullptr) {
        currentInput_->setCapturing(false);
    }
    run_ = nullptr;
    capturing_.reset();
    currentInput_ = nullptr;
    suspendHotkeysForCapture(false);
    updateHotkeyDisplays();
}

bool HotkeysPage::applyCapturedHotkey(const std::string& key, const std::string& hotkey) {
    QString conflict;
    try {
        conflict = standardHotkeyConflict(key, hotkey);
    } catch (const std::exception& error) {
        kLog.error_with(error, "Couldn't check the shortcut for conflicts");
        return false;
    }
    if (!conflict.isEmpty()) {
        host_.showMessage(conflict);
        updateHotkeyDisplays();
        showHotkeyNote(key, conflict, true);
        return false;
    }
    Json updated = currentHotkeys_;
    updated[key] = hotkey;
    return saveHotkeys(updated, qt::toQString(standard_hotkeys::label(key)) +
                                    QStringLiteral(" hotkey updated."))
        .isEmpty();
}

QString HotkeysPage::standardHotkeyConflict(const std::string& key,
                                            const std::string& hotkey) const {
    const auto settings = store_.snapshot();
    hotkeys::ConflictOptions options;
    options.exclude = {std::string(hotkeys::kStandard), key};
    options.standard_hotkeys = currentHotkeys_;
    options.bindings = conflictBindings(*settings);
    const std::string conflict = hotkeys::hotkey_conflict(hotkey, *settings, options);
    if (conflict.empty()) {
        return {};
    }
    return QStringLiteral("That shortcut is already used by ") + qt::toQString(conflict) +
           QStringLiteral(".");
}

QString HotkeysPage::saveHotkeys(const Json& hotkeys, const QString& message) {
    Json updated = defaultHotkeys();
    for (const auto& [action, value] : hotkeys.items()) {
        updated[action] = value;
    }
    try {
        const auto settings = store_.snapshot();
        const auto& sources = bindings_->sources();
        if (const auto found = sources.find(hotkeys::kProfile);
            found != sources.end() && found->second) {
            const hotkeys::Bindings all = conflictBindings(*settings);
            for (const auto& profile : found->second->list(*settings)) {
                hotkeys::ConflictOptions options;
                options.exclude = {std::string(hotkeys::kProfile), profile.id};
                options.standard_hotkeys = updated;
                options.bindings = all;
                const std::string conflict =
                    hotkeys::hotkey_conflict(profile.hotkey, *settings, options);
                if (!conflict.empty()) {
                    throw py::ValueError(profile.name + "'s shortcut is already used by " +
                                         conflict + ". Choose another.");
                }
            }
        }
        host_.hotkeysChanged(updated);
    } catch (const std::exception& error) {
        // A ValueError is a shortcut clash or a refused shortcut, shown to the user; anything
        // else is unexpected.
        if (dynamic_cast<const py::ValueError*>(&error) != nullptr) {
            kLog.error("Couldn't save hotkeys: {}", error.what());
        } else {
            kLog.error_with(error, "Couldn't save hotkeys");
        }
        const QString shown =
            QStringLiteral("Couldn't save hotkeys: ") + qt::toQString(error.what());
        host_.showMessage(shown);
        updateHotkeyDisplays();
        return shown;
    }
    currentHotkeys_ = std::move(updated);
    updateHotkeyDisplays();
    // A profile's, transform's or action's shortcut may now clash with one.
    bindings_->load(*store_.snapshot());
    host_.showMessage(message);
    return {};
}

QString HotkeysPage::setStandardHotkey(const std::string& action, const std::string& hotkey) {
    if (currentHotkeys_.empty()) {
        loadHotkeySettings();
    }
    Json updated = currentHotkeys_;
    updated[action] = hotkey;
    // Blank the action itself so its own default never reads as a conflict.
    Json standard = updated;
    standard[action] = "";
    const auto settings = store_.snapshot();
    hotkeys::ConflictOptions options;
    options.exclude = {std::string(hotkeys::kProfile), ""};
    options.standard_hotkeys = standard;
    options.bindings = conflictBindings(*settings);
    const std::string conflict = hotkeys::hotkey_conflict(hotkey, *settings, options);
    if (!conflict.empty()) {
        const QString error = QStringLiteral("That shortcut is already used by ") +
                              qt::toQString(conflict) + QStringLiteral(".");
        host_.showMessage(error);
        return error;
    }
    return saveHotkeys(updated, qt::toQString(standard_hotkeys::label(action)) +
                                    QStringLiteral(" hotkey updated."));
}

void HotkeysPage::clearHotkey(const std::string& key) {
    cancelHotkeyCapture();
    Json updated = currentHotkeys_;
    updated[key] = "";
    saveHotkeys(updated,
                qt::toQString(standard_hotkeys::label(key)) + QStringLiteral(" hotkey cleared."));
}

void HotkeysPage::confirmResetHotkeys() {
    cancelHotkeyCapture();
    if (prompts_.confirmReset && prompts_.confirmReset(this)) {
        saveHotkeys(defaultHotkeys(), QStringLiteral("Hotkeys reset to platform defaults."));
    }
}

void HotkeysPage::onBindingCaptureChanged(bool capturing) {
    if (capturing) {
        // A standard row's capture would take the same keys.
        cancelHotkeyCapture();
    }
    setHotkeyCaptureSuspended(capturing);
}

void HotkeysPage::onBindingShortcutSaved(const QString& kind, const QString& message) {
    host_.showMessage(message);
    const auto& sources = bindings_->sources();
    if (const auto found = sources.find(qt::toUtf8(kind));
        found != sources.end() && found->second) {
        found->second->saved();
    }
}

} // namespace fv
