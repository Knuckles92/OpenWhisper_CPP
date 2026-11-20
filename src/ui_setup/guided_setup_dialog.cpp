#include "ui_setup/guided_setup_dialog.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <memory>
#include <string>
#include <tuple>

#include <QAbstractButton>
#include <QApplication>
#include <QBoxLayout>
#include <QButtonGroup>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "appcore/guided_setup_state.h"
#include "appcore/mic_priority.h"
#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "base/log.h"
#include "history/format_utils.h"
#include "hotkeys/grammar.h"
#include "hotkeys/mouse_shortcuts.h"
#include "models/runtime_resolver.h"
#include "recorder/recorder.h"
#include "settings/hotkeys.h"
#include "settings/keys.h"
#include "settings/resolve.h"
#include "settings/store.h"
#include "ui_downloads/model_assignments.h"
#include "ui_hotkeys/hotkeys_page.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_personalize/cleanup_settings.h"
#include "ui_settings/settings_dialog.h"
#include "ui_setup/cleanup_provider_row.h"
#include "ui_theme/app_icon.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/icons.h"
#include "ui_theme/local_style.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.guided_setup"};

namespace dest = destinations;
namespace setup_state = appcore::guided_setup_state;

// Properties only this window's own stylesheet selects on (QPushButton#guidedStep[done="true"],
// QLabel#guidedPrivacy[remote="true"]); theme.qss has no rule for them.
constexpr const char* kDoneProperty = "done";
constexpr const char* kRemoteProperty = "remote";

constexpr std::string_view kSteps[] = {"Record", "Clean up", "Output"};

// _STYLE: the window's own sheet, over the slate tokens.
const QString& windowStyle() {
    static const QString style = QStringLiteral(uR"(
QDialog#guidedSetupDialog { background: @slate-bg; }
QWidget#guidedPage, QWidget#guidedCanvas, QWidget#guidedGroup { background: transparent; }
QFrame#guidedHeader { background: @slate-field; border: none; border-bottom: 1px solid @slate-border; }
QFrame#guidedFooter { background: transparent; border: none; border-top: 1px solid @slate-border-subtle; }
QFrame#guidedCard, QFrame#guidedPreview {
    background: @slate-field; border: 1px solid @slate-border; border-radius: 12px;
}
QLabel#guidedBrand { color: @slate-text; font-size: 15px; font-weight: 600; border: none; background: transparent; }
QLabel#guidedHeroTitle { color: @slate-text; font-size: 28px; font-weight: 600; border: none; background: transparent; }
QLabel#guidedTitle { color: @slate-text; font-size: 22px; font-weight: 600; border: none; background: transparent; }
QLabel#guidedLede { color: @slate-text-2; font-size: 14px; border: none; background: transparent; }
QLabel#guidedRowTitle { color: @slate-text; font-size: 14px; font-weight: 600; border: none; background: transparent; }
QLabel#guidedDetail { color: @slate-text-3; font-size: 13px; border: none; background: transparent; }
QLabel#guidedDetail[tone="ready"] { color: @success-text; }
QLabel#guidedDetail[tone="attention"] { color: @warning-text; }
QLabel#guidedSample { color: @slate-text; font-size: 17px; border: none; background: transparent; }
QLabel#guidedPrivacy {
    color: @slate-text-2; background: @slate-raised; border: 1px solid @slate-border;
    border-radius: 8px; padding: 10px 12px; font-size: 13px;
}
QLabel#guidedPrivacy[remote="true"] {
    color: @warning-text; background: @warning-tint; border-color: @warning-tint-border;
}
QLabel#guidedError { color: @danger-text; font-size: 13px; background: transparent; border: none; }
QPushButton#guidedStep {
    color: @slate-text-3; padding: 8px 6px; border: none; border-bottom: 2px solid @slate-border;
    border-radius: 0; font-size: 13px; background: transparent;
}
QPushButton#guidedStep:hover { color: @slate-text; }
QPushButton#guidedStep:focus { color: @slate-text; border-bottom-color: @accent-hover; }
QPushButton#guidedStep[active="true"] { color: @accent; border-color: @accent; font-weight: 600; }
QPushButton#guidedStep[done="true"] { color: @slate-text-2; border-color: @accent; }
QPushButton#guidedStep[done="true"]:hover { color: @slate-text; }
QProgressBar#microphoneCheckBar, QProgressBar#guidedProgress {
    background: @slate-raised; border: none; border-radius: 4px;
}
QProgressBar#microphoneCheckBar::chunk, QProgressBar#guidedProgress::chunk {
    background: @accent; border-radius: 4px;
}
QLabel#guidedKeycap {
    color: @slate-text; background: @slate-raised; border: 1px solid @slate-border-strong;
    border-bottom-width: 3px; border-radius: 8px; padding: 6px 16px; font-size: 22px; font-weight: 600;
}
QPlainTextEdit#guidedTry {
    color: @slate-text; background: @slate-field; border: 1px solid @slate-border;
    border-radius: 10px; padding: 10px 12px; font-size: 15px;
}
QPlainTextEdit#guidedTry:focus { border-color: @accent; }
QPushButton#guidedLink { color: @accent; background: transparent; border: none; padding: 6px 8px; font-size: 13px; }
QPushButton#guidedLink:hover { color: @accent-hover; text-decoration: underline; }
QPushButton#guidedLink:focus { border: 1px solid @accent; border-radius: 5px; }
QScrollArea#guidedScroll { background: transparent; border: none; }
)");
    return style;
}

QString product() {
    return qt::toQString(identity().product);
}

QString text(std::string_view utf8) {
    return qt::toQString(utf8);
}

// _label: a plain-text WrappedLabel with an object name.
WrappedLabel* label(const QString& content, const char* name = "guidedDetail") {
    auto* made = new WrappedLabel(content);
    made->setTextFormat(Qt::PlainText);
    made->setObjectName(QString::fromLatin1(name));
    return made;
}

// _button: a link, or a footer-style button in the shared Settings and dialog look.
QPushButton* button(const QString& content, std::function<void()> action,
                    const char* name = "guidedButton") {
    QPushButton* made = nullptr;
    if (std::string_view(name) == "guidedLink") {
        made = new QPushButton(content);
    } else if (std::string_view(name) == "guidedPrimary") {
        made = compactPrimaryButton(new Button(content));
    } else {
        made = neutralButton(new Button(content));
    }
    made->setObjectName(QString::fromLatin1(name));
    made->setCursor(Qt::PointingHandCursor);
    // Enter presses the focused button, and Continue from anywhere else.
    made->setAutoDefault(true);
    QObject::connect(made, &QPushButton::clicked, made, [action = std::move(action)] { action(); });
    return made;
}

// _group: a transparent widget holding a box layout without margins.
template <class Layout> std::pair<QWidget*, Layout*> group(int spacing = 12) {
    auto* widget = new QWidget();
    widget->setObjectName(QStringLiteral("guidedGroup"));
    auto* layout = new Layout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(spacing);
    return {widget, layout};
}

// Button refits its width to a new label only through its own setText.
void setLabel(QPushButton* target, const QString& content) {
    if (auto* fitted = qobject_cast<Button*>(target)) {
        fitted->setText(content);
    } else {
        target->setText(content);
    }
}

std::string recordHotkey(const SettingsStore& store) {
    const Json hotkeys = load_hotkey_settings(store);
    if (hotkeys.is_object()) {
        if (const auto found = hotkeys.find("record_toggle");
            found != hotkeys.end() && found->is_string()) {
            return found->get<std::string>();
        }
    }
    return {};
}

std::string displayHotkey(std::string_view hotkey) {
    return hotkeys::format_hotkey_display(hotkey, hotkeys::platform_vocabulary());
}

} // namespace

// --- The pure parts ---

bool on_number_pad(std::string_view hotkey) {
    return hotkey.starts_with("kp ") || hotkey.find("+kp ") != std::string_view::npos;
}

std::string shortcut_help(std::string_view hotkey, const Json& settings) {
    std::string help;
    if (keys::recording_trigger_mode.read(settings) == "push_hold") {
        help = "Hold it while you speak, then let go.";
        if (keys::recording_hands_free_latch.read(settings)) {
            help += " Double-tap to keep recording hands-free.";
        }
    } else {
        help = "Press it to start dictating, and again to stop.";
    }
    if (on_number_pad(hotkey)) {
        help += " It's on the number pad.";
    }
    const std::string note = hotkeys::mouse_shortcut_note(hotkey);
    if (!note.empty()) {
        help += " " + note;
    }
    return help;
}

std::string download_progress_text(std::int64_t done, std::int64_t total) {
    if (total > 0) {
        return std::format("Downloading… {} of {}", format_size_bytes(done),
                           format_size_bytes(total));
    }
    return "Downloading…";
}

SpeechStatus speech_status(std::string_view summary, std::string_view detail,
                           std::optional<bool> ready, const std::optional<SpeechProgress>& progress,
                           std::string_view error) {
    SpeechStatus status;
    const bool missing = ready.has_value() && !*ready;
    if (progress) {
        status.bar_maximum = progress->total > 0 ? 100 : 0;
        if (progress->total > 0) {
            // int(done * 100 / total): true division, truncated.
            status.bar_value = static_cast<int>(static_cast<double>(progress->done) * 100.0 /
                                                static_cast<double>(progress->total));
        }
        status.summary_line = std::format("{} · {}", summary, progress->text);
        status.ready_note = std::format("{}: {}", summary, progress->text);
    } else if (!ready.has_value()) {
        status.summary_line = std::format("{} · {}", summary, detail);
    } else if (*ready) {
        status.summary_line = std::format("{} · Downloaded and ready", summary);
        status.tone = "ready";
    } else {
        const std::string problem =
            !error.empty()
                ? std::string(error)
                : "Not downloaded yet. Download it now, or it's offered when you first dictate.";
        status.summary_line = std::format("{} · {}", summary, problem);
        status.ready_note = !error.empty()
                                ? std::format("{}: {}", summary, error)
                                : std::format("{} isn't downloaded yet, so your first dictation "
                                              "starts with the download.",
                                              summary);
        status.tone = "attention";
    }
    status.progress_visible = progress.has_value();
    status.record_download_visible = missing && !progress;
    status.ready_download_visible = !progress;
    status.ready_row_visible = missing || progress.has_value();
    const bool waiting = missing || progress.has_value();
    status.ready_title = waiting ? "Almost ready" : "You're ready";
    status.ready_lede =
        waiting ? "One thing left before your first dictation." : "Give it a try before you go.";
    return status;
}

// --- CleanupPort ---

namespace {

// _apply_switch(CLEANUP, "transcript_cleanup_check", ...): the AI cleanup page's switch, resynced
// from the saved settings first (a previous failed save must allow the same choice again) and
// after (a refused save leaves it as saved); its own handler saves, says cleanup changed and
// sets the page's dependent tiles. True when the setting now holds `enabled`.
bool setCleanupSwitch(SettingsDialog& dialog, CleanupSettings& cleanup, bool enabled) {
    dialog.ensurePage(dest::cleanup);
    const auto saved = [&dialog] {
        return resolve_bool_setting(keys::transcript_cleanup_enabled.key(),
                                    *dialog.settingsSnapshot());
    };
    auto* source = qobject_cast<QAbstractButton*>(dialog.control("transcript_cleanup_check"));
    if (source == nullptr) {
        cleanup.setCleanupEnabled(enabled, &dialog);
        return saved() == enabled;
    }
    {
        const QSignalBlocker blocker(source);
        source->setChecked(saved());
    }
    if (source->isEnabled()) {
        source->setChecked(enabled);
    }
    {
        const QSignalBlocker blocker(source);
        source->setChecked(saved());
    }
    return saved() == enabled;
}

} // namespace

CleanupPort CleanupPort::settingsDefault(std::function<bool(QWidget*, const QString&)> confirm) {
    if (!confirm) {
        // _confirm_custom_prompt_removal.
        confirm = [](QWidget* parent, const QString& level) {
            const auto& segments = CleanupChoices::segments();
            const int index = CleanupChoices::segmentIndex(qt::toUtf8(level));
            const QString name =
                QString::fromUtf8(segments[static_cast<std::size_t>(index < 3 ? index : 1)].title);
            const auto reply = QMessageBox::question(
                parent, QStringLiteral("Remove your custom prompt?"),
                QStringLiteral("AI cleanup will go back to the %1 level's instructions. Your "
                               "learned rules stay.")
                    .arg(name),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            return reply == QMessageBox::Yes;
        };
    }
    CleanupPort port;
    // cleanup_prompts.custom_prompt: a blank prompt, or a saved copy of a built-in one, is none.
    port.customPrompt = [](const Json& settings) {
        return customCleanupPrompt(settings);
    };
    // The AI cleanup page's switch when the app has its handlers (CleanupSettings), as
    // _apply_switch drives transcript_cleanup_check; a Settings window without them saves the
    // key itself.
    port.setEnabled = [](SettingsDialog& dialog, bool enabled) {
        if (CleanupSettings* cleanup = dialog.context().find<CleanupSettings>()) {
            return setCleanupSwitch(dialog, *cleanup, enabled);
        }
        return dialog.persist(keys::transcript_cleanup_enabled.key(), enabled);
    };
    port.updatePromptUi = [](SettingsDialog& dialog) {
        if (CleanupSettings* cleanup = dialog.context().find<CleanupSettings>()) {
            cleanup->updateCleanupUi(&dialog);
        }
    };
    port.chooseLevel = [confirm, custom = port.customPrompt](SettingsDialog& dialog,
                                                             const QString& level) {
        // The AI cleanup page's own choice, its custom-prompt question included.
        if (CleanupSettings* cleanup = dialog.context().find<CleanupSettings>()) {
            return cleanup->chooseCleanupLevel(level, &dialog);
        }
        std::vector<std::string> drops;
        if (!custom(*dialog.settingsSnapshot()).empty()) {
            if (!confirm(&dialog, level)) {
                return false;
            }
            drops.emplace_back(keys::transcript_cleanup_prompt.key());
        }
        Json updates = Json::object();
        updates[std::string(keys::transcript_cleanup_level.key())] = qt::toUtf8(level);
        return dialog.persistMany(updates, std::move(drops));
    };
    return port;
}

// --- CleanupChoices ---

const std::vector<CleanupChoices::Segment>& CleanupChoices::segments() {
    static const std::vector<Segment> all{
        {"light", "Light", "Punctuation & fillers"},
        {"medium", "Medium", "+ corrections & lists"},
        {"high", "High", "+ tighter wording"},
        {"custom", "Custom", "Your own prompt"},
    };
    return all;
}

int CleanupChoices::segmentIndex(std::string_view level) {
    const auto& all = segments();
    for (std::size_t index = 0; index < all.size(); ++index) {
        if (level == all[index].value) {
            return static_cast<int>(index);
        }
    }
    return 1;
}

namespace {

std::vector<std::pair<QString, QString>> presetTiles() {
    std::vector<std::pair<QString, QString>> tiles;
    for (std::size_t index = 0; index < 3; ++index) {
        const auto& segment = CleanupChoices::segments()[index];
        tiles.emplace_back(QString::fromUtf8(segment.title), QString::fromUtf8(segment.detail));
    }
    return tiles;
}

} // namespace

CleanupChoices::CleanupChoices(QWidget* parent) : SegmentedBar(presetTiles(), false, parent) {}

void CleanupChoices::setChoice(const QString& level, bool custom) {
    custom_ = custom;
    if (custom) {
        QButtonGroup* buttons = this->buttons().front()->group();
        buttons->setExclusive(false);
        for (SegmentButton* tile : this->buttons()) {
            tile->setChecked(false);
        }
        buttons->setExclusive(true);
        return;
    }
    const int index = segmentIndex(qt::toUtf8(level));
    setCurrentIndex(index);
    // The bar keeps the index a custom prompt unchecked, so checking it again is this call's.
    if (index >= 0 && index < static_cast<int>(this->buttons().size())) {
        this->buttons()[static_cast<std::size_t>(index)]->setChecked(true);
    }
}

// --- GuidedSetupDialog ---

GuidedSetupDialog::GuidedSetupDialog(SettingsStore& store, GuidedSetupPorts ports, QWidget* parent)
    : QDialog(parent), store_(store), ports_(std::move(ports)) {
    if (!ports_.cleanup.setEnabled || !ports_.cleanup.chooseLevel || !ports_.cleanup.customPrompt) {
        const CleanupPort fallback = CleanupPort::settingsDefault();
        if (!ports_.cleanup.setEnabled) {
            ports_.cleanup.setEnabled = fallback.setEnabled;
        }
        if (!ports_.cleanup.chooseLevel) {
            ports_.cleanup.chooseLevel = fallback.chooseLevel;
        }
        if (!ports_.cleanup.customPrompt) {
            ports_.cleanup.customPrompt = fallback.customPrompt;
        }
    }
    if (!ports_.isRuntime) {
        ports_.isRuntime = [](const QString& componentId) {
            const auto ids = models::runtime_ids();
            return std::ranges::find(ids, qt::toUtf8(componentId)) != ids.end();
        };
    }
    setObjectName(QStringLiteral("guidedSetupDialog"));
    setWindowTitle(QStringLiteral("Set up ") + product());
    setWindowIcon(ui::appIcon());
    setModal(false);
    // Dictation and the in-app shortcut fallback treat it as the app's own window, so the
    // try-it box on the last page takes dictation.
    ui::setStyleProperty(this, ui::prop::fiorivoceHotkeyWindow, true);
    setMinimumSize(kMinimumSize);
    resize(kDefaultSize);
    setSizeGripEnabled(true);
    ui::setLocalStyleSheet(this, windowStyle());
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    connect(refreshTimer_, &QTimer::timeout, this, [this] { refresh(); });
    privacyRevealTimer_ = new QTimer(this);
    privacyRevealTimer_->setSingleShot(true);
    connect(privacyRevealTimer_, &QTimer::timeout, this, [this] { revealCleanupPrivacy(); });
    tryFillTimer_ = new QTimer(this);
    tryFillTimer_->setSingleShot(true);
    connect(tryFillTimer_, &QTimer::timeout, this, [this] { fillTryBox(); });
    setupUi();
    showPage(Welcome);
    showProviderRow();
}

GuidedSetupDialog::~GuidedSetupDialog() = default;

SettingsDialog* GuidedSetupDialog::settingsDialog() const {
    return settings_.data();
}

SettingsDialog& GuidedSetupDialog::ensureSettings() {
    if (settings_ == nullptr) {
        SettingsDialog& made = ports_.settings();
        settings_ = &made;
        made.ensurePage(dest::recording);
        // The microphone worker fills this model asynchronously.
        if (auto* combo = qobject_cast<QComboBox*>(made.control("audio_device_combo"))) {
            QAbstractItemModel* model = combo->model();
            connect(model, &QAbstractItemModel::modelReset, this, [this] { queueRefresh(); });
            connect(model, &QAbstractItemModel::rowsInserted, this, [this] { queueRefresh(); });
            connect(model, &QAbstractItemModel::rowsRemoved, this, [this] { queueRefresh(); });
            connect(model, &QAbstractItemModel::dataChanged, this, [this] { queueRefresh(); });
        }
    }
    return *settings_;
}

// --- construction ---

void GuidedSetupDialog::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* header = new QFrame();
    header->setObjectName(QStringLiteral("guidedHeader"));
    auto* headerRow = new QHBoxLayout(header);
    headerRow->setContentsMargins(20, 12, 16, 12);
    headerRow->setSpacing(10);
    auto* icon = new QLabel();
    icon->setPixmap(ui::appIcon().pixmap(28, 28));
    headerRow->addWidget(icon);
    headerRow->addWidget(label(QStringLiteral("Set up ") + product(), "guidedBrand"));
    headerRow->addStretch();
    allSettingsButton_ =
        button(QStringLiteral("All settings"), [this] { allSettings(); }, "guidedLink");
    headerRow->addWidget(allSettingsButton_);
    root->addWidget(header);

    auto* body = new QWidget();
    body->setObjectName(QStringLiteral("guidedCanvas"));
    auto* column = new QVBoxLayout(body);
    column->setContentsMargins(28, 16, 28, 0);
    column->setSpacing(12);
    QHBoxLayout* steps = nullptr;
    std::tie(stepRow_, steps) = group<QHBoxLayout>(8);
    for (int index = 0; index < static_cast<int>(std::size(kSteps)); ++index) {
        const QString title = text(kSteps[index]);
        auto* step = new QPushButton(title);
        step->setObjectName(QStringLiteral("guidedStep"));
        step->setCursor(Qt::PointingHandCursor);
        step->setAccessibleName(QStringLiteral("Step %1 of %2: %3")
                                    .arg(index + 1)
                                    .arg(static_cast<int>(std::size(kSteps)))
                                    .arg(title));
        connect(step, &QPushButton::clicked, this, [this, index] { showPage(Record + index); });
        steps->addWidget(step, 1);
        stepButtons_.push_back(step);
    }
    column->addWidget(stepRow_);
    scroll_ = new QScrollArea();
    scroll_->setObjectName(QStringLiteral("guidedScroll"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pages_ = new QStackedWidget();
    pages_->setObjectName(QStringLiteral("guidedCanvas"));
    pages_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    buildWelcomePage();
    buildRecordPage();
    buildCleanUpPage();
    buildOutputPage();
    buildReadyPage();
    scroll_->setWidget(pages_);
    column->addWidget(scroll_, 1);
    error_ = label(QString(), "guidedError");
    error_->hide();
    column->addWidget(error_);
    root->addWidget(body, 1);

    auto* footer = new QFrame();
    footer->setObjectName(QStringLiteral("guidedFooter"));
    auto* actions = new QHBoxLayout(footer);
    actions->setContentsMargins(20, 12, 20, 14);
    actions->setSpacing(10);
    skipButton_ = button(QStringLiteral("Skip setup"), [this] { reject(); }, "guidedLink");
    actions->addWidget(skipButton_);
    actions->addStretch();
    backButton_ = button(QStringLiteral("Back"), [this] { back(); });
    actions->addWidget(backButton_);
    nextButton_ = button(QStringLiteral("Continue"), [this] { advance(); }, "guidedPrimary");
    nextButton_->setDefault(true);
    actions->addWidget(nextButton_);
    root->addWidget(footer);
}

QVBoxLayout* GuidedSetupDialog::addPage(const QString& title, const QString& lede,
                                        const char* titleName, WrappedLabel** titleLabel,
                                        WrappedLabel** ledeLabel) {
    auto* page = new QWidget();
    page->setObjectName(QStringLiteral("guidedPage"));
    auto* column = new QVBoxLayout(page);
    column->setContentsMargins(0, 8, 0, 16);
    column->setSpacing(14);
    auto [heading, headingLayout] = group<QVBoxLayout>(6);
    WrappedLabel* headingLabel = label(title, titleName);
    headingLayout->addWidget(headingLabel);
    WrappedLabel* ledeText = label(lede, "guidedLede");
    headingLayout->addWidget(ledeText);
    column->addWidget(heading);
    pages_->addWidget(page);
    if (titleLabel != nullptr) {
        *titleLabel = headingLabel;
    }
    if (ledeLabel != nullptr) {
        *ledeLabel = ledeText;
    }
    return column;
}

QVBoxLayout* GuidedSetupDialog::addCard(QVBoxLayout* column) {
    auto* card = new QFrame();
    card->setObjectName(QStringLiteral("guidedCard"));
    auto* inner = new QVBoxLayout(card);
    inner->setContentsMargins(20, 16, 20, 16);
    inner->setSpacing(16);
    column->addWidget(card);
    return inner;
}

WrappedLabel* GuidedSetupDialog::addRow(QBoxLayout* column, const QString& title,
                                        const QString& detail, QWidget* control) {
    auto* row = new QBoxLayout(QBoxLayout::LeftToRight);
    row->setSpacing(18);
    auto* copy = new QVBoxLayout();
    copy->setSpacing(3);
    WrappedLabel* titleLabel = label(title, "guidedRowTitle");
    copy->addWidget(titleLabel);
    WrappedLabel* description = label(detail);
    copy->addWidget(description);
    row->addLayout(copy, 1);
    row->addWidget(control);
    control->setAccessibleName(title);
    column->addLayout(row);
    rows_.push_back({row, control});
    if (title == QStringLiteral("Cleanup provider")) {
        providerTitle_ = titleLabel;
    }
    return description;
}

QProgressBar* GuidedSetupDialog::makeProgressBar() {
    auto* bar = new QProgressBar();
    bar->setObjectName(QStringLiteral("guidedProgress"));
    bar->setTextVisible(false);
    bar->setFixedHeight(8);
    bar->setAccessibleName(QStringLiteral("Speech model download"));
    bar->hide();
    return bar;
}

QPushButton* GuidedSetupDialog::addLinkRow(QVBoxLayout* column, const QString& content,
                                           std::function<void()> action) {
    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    QPushButton* link = button(content, std::move(action), "guidedLink");
    row->addWidget(link);
    row->addStretch();
    column->addLayout(row);
    return link;
}

void GuidedSetupDialog::buildWelcomePage() {
    QVBoxLayout* column =
        addPage(QStringLiteral("Welcome to ") + product(),
                QStringLiteral("Speak, and your words appear wherever you're typing. Three quick "
                               "choices and you're ready."),
                "guidedHeroTitle");
    auto* icon = new QLabel();
    icon->setPixmap(ui::appIcon().pixmap(64, 64));
    column->insertWidget(0, icon);
    QVBoxLayout* card = addCard(column);
    struct Line {
        const char* svg;
        const char* title;
        const char* detail;
    };
    for (const Line& line : {
             Line{"microphone-blue.svg", "Record",
                  "Your microphone and the shortcut that starts dictation."},
             Line{"wand-purple.svg", "Clean up",
                  "Optional AI tidying for punctuation and filler words."},
             Line{"bolt-green.svg", "Output", "Paste into the app you're using, copy it, or both."},
         }) {
        auto* row = new QHBoxLayout();
        row->setSpacing(14);
        auto* glyph = new QLabel();
        glyph->setPixmap(ui::designIcon(QString::fromLatin1(line.svg)).pixmap(22, 22));
        glyph->setAlignment(Qt::AlignTop);
        row->addWidget(glyph);
        auto* copy = new QVBoxLayout();
        copy->setSpacing(2);
        copy->addWidget(label(QString::fromUtf8(line.title), "guidedRowTitle"));
        copy->addWidget(label(QString::fromUtf8(line.detail)));
        row->addLayout(copy, 1);
        card->addLayout(row);
    }
    column->addWidget(label(QStringLiteral(u"Takes about a minute. Every choice saves as you go, "
                                           u"and you can come back any time from File → Guided "
                                           u"setup.")));
    column->addStretch();
    welcomeExtras_ = new QVBoxLayout();
    welcomeExtras_->setContentsMargins(0, 0, 0, 0);
    welcomeExtras_->setSpacing(0);
    column->addLayout(welcomeExtras_);
}

void GuidedSetupDialog::refreshImportRow() {
    const bool offered = ports_.importOffered && ports_.importOffered();
    if (offered && importRow_ == nullptr) {
        // A card like the Welcome page's, with a row like the steps' own (addRow), so nothing
        // new needs a style of its own.
        importRow_ = new QFrame();
        importRow_->setObjectName(QStringLiteral("guidedCard"));
        auto* row = new QHBoxLayout(importRow_);
        row->setContentsMargins(20, 16, 20, 16);
        row->setSpacing(18);
        auto* copy = new QVBoxLayout();
        copy->setSpacing(3);
        const QString legacy = text(legacy_identity().product);
        const QString title = QStringLiteral("Bring over your %1 data").arg(legacy);
        copy->addWidget(label(title, "guidedRowTitle"));
        copy->addWidget(label(QStringLiteral("Your %1 settings, history and recordings, and the "
                                             "speech model it already downloaded.")
                                  .arg(legacy)));
        row->addLayout(copy, 1);
        importButton_ = button(QStringLiteral("Import…"), [this] {
            if (ports_.openImport) {
                ports_.openImport(this);
            }
        });
        importButton_->setAccessibleName(title);
        row->addWidget(importButton_, 0, Qt::AlignVCenter);
        welcomeExtras_->addWidget(importRow_);
    }
    if (importRow_ != nullptr) {
        importRow_->setVisible(offered);
    }
}

void GuidedSetupDialog::buildRecordPage() {
    QVBoxLayout* column = addPage(
        QStringLiteral("How will you record?"),
        QStringLiteral("Pick a microphone and the shortcut that starts dictation."), "guidedTitle");
    QVBoxLayout* card = addCard(column);
    microphone_ = new ElidingComboBox();
    microphone_->setObjectName(QStringLiteral("basicSettingsCombo"));
    microphone_->setMinimumHeight(38);
    microphone_->setMinimumWidth(200);
    microphone_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto [microphoneHolder, microphoneRow] = group<QHBoxLayout>(8);
    microphoneRow->addWidget(microphone_, 1);
    micTestButton_ = button(QStringLiteral("Test"), [this] { toggleMicCheck(); });
    micTestButton_->setAccessibleName(QStringLiteral("Test microphone"));
    microphoneRow->addWidget(micTestButton_);
    microphoneDetail_ = addRow(card, QStringLiteral("Microphone"), QString(), microphoneHolder);
    microphone_->setAccessibleName(QStringLiteral("Microphone"));
    connect(microphone_, &QComboBox::currentIndexChanged, this, [this] { chooseMicrophone(); });
    micCheck_ = new MicrophoneCheck([this]() -> std::unique_ptr<Recorder> {
        ensureSettings();
        return ports_.recorder ? ports_.recorder() : nullptr;
    });
    connect(micCheck_, &MicrophoneCheck::runningChanged, this, [this](bool running) {
        setLabel(micTestButton_, running ? QStringLiteral("Stop") : QStringLiteral("Test"));
    });
    micCheck_->hide();
    card->addWidget(micCheck_);

    auto [shortcutHolder, shortcutRow] = group<QHBoxLayout>(8);
    shortcut_ = new ProfileHotkeyInput();
    shortcut_->setAccessibleName(QStringLiteral("Recording shortcut"));
    shortcut_->setToolTip(QStringLiteral("Click Change, then press a shortcut. Escape cancels."));
    connect(shortcut_, &ProfileHotkeyInput::captureChanged, this,
            [this](bool capturing) { ensureSettings().setHotkeyCaptureSuspended(capturing); });
    connect(shortcut_, &ProfileHotkeyInput::captured, this,
            [this](const QString& hotkey) { shortcutCaptured(hotkey); });
    shortcutRow->addWidget(shortcut_);
    shortcutRow->addWidget(button(QStringLiteral("Change"), [this] { captureShortcut(); }));
    shortcutDetail_ = addRow(card, QStringLiteral("Shortcut"), QString(), shortcutHolder);

    trigger_ = new ElidingComboBox();
    trigger_->setObjectName(QStringLiteral("basicSettingsCombo"));
    trigger_->setMinimumHeight(38);
    trigger_->setMinimumWidth(200);
    trigger_->addItem(QStringLiteral("Press to start and stop"), QStringLiteral("toggle"));
    trigger_->addItem(QStringLiteral("Hold to talk"), QStringLiteral("push_hold"));
    addRow(card, QStringLiteral("Press or hold"),
           QStringLiteral("Press once and talk for as long as you like, or hold the shortcut like "
                          "a walkie-talkie."),
           trigger_);
    connect(trigger_, &QComboBox::currentIndexChanged, this, [this] { chooseTrigger(); });

    auto [speechHolder, speechRow] = group<QHBoxLayout>(8);
    speechDownloadButton_ = button(QStringLiteral("Download"), [this] { downloadSpeech(); });
    speechDownloadButton_->setAccessibleName(QStringLiteral("Download the speech model"));
    speechRow->addWidget(speechDownloadButton_);
    speechButton_ =
        button(QStringLiteral("Change"), [this] { open(dest::voice_model); }, "guidedLink");
    speechRow->addWidget(speechButton_);
    speechSummary_ = addRow(card, QStringLiteral("Speech engine"), QString(), speechHolder);
    speechProgress_ = makeProgressBar();
    card->addWidget(speechProgress_);
    addLinkRow(column, QStringLiteral("More recording options"), [this] { open(dest::recording); });
    column->addStretch();
}

void GuidedSetupDialog::buildCleanUpPage() {
    QVBoxLayout* column = addPage(QStringLiteral("Tidy up what you say?"),
                                  QStringLiteral("Optional. After each dictation, an AI model "
                                                 "fixes punctuation and drops filler words."),
                                  "guidedTitle");
    QVBoxLayout* card = addCard(column);
    cleanupSwitch_ = new SettingsSwitch();
    cleanupDetail_ = addRow(card, QStringLiteral("Use AI cleanup"), QString(), cleanupSwitch_);
    connect(cleanupSwitch_, &QAbstractButton::toggled, this,
            [this](bool value) { applyCleanupSwitch(value); });
    QVBoxLayout* options = nullptr;
    std::tie(cleanupOptions_, options) = group<QVBoxLayout>(14);
    privacy_ = label(QString(), "guidedPrivacy");
    options->addWidget(privacy_);
    providerButton_ = button(QStringLiteral("Change"), [this] { providerAction(); }, "guidedLink");
    providerDetail_ =
        addRow(options, QStringLiteral("Cleanup provider"), QString(), providerButton_);
    levels_ = new CleanupChoices();
    levels_->setAccessibleName(QStringLiteral("Cleanup level"));
    connect(levels_, &SegmentedBar::activated, this, [this](int index) { chooseLevel(index); });
    options->addWidget(levels_);
    levelNote_ = label(QString());
    options->addWidget(levelNote_);
    preview_ = new QFrame();
    preview_->setObjectName(QStringLiteral("guidedPreview"));
    auto* preview = new QBoxLayout(QBoxLayout::LeftToRight, preview_);
    preview->setContentsMargins(16, 12, 16, 12);
    preview->setSpacing(18);
    rows_.push_back({preview, nullptr});
    for (const auto& [title, content] :
         {std::pair{QStringLiteral("What you say"), QStringLiteral("Meet at two, actually three.")},
          std::pair{QStringLiteral("What you get"), QString()}}) {
        auto* copy = new QVBoxLayout();
        copy->setSpacing(4);
        copy->addWidget(label(title));
        WrappedLabel* sample = label(content, "guidedSample");
        copy->addWidget(sample);
        preview->addLayout(copy, 1);
        sampleResult_ = sample;
    }
    options->addWidget(preview_);
    sampleNote_ = label(
        QStringLiteral("An illustration. Real results depend on your model and instructions."));
    options->addWidget(sampleNote_);
    card->addWidget(cleanupOptions_);
    column->addStretch();
}

void GuidedSetupDialog::buildOutputPage() {
    QVBoxLayout* column =
        addPage(QStringLiteral("Where should your words go?"),
                QStringLiteral("Choose what happens when you finish speaking."), "guidedTitle");
    QVBoxLayout* card = addCard(column);
    pasteSwitch_ = new SettingsSwitch();
    pasteDetail_ =
        addRow(card, QStringLiteral("Paste automatically"),
               QStringLiteral("Insert the text into the app you're typing in."), pasteSwitch_);
    connect(pasteSwitch_, &QAbstractButton::toggled, this, [this](bool value) {
        applySwitch(dest::general, "auto_paste_check", keys::auto_paste.key(), value);
    });
    copySwitch_ = new SettingsSwitch();
    addRow(card, QStringLiteral("Copy to clipboard"),
           QStringLiteral("Keep it ready to paste yourself when auto-paste is off or can't reach "
                          "an app."),
           copySwitch_);
    connect(copySwitch_, &QAbstractButton::toggled, this, [this](bool value) {
        applySwitch(dest::general, "copy_clipboard_check", keys::copy_clipboard.key(), value);
    });
    liveSwitch_ = new SettingsSwitch();
    addRow(card, QStringLiteral("Show live transcript"),
           QStringLiteral("Watch words appear while you speak. Not every speech engine supports "
                          "it."),
           liveSwitch_);
    connect(liveSwitch_, &QAbstractButton::toggled, this, [this](bool value) {
        applySwitch(dest::recording, "streaming_enabled_check", keys::streaming_enabled.key(),
                    value);
    });
    addLinkRow(column, QStringLiteral("More output options"), [this] { open(dest::general); });
    column->addStretch();
}

void GuidedSetupDialog::buildReadyPage() {
    QVBoxLayout* column = addPage(QStringLiteral("You're ready"), QString(), "guidedTitle",
                                  &readyTitle_, &readyLede_);
    QHBoxLayout* speech = nullptr;
    std::tie(readySpeech_, speech) = group<QHBoxLayout>(8);
    readySpeechNote_ = label(QString(), "guidedDetail");
    ui::setStyleProperty(readySpeechNote_, ui::prop::tone, QStringLiteral("attention"));
    speech->addWidget(readySpeechNote_, 1);
    readyDownloadButton_ =
        button(QStringLiteral("Download"), [this] { downloadSpeech(); }, "guidedLink");
    speech->addWidget(readyDownloadButton_);
    column->addWidget(readySpeech_);
    readyProgress_ = makeProgressBar();
    column->addWidget(readyProgress_);
    QHBoxLayout* cleanup = nullptr;
    std::tie(readyCleanup_, cleanup) = group<QHBoxLayout>(8);
    readyCleanupNote_ = label(QString(), "guidedDetail");
    ui::setStyleProperty(readyCleanupNote_, ui::prop::tone, QStringLiteral("attention"));
    cleanup->addWidget(readyCleanupNote_, 1);
    cleanup->addWidget(
        button(QStringLiteral("Add key"), [this] { open(dest::api_keys); }, "guidedLink"));
    column->addWidget(readyCleanup_);
    QVBoxLayout* card = addCard(column);
    auto* keyRow = new QHBoxLayout();
    keyRow->setSpacing(16);
    keycap_ = new QLabel();
    keycap_->setObjectName(QStringLiteral("guidedKeycap"));
    keycap_->setAlignment(Qt::AlignCenter);
    keycap_->setMinimumWidth(64);
    keyRow->addWidget(keycap_, 0, Qt::AlignVCenter);
    keycapHelp_ = label(QString(), "guidedLede");
    keyRow->addWidget(keycapHelp_, 1);
    card->addLayout(keyRow);
    tryBox_ = new QPlainTextEdit();
    tryBox_->setObjectName(QStringLiteral("guidedTry"));
    tryBox_->setAccessibleName(QStringLiteral("Try dictation here"));
    tryBox_->setTabChangesFocus(true);
    tryBox_->setMinimumHeight(96);
    tryBox_->setMaximumHeight(140);
    card->addWidget(tryBox_);
    tryNote_ = label(QString());
    card->addWidget(tryNote_);
    column->addWidget(label(QStringLiteral(u"Change any of this later in Settings, or run setup "
                                           u"again from File → Guided setup.")));
    column->addStretch();
}

// --- projection ---

void GuidedSetupDialog::queueRefresh() {
    refreshTimer_->start(0);
}

void GuidedSetupDialog::refresh() {
    if (settings_ == nullptr) {
        return;
    }
    const auto snapshot = store_.snapshot();
    const Json& settings = *snapshot;
    std::vector<std::pair<QString, QVariant>> items;
    if (auto* source = qobject_cast<QComboBox*>(settings_->control("audio_device_combo"))) {
        for (int index = 0; index < source->count(); ++index) {
            items.emplace_back(source->itemText(index), source->itemData(index));
        }
    }
    std::vector<std::pair<QString, QVariant>> old;
    for (int index = 0; index < microphone_->count(); ++index) {
        old.emplace_back(microphone_->itemText(index), microphone_->itemData(index));
    }
    {
        const QSignalBlocker blocker(microphone_);
        if (items != old) {
            microphone_->clear();
            for (const auto& [name, token] : items) {
                microphone_->addItem(name, token);
            }
        }
        microphone_->setCurrentIndex(std::max(
            0, microphone_->findData(qt::toQString(appcore::mic::preferred_token(settings)))));
    }
    const bool backups = appcore::mic::saved_priority(settings).size() > 1;
    microphoneDetail_->setText(
        QStringLiteral("The one %1 listens to. Test it to see that it hears you.").arg(product()) +
        (backups ? QStringLiteral(" Your backup microphones stay as they are.") : QString()));
    {
        const QSignalBlocker blocker(trigger_);
        trigger_->setCurrentIndex(
            std::max(0, trigger_->findData(text(keys::recording_trigger_mode.read(settings)))));
    }
    const QString hotkey = qt::toQString(recordHotkey(store_));
    if (!shortcut_->isCapturing()) {
        shortcut_->setHotkey(hotkey);
    }
    const std::string current = qt::toUtf8(shortcut_->hotkey());
    shortcutDetail_->setText(
        text(shortcut_help(current, settings)) +
        // The default is a number-pad key, which most laptops lack.
        (on_number_pad(current) ? QStringLiteral(" No number pad? Choose Change.") : QString()));
    const std::pair<SettingsSwitch*, std::string_view> switches[] = {
        {cleanupSwitch_, keys::transcript_cleanup_enabled.key()},
        {pasteSwitch_, keys::auto_paste.key()},
        {copySwitch_, keys::copy_clipboard.key()},
        {liveSwitch_, keys::streaming_enabled.key()},
    };
    for (const auto& [control, key] : switches) {
        const QSignalBlocker blocker(control);
        control->setChecked(resolve_bool_setting(key, settings));
    }
    refreshCleanup(settings);
    refreshSpeech();
    // _refresh_output's variants are for native Wayland and macOS only; Windows keeps the
    // built text.
    refreshReady(settings, hotkey);
    // Rows that come and go change this page's height.
    pages_->adjustSize();
}

void GuidedSetupDialog::refreshCleanup(const Json& settings) {
    const bool enabled = cleanupSwitch_->isChecked();
    cleanupOptions_->setVisible(enabled);
    cleanupDetail_->setText(enabled ? QStringLiteral("Fixes punctuation and formatting after "
                                                     "transcription.")
                                    : QStringLiteral("Off: you get exactly what the speech engine "
                                                     "heard, and no text is sent anywhere for "
                                                     "cleanup."));
    const bool custom = !ports_.cleanup.customPrompt(settings).empty();
    const std::string level = keys::transcript_cleanup_level.read(settings);
    // The built-in tiles must not appear selected while a custom prompt wins.
    levels_->setChoice(text(level), custom);
    levelNote_->setText(QStringLiteral(
        "Your own cleanup prompt is in use. Picking a level asks before replacing it."));
    levelNote_->setVisible(custom);
    preview_->setVisible(!custom);
    sampleNote_->setVisible(!custom);
    sampleResult_->setText(level == "light" ? QStringLiteral("Meet at two, actually three.")
                                            : QStringLiteral("Meet at 3."));
    providerNeedsKey_ = false;
    // The provider row and the privacy text are provider_summary's (M4): hidden without it.
    const bool summarized = showProviderRow();
    if (enabled && summarized) {
        const ProviderSummary summary = ports_.providerSummary(settings);
        providerDetail_->setText(summary.name + QStringLiteral(" · ") + summary.status);
        ui::setStyleProperty(providerDetail_, ui::prop::tone,
                             summary.needsKey ? QStringLiteral("attention") : QString());
        providerButton_->setText(summary.needsKey ? QStringLiteral("Add key")
                                                  : QStringLiteral("Change"));
        providerNeedsKey_ = summary.needsKey;
        privacy_->setText(summary.privacy);
        ui::setStyleProperty(privacy_, kRemoteProperty, summary.remote);
        readyCleanupNote_->setText(QStringLiteral("AI cleanup needs an API key for %1. Until it "
                                                  "has one, your text arrives without cleanup.")
                                       .arg(summary.name));
    }
    readyCleanup_->setVisible(providerNeedsKey_);
}

bool GuidedSetupDialog::showProviderRow() {
    const bool summarized = static_cast<bool>(ports_.providerSummary);
    for (QWidget* part :
         {static_cast<QWidget*>(privacy_), static_cast<QWidget*>(providerTitle_),
          static_cast<QWidget*>(providerDetail_), static_cast<QWidget*>(providerButton_)}) {
        part->setVisible(summarized);
    }
    return summarized;
}

void GuidedSetupDialog::refreshSpeech() {
    const auto snapshot = store_.snapshot();
    const SpeechStatus status =
        speech_status(voice_summary(*snapshot), voice_detail(*snapshot), speechReady_,
                      downloadProgress_, qt::toUtf8(downloadError_));
    if (downloadProgress_) {
        for (QProgressBar* bar : {speechProgress_, readyProgress_}) {
            bar->setRange(0, status.bar_maximum);
            if (status.bar_value) {
                bar->setValue(*status.bar_value);
            }
        }
    }
    speechSummary_->setText(text(status.summary_line));
    if (status.ready_note) {
        readySpeechNote_->setText(text(*status.ready_note));
    }
    ui::setStyleProperty(speechSummary_, ui::prop::tone, text(status.tone));
    ui::setStyleProperty(readySpeechNote_, ui::prop::tone, text(status.tone));
    speechProgress_->setVisible(status.progress_visible);
    readyProgress_->setVisible(status.progress_visible);
    speechDownloadButton_->setVisible(status.record_download_visible);
    readyDownloadButton_->setVisible(status.ready_download_visible);
    readySpeech_->setVisible(status.ready_row_visible);
    readyTitle_->setText(text(status.ready_title));
    readyLede_->setText(text(status.ready_lede));
}

void GuidedSetupDialog::refreshReady(const Json& settings, const QString& hotkey) {
    const QString shown = text(displayHotkey(qt::toUtf8(hotkey)));
    keycap_->setText(shown.isEmpty() ? QStringLiteral("No shortcut") : shown);
    keycapHelp_->setText(
        shown.isEmpty()
            ? QStringLiteral("Set a shortcut on the Record step, or use the Record button.")
            : text(shortcut_help(qt::toUtf8(hotkey), settings)));
    tryBox_->setPlaceholderText(
        shown.isEmpty() ? QStringLiteral("Try it here.")
                        : QStringLiteral("Try it here: press %1 and say something.").arg(shown));
    ui::setStyleProperty(tryNote_, ui::prop::tone, tryResult_ ? tryResult_->first : QString());
    if (tryResult_) {
        tryNote_->setText(tryResult_->second);
    } else if (!pasteSwitch_->isChecked()) {
        tryNote_->setText(copySwitch_->isChecked()
                              ? QStringLiteral("Auto-paste is off, so click here and press Ctrl+V "
                                               "after you speak.")
                              : QStringLiteral("Auto-paste and copying are both off, so the text "
                                               "only appears in History."));
    } else {
        tryNote_->setText(QStringLiteral("Your words land wherever the cursor is, in any app."));
    }
}

void GuidedSetupDialog::updateSpeechStatus() {
    const auto snapshot = store_.snapshot();
    speechReady_ =
        ports_.speechModelDownloaded ? ports_.speechModelDownloaded(*snapshot) : std::nullopt;
}

// --- actions ---

void GuidedSetupDialog::sourceResult() {
    const QString message = ensureSettings().messageText();
    const bool failure = message.startsWith(QStringLiteral("Couldn't")) ||
                         message.startsWith(QStringLiteral("That shortcut")) ||
                         message.startsWith(QStringLiteral("Could not"));
    showError(failure ? message : QString());
    refresh();
}

void GuidedSetupDialog::applySwitch(std::string_view destination, std::string_view control,
                                    std::string_view key, bool value) {
    SettingsDialog& settings = ensureSettings();
    settings.ensurePage(destination);
    auto* source = qobject_cast<QAbstractButton*>(settings.control(control));
    if (source == nullptr) {
        kLog.warning("Settings has no {} to change", control);
        refresh();
        return;
    }
    // Resync first: a previous failed save must allow the same choice again.
    {
        const QSignalBlocker blocker(source);
        source->setChecked(resolve_bool_setting(key, *store_.snapshot()));
    }
    if (source->isEnabled()) {
        source->setChecked(value);
    }
    sourceResult();
    {
        const QSignalBlocker blocker(source);
        source->setChecked(resolve_bool_setting(key, *store_.snapshot()));
    }
}

void GuidedSetupDialog::applyCleanupSwitch(bool value) {
    SettingsDialog& settings = ensureSettings();
    ports_.cleanup.setEnabled(settings, value);
    sourceResult();
    if (ports_.cleanup.updatePromptUi) {
        ports_.cleanup.updatePromptUi(settings);
    }
    if (cleanupSwitch_->isChecked()) {
        privacyRevealTimer_->start(0);
    }
}

void GuidedSetupDialog::revealCleanupPrivacy() {
    if (page_ == CleanUp && cleanupSwitch_->isChecked()) {
        scroll_->ensureWidgetVisible(privacy_, 0, 8);
    }
}

void GuidedSetupDialog::chooseMicrophone() {
    auto* source = qobject_cast<QComboBox*>(ensureSettings().control("audio_device_combo"));
    const int index = source == nullptr ? -1 : source->findData(microphone_->currentData());
    if (index < 0) {
        // The worker replaced its inventory while this menu was open.
        refresh();
        return;
    }
    source->setCurrentIndex(index);
    sourceResult();
    if (micCheck_->isRunning()) {
        micCheck_->start();
    }
}

void GuidedSetupDialog::toggleMicCheck() {
    if (micCheck_->isRunning()) {
        micCheck_->stop();
        return;
    }
    micCheck_->show();
    micCheck_->start();
    pages_->adjustSize();
}

void GuidedSetupDialog::chooseTrigger() {
    SettingsDialog& settings = ensureSettings();
    settings.ensurePage(dest::hotkeys);
    auto* source = qobject_cast<QComboBox*>(settings.control("record_mode_combo"));
    if (source == nullptr) {
        kLog.warning("Settings has no record_mode_combo to change");
        refresh();
        return;
    }
    const auto saved = [this, source] {
        return source->findData(text(keys::recording_trigger_mode.read(*store_.snapshot())));
    };
    // Resync first: a previous failed save must allow the same choice again.
    {
        const QSignalBlocker blocker(source);
        source->setCurrentIndex(saved());
    }
    source->setCurrentIndex(source->findData(trigger_->currentData()));
    sourceResult();
    {
        const QSignalBlocker blocker(source);
        source->setCurrentIndex(saved());
    }
    // Settings shows the double-tap row for the saved mode, not a refused one
    // (_load_hands_free_latch, which the page's load runs).
    if (auto* hotkeys = qobject_cast<HotkeysPage*>(settings.pageContent(dest::hotkeys))) {
        hotkeys->loadSettings(*store_.snapshot());
    }
}

void GuidedSetupDialog::captureShortcut() {
    shortcut_->setFocus();
    shortcut_->beginCapture();
}

void GuidedSetupDialog::shortcutCaptured(const QString& hotkey) {
    SettingsDialog& settings = ensureSettings();
    // current_hotkeys = load_hotkey_settings(): the page saves over what is saved now.
    settings.ensurePage(dest::hotkeys);
    if (auto* hotkeys = qobject_cast<HotkeysPage*>(settings.pageContent(dest::hotkeys))) {
        hotkeys->loadSettings(*store_.snapshot());
    }
    settings.setStandardHotkey(QStringLiteral("record_toggle"), hotkey);
    sourceResult();
}

void GuidedSetupDialog::chooseLevel(int index) {
    const auto& segment = CleanupChoices::segments().at(static_cast<std::size_t>(index));
    if (std::string_view(segment.value) != "custom") {
        ports_.cleanup.chooseLevel(ensureSettings(), QString::fromLatin1(segment.value));
    }
    // activated fires before the bar finishes selecting the clicked button.
    sourceResult();
    queueRefresh();
}

void GuidedSetupDialog::providerAction() {
    open(providerNeedsKey_ ? dest::api_keys : dest::cleanup);
}

// --- the speech download ---

void GuidedSetupDialog::downloadSpeech() {
    const auto snapshot = store_.snapshot();
    const std::optional<std::string> model =
        ports_.speechModelName ? ports_.speechModelName(*snapshot) : std::nullopt;
    if (!model || !ports_.downloadModel) {
        open(dest::voice_model);
        return;
    }
    downloadError_.clear();
    expectingDownload_ = true;
    requestedModel_ = text(*model);
    // The app's own request: its consent dialogs decide whether it starts.
    ports_.downloadModel(requestedModel_);
    refresh();
}

bool GuidedSetupDialog::showsModelDownload(const QString& model) const {
    return modelBusy_ && *modelBusy_ == model;
}

bool GuidedSetupDialog::claimsModelDownload(const QString& model) const {
    return showsModelDownload(model) || (expectingDownload_ && !modelBusy_ &&
                                         !requestedModel_.isEmpty() && model == requestedModel_);
}

bool GuidedSetupDialog::showsRuntimeInstall(const QString& componentId) const {
    return expectingDownload_ && isVisible() && ports_.isRuntime(componentId);
}

void GuidedSetupDialog::modelDownloadStarted(const QString& model) {
    if (!expectingDownload_ || model != requestedModel_) {
        return;
    }
    modelBusy_ = model;
    downloadProgress_ = SpeechProgress{"Downloading…", 0, 0};
    refresh();
}

void GuidedSetupDialog::modelDownloadProgress(const QString& model, std::int64_t done,
                                              std::int64_t total) {
    if (!modelBusy_ || *modelBusy_ != model) {
        return;
    }
    downloadProgress_ = SpeechProgress{download_progress_text(done, total), done, total};
    refreshSpeech();
}

void GuidedSetupDialog::modelDownloadFinished(const QString& model, bool success) {
    if (!modelBusy_ || *modelBusy_ != model) {
        return;
    }
    modelBusy_.reset();
    if (!success) {
        downloadError_ =
            QStringLiteral("The download didn't finish. Check your connection and try again.");
    }
    downloadSettled();
}

void GuidedSetupDialog::runtimeInstallProgress(const QString& componentId, std::int64_t done,
                                               std::int64_t total) {
    if (!showsRuntimeInstall(componentId) && !runtimeBusy_) {
        return;
    }
    runtimeBusy_ = true;
    if (!modelBusy_) {
        downloadProgress_ = SpeechProgress{"Installing what the model runs on…", done, total};
        refreshSpeech();
    }
}

void GuidedSetupDialog::runtimeInstallFinished(const QString& /*componentId*/, bool success,
                                               const QString& message) {
    if (!runtimeBusy_) {
        return;
    }
    runtimeBusy_ = false;
    if (!success) {
        downloadError_ =
            message.isEmpty() ? QStringLiteral("The install didn't finish. Try again.") : message;
    }
    downloadSettled();
}

void GuidedSetupDialog::downloadSettled() {
    if (modelBusy_) {
        return;
    }
    if (runtimeBusy_) {
        downloadProgress_ = SpeechProgress{"Installing what the model runs on…", 0, 0};
    } else {
        downloadProgress_.reset();
        updateSpeechStatus();
        if (speechReady_ != std::optional<bool>(false) || !downloadError_.isEmpty()) {
            expectingDownload_ = false;
        }
    }
    if (settings_ != nullptr) {
        refresh();
    }
}

void GuidedSetupDialog::setDownloadModel(std::function<void(const QString&)> download) {
    ports_.downloadModel = std::move(download);
}

void GuidedSetupDialog::setProviderSummary(std::function<ProviderSummary(const Json&)> summary) {
    ports_.providerSummary = std::move(summary);
    if (settings_ != nullptr) {
        refresh();
    }
}

// --- trying it out ---

void GuidedSetupDialog::dictationReceived(const QString& dictated) {
    if (page_ != Ready || !isVisible() || dictated.isEmpty()) {
        return;
    }
    triedText_ = dictated;
    tryResult_ = std::pair{QStringLiteral("ready"),
                           QStringLiteral("That worked. %1 is set up.").arg(product())};
    // A first dictation may have just downloaded the model.
    updateSpeechStatus();
    queueRefresh();
    // A paste lands a moment after the transcript is announced; wait it out.
    tryFillTimer_->start(pasteSwitch_->isChecked() ? kTryFillMs : kTryFillQuickMs);
}

void GuidedSetupDialog::dictationFailed(const QString& message) {
    if (page_ != Ready || !isVisible()) {
        return;
    }
    tryFillTimer_->stop();
    tryResult_ = std::pair{QStringLiteral("attention"),
                           message.isEmpty()
                               ? QStringLiteral("That one didn't come through. Try again.")
                               : QStringLiteral("That one didn't come through: %1").arg(message)};
    queueRefresh();
}

void GuidedSetupDialog::fillTryBox() {
    // Auto-paste is off, or could not reach this box: show the words anyway.
    if (page_ != Ready || !tryBox_->toPlainText().trimmed().isEmpty()) {
        return;
    }
    tryBox_->setPlainText(triedText_);
    const QString where = copySwitch_->isChecked() ? QStringLiteral("It's on your clipboard too.")
                                                   : QStringLiteral("It's saved in History.");
    tryResult_ = std::pair{QStringLiteral("ready"),
                           QStringLiteral("That worked. Here's what you said. ") + where};
    refresh();
}

void GuidedSetupDialog::showError(const QString& message) {
    error_->setText(message);
    error_->setVisible(!message.isEmpty());
}

// --- navigation ---

void GuidedSetupDialog::begin(bool welcome) {
    finished_ = setup_state::is_completed(store_);
    tryBox_->clear();
    downloadError_.clear();
    tryResult_.reset();
    tryFillTimer_->stop();
    showError(QString());
    refreshImportRow();
    if (welcome) {
        showPage(Welcome);
    } else if (saveProgress(setup_state::kStarted)) {
        showPage(Record);
    }
}

void GuidedSetupDialog::showPage(int page) {
    shortcut_->cancelCapture();
    micCheck_->stop();
    micCheck_->hide();
    page_ = std::clamp(page, static_cast<int>(Welcome), static_cast<int>(Ready));
    pages_->setCurrentIndex(page_);
    // Size the stack to this page alone, or every page scrolls as far as the tallest one (Clean
    // up with its options open).
    for (int index = 0; index < pages_->count(); ++index) {
        const QSizePolicy::Policy policy =
            index == page_ ? QSizePolicy::Preferred : QSizePolicy::Ignored;
        pages_->widget(index)->setSizePolicy(policy, policy);
    }
    pages_->adjustSize();
    const int step = page_ - Record;
    stepRow_->setVisible(page_ != Welcome);
    for (int index = 0; index < static_cast<int>(stepButtons_.size()); ++index) {
        const bool done = index < step;
        QPushButton* stepButton = stepButtons_[static_cast<std::size_t>(index)];
        stepButton->setText((done ? QStringLiteral(u"✓") : QString::number(index + 1)) +
                            QStringLiteral("  ") + text(kSteps[index]));
        ui::setStyleProperty(stepButton, ui::prop::active, index == step);
        ui::setStyleProperty(stepButton, kDoneProperty, done);
    }
    QString skip;
    if (page_ == Welcome) {
        skip = QStringLiteral("Not now");
    } else {
        // Reopened from the File menu after finishing: nothing is skipped.
        skip = finished_ ? QStringLiteral("Close") : QStringLiteral("Skip setup");
    }
    skipButton_->setText(skip);
    skipButton_->setVisible(page_ != Ready);
    backButton_->setVisible(page_ > Record);
    setLabel(nextButton_, page_ == Welcome  ? QStringLiteral("Get started")
                          : page_ == Output ? QStringLiteral("Finish")
                          : page_ == Ready  ? QStringLiteral("Done")
                                            : QStringLiteral("Continue"));
    scroll_->verticalScrollBar()->setValue(0);
    if (page_ == Record || page_ == Ready) {
        updateSpeechStatus();
    }
    if (page_ != Welcome) {
        ensureSettings();
        refresh();
    }
    reflow();
    if (page_ == CleanUp && cleanupSwitch_->isChecked()) {
        privacyRevealTimer_->start(0);
    }
    if (isVisible()) {
        focusPage();
    }
}

void GuidedSetupDialog::focusPage() {
    // Never the header's All settings link, where Enter would leave setup.
    if (page_ == Ready) {
        tryBox_->setFocus();
    } else {
        nextButton_->setFocus();
    }
}

void GuidedSetupDialog::back() {
    showPage(page_ - 1);
}

void GuidedSetupDialog::advance() {
    if (page_ == Welcome) {
        if (saveProgress(setup_state::kStarted)) {
            showPage(Record);
        }
    } else if (page_ == Output) {
        if (saveOutcome(setup_state::kCompleted)) {
            finished_ = true;
            showPage(Ready);
        }
    } else if (page_ == Ready) {
        accept();
    } else {
        showPage(page_ + 1);
    }
}

bool GuidedSetupDialog::saveOutcome(std::string_view state) {
    try {
        setup_state::save_state(state, store_);
    } catch (const std::exception& error) {
        kLog.warning("Couldn't save guided setup progress: {}", error.what());
        showError(QStringLiteral("Couldn't save setup progress: ") +
                  QString::fromUtf8(error.what()));
        return false;
    }
    shortcut_->cancelCapture();
    showError(QString());
    return true;
}

bool GuidedSetupDialog::saveProgress(std::string_view state) {
    // A finished setup stays finished when it is reopened and left again.
    return finished_ || saveOutcome(state);
}

void GuidedSetupDialog::reject() {
    if (saveProgress(setup_state::kSkipped)) {
        QDialog::reject();
    }
}

void GuidedSetupDialog::allSettings() {
    if (saveProgress(setup_state::kSkipped)) {
        accept();
        if (ports_.openAllSettings) {
            ports_.openAllSettings();
        }
    }
}

void GuidedSetupDialog::open(std::string_view destination) {
    shortcut_->cancelCapture();
    if (ports_.openDestination) {
        ports_.openDestination(text(destination));
    }
}

// --- layout ---

void GuidedSetupDialog::reflow() {
    // round(620 * scale) never lands on a half at the four scales (558, 620, 713, 806).
    const bool narrow =
        width() < static_cast<int>(std::lround(kStackBelow * ui::currentUiFontScale()));
    for (const Row& row : rows_) {
        row.layout->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
        if (row.control != nullptr) {
            row.layout->setAlignment(row.control, narrow ? Qt::AlignLeft : Qt::AlignVCenter);
        }
    }
}

void GuidedSetupDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    if (!rows_.empty()) {
        reflow();
    }
}

void GuidedSetupDialog::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isActiveWindow() && page_ != Welcome) {
        // Back from Settings (a download, a key, another engine).
        if (page_ == Record || page_ == Ready) {
            updateSpeechStatus();
        }
        queueRefresh();
    }
    if ((event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) &&
        !rows_.empty()) {
        reflow();
    }
}

void GuidedSetupDialog::showEvent(QShowEvent* event) {
    QScreen* screen = this->screen() != nullptr ? this->screen() : QApplication::primaryScreen();
    if (screen != nullptr) {
        const QSize available = screen->availableGeometry().size() - QSize(40, 60);
        resize(size().boundedTo(available).expandedTo(minimumSize()));
    }
    QDialog::showEvent(event);
    queueRefresh();
    focusPage();
}

void GuidedSetupDialog::hideEvent(QHideEvent* event) {
    shortcut_->cancelCapture();
    micCheck_->stop();
    tryFillTimer_->stop();
    // A request that was declined must not claim later installs.
    if (!modelBusy_ && !runtimeBusy_) {
        expectingDownload_ = false;
    }
    QDialog::hideEvent(event);
}

} // namespace fv
