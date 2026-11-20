#include "ui_settings/home_page.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>

#include <QAbstractButton>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSystemTrayIcon>
#include <QVBoxLayout>

#include "appcore/mic_priority.h"
#include "appcore/whats_new.h"
#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "audio/devices.h"
#include "base/desktop_session.h"
#include "base/log.h"
#include "focus/capture_service.h"
#include "history/format_utils.h"
#include "hotkeys/mouse_shortcuts.h"
#include "llm/cleanup_settings.h"
#include "llm/profiles.h"
#include "settings/enums.h"
#include "settings/hotkeys.h"
#include "settings/keys.h"
#include "settings/model_selection.h"
#include "text/cleanup_prompts.h"
#include "text/personalize_copy.h"
#include "ui_hotkeys/hotkeys_page.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_settings/app_pages.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"
#include "ui_settings/settings_search_palette.h"
#include "ui_settings/whats_new_tile.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/icons.h"
#include "ui_theme/palette.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/speech_backend_picker.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_home"};
// choose_backend's own logger.
constexpr log::Logger kModelsLog{"ui_qt.dialogs.settings_models"};

QString q(std::string_view text) {
    return qt::toQString(text);
}

// The body of a Qt slot: nothing may leave it into Qt's event loop (architecture 4 rule 10).
template <class Fn> void guarded(std::string_view message, Fn&& fn) {
    try {
        fn();
    } catch (const std::exception& error) {
        kLog.error_with(error, message);
    } catch (...) {
        kLog.error("{}", message);
    }
}

// _provider_is_remote: whether a text provider sends text off this computer. A provider with no
// profile (an installed agent, a deleted endpoint), or one whose lookup fails, counts as remote.
bool providerIsRemote(const std::string& provider, const Json& settings) {
    try {
        const std::optional<llm::TextLLMProfile> profile = llm::get_profile(provider, settings);
        return !profile.has_value() || !profile->is_local();
    } catch (const std::exception& error) {
        kLog.warning("Couldn't look up text provider '{}'; treating it as remote ({})", provider,
                     type_display_name(typeid(error)));
        return true;
    }
}

// The old Basic tabs' keys, now the cards a deep link scrolls to (SECTIONS).
std::string_view sectionCard(std::string_view key) {
    if (key == destinations::basic_dictation) {
        return "dictation";
    }
    if (key == destinations::basic_meetings) {
        return "meetings";
    }
    if (key == destinations::basic_app) {
        return "app";
    }
    return {};
}

QString hfPolicyLabel(std::string_view policy) {
    if (policy == "always") {
        return QStringLiteral("always download");
    }
    if (policy == "never") {
        return QStringLiteral("offline");
    }
    return QStringLiteral("ask first");
}

// resolve_ui_theme: the saved theme; with none saved, Omarchy's own on an Omarchy desktop.
QString resolvedTheme(const Json& settings) {
    if (!(settings.is_object() && settings.contains(keys::ui_theme.key())) && use_omarchy_ui()) {
        return QStringLiteral("omarchy");
    }
    return q(keys::ui_theme.read(settings));
}

HomePage::Resolver boolOf(const BoolSetting& setting) {
    return [&setting](const Json& settings) {
        return QVariant(setting.read(settings));
    };
}

bool isSwitch(const QWidget* control) {
    return qobject_cast<const SettingsSwitch*>(control) != nullptr;
}

} // namespace

// --- StorageBar ---

StorageBar::StorageBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("homeStorageBar"));
    setFixedHeight(8);
}

void StorageBar::setSegments(const std::vector<std::int64_t>& sizes) {
    std::int64_t total = 0;
    for (const std::int64_t size : sizes) {
        total += size;
    }
    segments_.clear();
    constexpr std::size_t count = std::size(kBackendTokens);
    for (std::size_t index = 0; index < sizes.size(); ++index) {
        if (total != 0 && sizes[index] != 0) {
            segments_.emplace_back(static_cast<double>(sizes[index]) / static_cast<double>(total),
                                   kBackendTokens[index % count]);
        }
    }
    update();
}

void StorageBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF bounds(rect());
    QPainterPath clip;
    clip.addRoundedRect(bounds, bounds.height() / 2, bounds.height() / 2);
    painter.setClipPath(clip);
    painter.fillRect(bounds, ui::tokenColor("slate-raised"));
    double x = bounds.left();
    for (const auto& [fraction, token] : segments_) {
        const double width = bounds.width() * fraction;
        painter.fillRect(QRectF(x, bounds.top(), width, bounds.height()), ui::tokenColor(token));
        x += width;
    }
}

// --- HomeMicrophoneCombo ---

HomeMicrophoneCombo::HomeMicrophoneCombo(std::function<void()> beforePopup, QWidget* parent)
    : ElidingComboBox(parent), beforePopup_(std::move(beforePopup)) {}

void HomeMicrophoneCombo::showPopup() {
    if (beforePopup_) {
        guarded("Couldn't list the microphones for Home", beforePopup_);
    }
    ElidingComboBox::showPopup();
}

// --- HomeCard ---

HomeCard::HomeCard(const QString& title, const QString& icon, QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("homeCard"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Maximum);
    rows_ = new QVBoxLayout(this);
    rows_->setContentsMargins(18, 13, 18, 8);
    rows_->setSpacing(0);
    auto* header = new QHBoxLayout;
    header->setSpacing(8);
    auto* glyph = new QLabel;
    glyph->setObjectName(QStringLiteral("homeCardIcon"));
    glyph->setPixmap(ui::designIcon(icon).pixmap(18, 18));
    header->addWidget(glyph);
    title_ = new QLabel(title);
    title_->setObjectName(QStringLiteral("homeCardTitle"));
    header->addWidget(title_);
    header->addStretch();
    rows_->addLayout(header);
    rows_->addSpacing(4);
}

void HomeCard::addRow(QBoxLayout* row, QWidget* control) {
    if (!rowList_.empty()) {
        auto* divider = new QFrame;
        divider->setObjectName(QStringLiteral("homeDivider"));
        divider->setFixedHeight(1);
        rows_->addWidget(divider);
    }
    rows_->addLayout(row);
    rowList_.emplace_back(row, control);
    stacked_.reset();
    reflow();
}

void HomeCard::reflow() {
    const bool stacked = width() < ui::scaled(kSideBySideWidth);
    if (stacked_ == stacked) {
        return;
    }
    stacked_ = stacked;
    for (const auto& [row, control] : rowList_) {
        const bool expanding = control->sizePolicy().horizontalPolicy() == QSizePolicy::Expanding;
        row->setDirection(stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
        row->setSpacing(stacked ? 8 : 20);
        row->setStretch(1, expanding && !stacked ? 3 : 0);
        row->setAlignment(control, stacked && !expanding ? Qt::AlignLeft : Qt::AlignVCenter);
    }
}

void HomeCard::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    reflow();
}

void HomeCard::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        stacked_.reset();
        reflow();
    }
}

// --- HomePage ---

HomePage::HomePage(SettingsDialog& dialog, Options options, QWidget* parent)
    : QWidget(parent), dialog_(dialog), options_(options) {
    setObjectName(QStringLiteral("homePage"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    buildIntro(layout);
    layout->addWidget(buildWhereStrip());

    // The cards are made without a parent and become Home's children when reflowColumns places
    // them, after the footer, as OpenWhisper's do; held here until then.
    std::vector<std::unique_ptr<HomeCard>> unplaced;
    const auto make = [this, &unplaced](const char* name, const QString& title,
                                        const QString& icon) {
        unplaced.push_back(std::make_unique<HomeCard>(title, icon));
        cards_[name] = unplaced.back().get();
    };
    make("dictation", QStringLiteral("Dictation"), QStringLiteral("microphone-blue.svg"));
    make("personalize", QStringLiteral("Personalize"), QStringLiteral("wand-purple.svg"));
    make("meetings", QStringLiteral("Meetings"), QStringLiteral("users-amber.svg"));
    make("app", QStringLiteral("App"), QStringLiteral("adjustments-slate.svg"));
    make("storage", QStringLiteral("Storage"), QStringLiteral("download-blue.svg"));
    buildDictation();
    buildContributed(HomeCardId::Personalize);
    buildContributed(HomeCardId::Meetings);
    buildApp();
    buildStorage();

    columnRow_ = new QHBoxLayout;
    columnRow_->setContentsMargins(0, 0, 0, 0);
    columnRow_->setSpacing(kColumnSpacing);
    for (QVBoxLayout*& column : columns_) {
        column = new QVBoxLayout;
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(kColumnSpacing);
        columnRow_->addLayout(column, 1);
    }
    layout->addLayout(columnRow_);
    footer_ = new WrappedLabel(QString());
    footer_->setObjectName(QStringLiteral("homeFooter"));
    layout->addWidget(footer_);
    reflowColumns();
    for (std::unique_ptr<HomeCard>& card : unplaced) {
        // Placed in a column, Home owns it.
        if (card->parentWidget() == this) {
            (void)card.release();
        }
    }
    refresh();
}

HomePage::~HomePage() = default;

HomeCard* HomePage::card(std::string_view name) const {
    const auto found = cards_.find(name);
    return found == cards_.end() ? nullptr : found->second;
}

HomeCard* HomePage::section(std::string_view key) const {
    const std::string_view name = sectionCard(destinations::resolve_destination(key));
    return name.empty() ? nullptr : card(name);
}

HomeCard* HomePage::cardOf(HomeCardId card) const {
    switch (card) {
    case HomeCardId::Dictation:
        return this->card("dictation");
    case HomeCardId::Personalize:
        return this->card("personalize");
    case HomeCardId::Meetings:
        break;
    }
    return this->card("meetings");
}

QWidget* HomePage::control(std::string_view key) const {
    const auto found = controls_.find(key);
    return found == controls_.end() ? nullptr : found->second.data();
}

// --- Layout ---

void HomePage::reflowColumns() {
    const bool two = width() >= ui::scaled(kTwoColumnWidth);
    whereRow_->setDirection(two ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    whereDivider_->setVisible(two);
    if (twoColumns_ == two) {
        return;
    }
    twoColumns_ = two;
    static constexpr std::string_view kLeft[] = {"dictation", "app"};
    static constexpr std::string_view kRight[] = {"personalize", "meetings", "storage"};
    static constexpr std::string_view kAll[] = {"dictation", "personalize", "meetings", "app",
                                                "storage"};
    for (QVBoxLayout* column : columns_) {
        while (QLayoutItem* item = column->takeAt(0)) {
            delete item;
        }
    }
    const auto fill = [this](QVBoxLayout* column, std::span<const std::string_view> names) {
        for (const std::string_view name : names) {
            column->addWidget(card(name));
        }
        column->addStretch();
    };
    if (two) {
        fill(columns_[0], kLeft);
        fill(columns_[1], kRight);
    } else {
        fill(columns_[0], kAll);
        fill(columns_[1], {});
    }
    // One column takes the whole width.
    columnRow_->setStretch(1, two ? 1 : 0);
    columnRow_->setSpacing(two ? kColumnSpacing : 0);
}

void HomePage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    reflowColumns();
}

void HomePage::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if ((event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) &&
        columnRow_ != nullptr) {
        twoColumns_.reset();
        reflowColumns();
    }
}

// --- Rows ---

WrappedLabel* HomePage::addRowTo(HomeCard* card, const QString& title, const QString& description,
                                 QWidget* control) {
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 7, 0, 7);
    row->setSpacing(20);
    auto* copy = new QVBoxLayout;
    copy->setSpacing(3);
    auto* name = new WrappedLabel(title);
    name->setObjectName(QStringLiteral("homeRowTitle"));
    auto* detail = new WrappedLabel(description);
    detail->setObjectName(QStringLiteral("homeRowDescription"));
    copy->addWidget(name);
    copy->addWidget(detail);
    row->addLayout(copy, 1);
    row->addWidget(control, 0, Qt::AlignVCenter);
    control->setAccessibleName(title);
    card->addRow(row, control);
    return detail;
}

WrappedLabel* HomePage::addRow(HomeCardId card, const QString& title, const QString& description,
                               QWidget* control) {
    return addRowTo(cardOf(card), title, description, control);
}

WrappedLabel* HomePage::addRow(HomeCardId card, const QString& title, const QString& description,
                               QWidget* control, std::string destination, std::string attribute,
                               Resolver resolver, std::string key) {
    WrappedLabel* detail = addRow(card, title, description, control);
    bind(control, std::move(destination), std::move(attribute), std::move(resolver),
         std::move(key));
    return detail;
}

void HomePage::addWidget(HomeCardId card, QWidget* widget) {
    cardOf(card)->rows()->addWidget(widget);
}

SettingsSwitch* HomePage::toggle(HomeCardId card, const QString& title, const QString& description,
                                 std::string destination, std::string attribute, Resolver resolver,
                                 std::string key) {
    auto* control = new SettingsSwitch;
    addRow(card, title, description, control, std::move(destination), std::move(attribute),
           std::move(resolver), std::move(key));
    return control;
}

ElidingComboBox* HomePage::combo(bool expanding) {
    auto* made = new ElidingComboBox;
    made->setObjectName(QStringLiteral("homeCombo"));
    made->setMinimumHeight(36);
    made->setMinimumWidth(150);
    if (expanding) {
        made->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    } else {
        made->setMaximumWidth(240);
    }
    return made;
}

void HomePage::bind(QWidget* control, std::string destination, std::string attribute,
                    Resolver resolver, std::string key) {
    if (!key.empty()) {
        controls_[key] = control;
    }
    bindings_.push_back(
        Binding{control, std::move(destination), std::move(attribute), std::move(resolver)});
    const std::size_t index = bindings_.size() - 1;
    const auto changed = [this, index] {
        guarded("Couldn't save a Home setting", [&] { apply(bindings_[index]); });
    };
    if (auto* button = qobject_cast<QAbstractButton*>(control)) {
        connect(button, &QAbstractButton::toggled, this, changed);
    } else if (auto* box = qobject_cast<QComboBox*>(control)) {
        connect(box, &QComboBox::currentIndexChanged, this, changed);
    } else {
        throw std::invalid_argument("A Home projection is a switch or a combo.");
    }
}

void HomePage::registerControl(std::string key, QWidget* control) {
    controls_[std::move(key)] = control;
}

void HomePage::addRefreshHook(std::function<void(const Json&)> hook) {
    refreshHooks_.push_back(std::move(hook));
}

void HomePage::apply(const Binding& binding) {
    QWidget* control = binding.control;
    if (control == nullptr) {
        return;
    }
    auto* button = qobject_cast<QAbstractButton*>(control);
    auto* box = qobject_cast<QComboBox*>(control);
    const QVariant value = button != nullptr ? QVariant(button->isChecked()) : box->currentData();
    // The owning page's own handler runs, with its callbacks, platform restrictions,
    // confirmations and multi-key changes. Building that page re-reads it, so the edit is kept
    // in `value` before the page exists.
    QWidget* source = nullptr;
    if (dialog_.isRegistered(binding.destination)) {
        // A page that cannot be built saves nothing; Home shows the saved value again below.
        guarded("Couldn't build the page a Home setting saves through", [&] {
            dialog_.ensurePage(binding.destination);
            source = dialog_.control(binding.attribute);
        });
    }
    auto* sourceButton = qobject_cast<QAbstractButton*>(source);
    auto* sourceBox = qobject_cast<QComboBox*>(source);
    if (sourceButton != nullptr) {
        sourceButton->setChecked(value.toBool());
    } else if (sourceBox != nullptr) {
        const int index = sourceBox->findData(value);
        if (index >= 0) {
            sourceBox->setCurrentIndex(index);
        }
    }
    refresh();
    // A failed save or a declined confirmation restores the projection and its source, so
    // trying the same change again still emits a signal.
    if (sourceButton != nullptr) {
        const QSignalBlocker blocker(sourceButton);
        sourceButton->setChecked(button != nullptr ? button->isChecked() : value.toBool());
    } else if (sourceBox != nullptr) {
        const QSignalBlocker blocker(sourceBox);
        const QVariant shown = box != nullptr ? box->currentData() : value;
        sourceBox->setCurrentIndex(std::max(0, sourceBox->findData(shown)));
    }
}

QPushButton* HomePage::addLink(HomeCard* card, const QString& text, std::string_view destination) {
    auto* link = new QPushButton(text + QStringLiteral("  →"));
    link->setObjectName(QStringLiteral("homeLink"));
    link->setCursor(Qt::PointingHandCursor);
    const QString key = q(destination);
    connect(link, &QPushButton::clicked, this, [this, key] {
        guarded("Couldn't open a settings page", [&] { dialog_.selectDestination(key); });
    });
    card->rows()->addWidget(link, 0, Qt::AlignLeft);
    // A link to a page this build has not registered would lead nowhere.
    if (!dialog_.isRegistered(destination)) {
        link->hide();
    }
    return link;
}

// --- Cards ---

void HomePage::buildIntro(QVBoxLayout* layout) {
    if (appcore::is_seen(Json(keys::whats_new_seen.read(*dialog_.settingsSnapshot())))) {
        return;
    }
    intro_ = new WhatsNewTile;
    connect(intro_, &WhatsNewTile::dismissRequested, this,
            [this] { guarded("Couldn't hide the note", [this] { dismissIntro(); }); });
    layout->addWidget(intro_);
}

void HomePage::dismissIntro() {
    if (dialog_.persist(keys::whats_new_seen.key(), Json(appcore::whats_new().updated))) {
        intro_->hide();
    }
}

QFrame* HomePage::buildWhereStrip() {
    where_ = new QFrame;
    where_->setObjectName(QStringLiteral("homeWhere"));
    auto* row = new QHBoxLayout(where_);
    whereRow_ = row;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    const auto half = [row](const QString& title, const QString& tone, WrappedLabel*& titleLabel,
                            WrappedLabel*& items) {
        auto* widget = new QWidget;
        widget->setObjectName(QStringLiteral("homeWhereHalf"));
        auto* layout = new QHBoxLayout(widget);
        layout->setContentsMargins(16, 11, 16, 11);
        layout->setSpacing(11);
        auto* dot = new QLabel;
        dot->setObjectName(QStringLiteral("homeDot"));
        ui::setStyleProperty(dot, ui::prop::tone, tone);
        dot->setFixedSize(8, 8);
        auto* dotColumn = new QVBoxLayout;
        dotColumn->setContentsMargins(0, 5, 0, 0);
        dotColumn->addWidget(dot);
        dotColumn->addStretch();
        layout->addLayout(dotColumn);
        auto* copy = new QVBoxLayout;
        copy->setContentsMargins(0, 0, 0, 0);
        copy->setSpacing(2);
        titleLabel = new WrappedLabel(title);
        titleLabel->setObjectName(QStringLiteral("homeWhereTitle"));
        copy->addWidget(titleLabel);
        items = new WrappedLabel(QString());
        items->setObjectName(QStringLiteral("homeWhereList"));
        copy->addWidget(items);
        copy->addStretch();
        layout->addLayout(copy, 1);
        row->addWidget(widget, 1);
    };
    half(QStringLiteral("Stays on this computer"), QStringLiteral("ok"), localTitle_, localList_);
    whereDivider_ = new QFrame;
    whereDivider_->setObjectName(QStringLiteral("homeWhereDivider"));
    whereDivider_->setFixedWidth(1);
    row->addWidget(whereDivider_);
    half(QStringLiteral("Leaves this computer, only when used"), QStringLiteral("warn"),
         cloudTitle_, cloudList_);
    return where_;
}

void HomePage::buildDictation() {
    HomeCard* dictation = card("dictation");
    // A choice here becomes the first microphone in the Recording order. The device list is the
    // Recording page's, read on a worker once this menu first opens, and keeps unavailable saved
    // devices.
    microphone_ = new HomeMicrophoneCombo([this] { loadMicrophones(); });
    microphone_->setObjectName(QStringLiteral("homeCombo"));
    microphone_->setMinimumHeight(36);
    microphone_->setMinimumWidth(150);
    microphone_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    addRowTo(dictation, QStringLiteral("Microphone"),
             QStringLiteral("Used for dictation and meetings."), microphone_);
    bind(
        microphone_, std::string(destinations::recording), "audio_device_combo",
        [](const Json& settings) { return QVariant(q(appcore::mic::preferred_token(settings))); },
        std::string(keys::audio_input_priority.key()));
    bindings_.back().refreshed = false;

    auto* holder = new QWidget;
    holder->setObjectName(QStringLiteral("homeShortcut"));
    auto* shortcutRow = new QHBoxLayout(holder);
    shortcutRow->setContentsMargins(0, 0, 0, 0);
    shortcutRow->setSpacing(8);
    shortcut_ = new ProfileHotkeyInput;
    // As wide as the Command Mode shortcut field, which fits its placeholder.
    shortcut_->setMinimumWidth(ui::scaled(kShortcutMinWidth));
    shortcut_->setMaximumWidth(kShortcutMaxWidth);
    shortcut_->setAccessibleName(QStringLiteral("Recording shortcut"));
    connect(shortcut_, &ProfileHotkeyInput::captureChanged, this,
            [this](bool capturing) { dialog_.setHotkeyCaptureSuspended(capturing); });
    connect(shortcut_, &ProfileHotkeyInput::captured, this, [this](const QString& hotkey) {
        guarded("Couldn't save the recording shortcut", [&] {
            dialog_.setStandardHotkey(QStringLiteral("record_toggle"), hotkey);
            refreshShortcut();
        });
    });
    shortcutRow->addWidget(shortcut_);
    shortcutChange_ = new QPushButton(QStringLiteral("Change"));
    shortcutChange_->setObjectName(QStringLiteral("settingsChangeShortcut"));
    connect(shortcutChange_, &QPushButton::clicked, this, [this] { beginShortcutCapture(); });
    shortcutRow->addWidget(shortcutChange_);
    shortcutDescription_ =
        addRowTo(dictation, QStringLiteral("Recording shortcut"), QString(), holder);

    voiceCombo_ = combo();
    voiceCombo_->setMinimumWidth(200);
    populateBackendCombo(voiceCombo_, model_values());
    voiceDetail_ = addRowTo(dictation, QStringLiteral("Voice model"), QString(), voiceCombo_);
    // Not a bind projection: the choice goes to the engine without building the Voice model
    // page (_change_voice, models.choose_backend).
    registerControl(std::string(keys::selected_model.key()), voiceCombo_);
    connect(voiceCombo_, &QComboBox::currentIndexChanged, this,
            [this] { guarded("Couldn't change voice model", [this] { changeVoice(); }); });

    // The AI cleanup page's rows (M4) sit here, as OpenWhisper's inline cleanup rows do.
    buildContributed(HomeCardId::Dictation);

    auto* paste = new SettingsSwitch;
    pasteDescription_ =
        addRowTo(dictation, QStringLiteral("Paste text automatically"),
                 QStringLiteral("Insert the transcript into the active window."), paste);
    bind(paste, std::string(destinations::general), "auto_paste_check", boolOf(keys::auto_paste),
         std::string(keys::auto_paste.key()));
    toggle(HomeCardId::Dictation, QStringLiteral("Show live transcript"),
           QStringLiteral("See words appear while you speak."),
           std::string(destinations::recording), "streaming_enabled_check",
           boolOf(keys::streaming_enabled), std::string(keys::streaming_enabled.key()));
    if (options_.nativeWayland) {
        // The General page's sentence: Hyprland's paste dispatcher is not ported.
        pasteDescription_->setText(
            QStringLiteral("This Wayland desktop requires manual clipboard paste."));
    }
    addLink(dictation, QStringLiteral("All dictation settings"), destinations::recording);
}

void HomePage::buildContributed(HomeCardId id) {
    if (id != HomeCardId::Dictation) {
        HomeCard* target = cardOf(id);
        // Page modules own their Home rows; each adds them only if it has any.
        if (const HomeRows* rows = dialog_.context().find<HomeRows>()) {
            for (const HomeRows::Contributor& contributor : rows->contributors(id)) {
                guarded("Couldn't add a page's Home rows", [&] { contributor(*this); });
            }
        }
        const std::string_view destination =
            id == HomeCardId::Personalize ? destinations::dictionary : destinations::meeting_voice;
        addLink(target,
                id == HomeCardId::Personalize
                    ? QStringLiteral("Dictionary, snippets, styles and commands")
                    : QStringLiteral("All meeting settings"),
                destination);
        // A card without rows whose link leads nowhere would be an empty frame.
        if (target->rowCount() == 0 && !dialog_.isRegistered(destination)) {
            target->hide();
        }
        return;
    }
    if (const HomeRows* rows = dialog_.context().find<HomeRows>()) {
        for (const HomeRows::Contributor& contributor : rows->contributors(id)) {
            guarded("Couldn't add a page's Home rows", [&] { contributor(*this); });
        }
    }
}

void HomePage::buildApp() {
    HomeCard* app = card("app");
    struct Choice {
        const char* title;
        const char* description;
        enums::EnumId id;
        std::string_view key;
        const char* attribute;
        Resolver resolver;
    };
    const Choice choices[] = {
        {"Theme", "Dark, light, or match your operating system.", enums::EnumId::UiTheme,
         keys::ui_theme.key(), "ui_theme_combo",
         [](const Json& settings) {
             return QVariant(resolvedTheme(settings));
         }},
        {"Font size", "Text size in windows and dialogs.", enums::EnumId::UiFontScale,
         keys::ui_font_scale.key(), "ui_font_scale_combo",
         [](const Json& settings) {
             return QVariant(static_cast<int>(keys::ui_font_scale.read(settings)));
         }},
    };
    for (const Choice& choice : choices) {
        ElidingComboBox* box = combo();
        for (const Json& value : enums::values(choice.id)) {
            const QString label = q(enums::label(choice.id, value).value_or(std::string()));
            if (value.is_number_integer()) {
                box->addItem(label, static_cast<int>(value.get<std::int64_t>()));
            } else {
                box->addItem(label, q(value.get<std::string>()));
            }
        }
        addRowTo(app, QString::fromUtf8(choice.title), QString::fromUtf8(choice.description), box);
        bind(box, std::string(destinations::appearance), choice.attribute, choice.resolver,
             std::string(choice.key));
    }
    const bool tray = options_.trayAvailable;
    SettingsSwitch* traySwitch = new SettingsSwitch;
    addRowTo(app, QStringLiteral("Keep running when I close the window"),
             QStringLiteral("Keep hotkeys available in the system tray."), traySwitch);
    bind(
        traySwitch, std::string(destinations::general), "minimize_tray_check",
        [tray](const Json& settings) {
            return QVariant(tray && keys::minimize_tray.read(settings));
        },
        std::string(keys::minimize_tray.key()));
    traySwitch->setEnabled(tray);
    if (!tray) {
        traySwitch->setToolTip(
            QStringLiteral("This desktop session does not provide a system tray."));
    }
    const auto appToggle = [this, app](const char* title, const char* description,
                                       const char* attribute, const BoolSetting& setting) {
        auto* control = new SettingsSwitch;
        addRowTo(app, QString::fromUtf8(title), QString::fromUtf8(description), control);
        bind(control, std::string(destinations::general), attribute, boolOf(setting),
             std::string(setting.key()));
        return control;
    };
    appToggle("Check for updates automatically",
              "Ask GitHub for the latest release at launch and every 4 hours. Sends the app "
              "version, no IDs.",
              "update_check_check", keys::update_check_enabled);
    updateNotify_ = appToggle("Notify me about updates",
                              "Show an Update button when a new version is available.",
                              "update_notify_check", keys::update_notify_enabled);
    appToggle("Ask me to restart after an update",
              "Offer a restart when the app was updated while it was running.",
              "update_restart_prompt_check", keys::update_restart_prompt_enabled);
    addLink(app, QStringLiteral("Overlay, hotkeys and all app settings"), destinations::appearance);
}

void HomePage::buildStorage() {
    HomeCard* storage = card("storage");
    QVBoxLayout* group = storage->rows();
    storageValue_ = new QLabel(QString());
    storageValue_->setObjectName(QStringLiteral("homeStorageValue"));
    group->addWidget(storageValue_);
    storageDetail_ = new WrappedLabel(QString());
    storageDetail_->setObjectName(QStringLiteral("homeRowDescription"));
    group->addWidget(storageDetail_);
    group->addSpacing(6);
    storageBar_ = new StorageBar;
    group->addWidget(storageBar_);
    group->addSpacing(4);
    storageLegend_ = new WrappedLabel(QString());
    storageLegend_->setObjectName(QStringLiteral("homeLegend"));
    storageLegend_->setTextFormat(Qt::RichText);
    group->addWidget(storageLegend_);
    group->addSpacing(8);
    components_ = new WrappedLabel(QString());
    components_->setObjectName(QStringLiteral("homeComponents"));
    components_->setTextFormat(Qt::RichText);
    group->addWidget(components_);
    addLink(storage, QStringLiteral("Downloads"), destinations::downloads);
}

void HomePage::beginShortcutCapture() {
    shortcut_->setFocus();
    shortcut_->beginCapture();
}

void HomePage::changeVoice() {
    const QString backend = voiceCombo_->currentData().toString();
    if (dialog_.isBuilt(destinations::voice_model)) {
        // choose_backend's built branch: the page's own engine combo takes the choice, so its
        // handler (the controller's hook, or the save) runs and the page shows it.
        if (auto* source = qobject_cast<QComboBox*>(dialog_.control("engine_combo"))) {
            const int index = source->findData(backend);
            if (index >= 0) {
                source->setCurrentIndex(index);
            }
        }
        refresh();
        return;
    }
    const std::string id = qt::toUtf8(backend);
    const auto known = model_values();
    if (std::ranges::find(known, std::string_view(id)) == known.end()) {
        refresh();
        return;
    }
    try {
        const HomeSources* sources = dialog_.context().find<HomeSources>();
        if (sources != nullptr && sources->backendChanged) {
            sources->backendChanged(backendDisplayName(id));
        } else {
            save_model_selection(dialog_.context().settings(), id);
        }
    } catch (const std::exception& error) {
        kModelsLog.warning("Couldn't change voice model: {}", error.what());
        dialog_.showMessage(
            QStringLiteral("Couldn't change voice model: %1").arg(QString::fromUtf8(error.what())));
        refresh();
        return;
    }
    dialog_.refreshRailValues();
    refresh();
}

// --- State ---

void HomePage::refresh() {
    const std::shared_ptr<const Json> snapshot = dialog_.settingsSnapshot();
    const Json& settings = *snapshot;
    refreshMicrophone(settings);
    for (const Binding& binding : bindings_) {
        QWidget* control = binding.control;
        if (!binding.refreshed || control == nullptr) {
            continue;
        }
        const QSignalBlocker blocker(control);
        const QVariant value = binding.resolver(settings);
        if (auto* button = qobject_cast<QAbstractButton*>(control)) {
            button->setChecked(value.toBool());
        } else if (auto* box = qobject_cast<QComboBox*>(control)) {
            box->setCurrentIndex(std::max(0, box->findData(value)));
        }
    }
    for (const auto& hook : refreshHooks_) {
        guarded("Couldn't refresh a page's Home rows", [&] { hook(settings); });
    }
    {
        const QSignalBlocker blocker(voiceCombo_);
        voiceCombo_->setCurrentIndex(
            std::max(0, voiceCombo_->findData(q(keys::selected_model.read(settings)))));
    }
    const HomeSources* sources = dialog_.context().find<HomeSources>();
    const bool remote =
        sources != nullptr && sources->voiceIsRemote && sources->voiceIsRemote(settings);
    voiceDetail_->setText(remote && sources->voiceDetail
                              ? q(sources->voiceDetail(settings))
                              : QStringLiteral("Audio is transcribed on this computer."));
    refreshShortcut();
    if (QWidget* check = control(keys::update_check_enabled.key())) {
        updateNotify_->setEnabled(qobject_cast<QAbstractButton*>(check)->isChecked());
    }
}

void HomePage::refreshMicrophone(const Json& settings) {
    const QSignalBlocker blocker(microphone_);
    microphone_->clear();
    auto* source = dialog_.isBuilt(destinations::recording)
                       ? qobject_cast<QComboBox*>(dialog_.control("audio_device_combo"))
                       : nullptr;
    const QString preferred = q(appcore::mic::preferred_token(settings));
    if (source == nullptr) {
        // Named as the Recording page's list names it before devices arrive.
        const std::vector<audio::DeviceKey> priority = appcore::mic::saved_priority(settings);
        microphone_->addItem(priority.empty() ? QStringLiteral("System default")
                                              : q(audio::display_name(priority.front().name)),
                             preferred);
        return;
    }
    for (int index = 0; index < source->count(); ++index) {
        microphone_->addItem(source->itemText(index), source->itemData(index));
    }
    microphone_->setCurrentIndex(std::max(0, microphone_->findData(preferred)));
}

void HomePage::loadMicrophones() {
    if (!dialog_.isBuilt(destinations::recording) &&
        dialog_.isRegistered(destinations::recording)) {
        dialog_.ensurePage(destinations::recording);
        refreshMicrophone(*dialog_.settingsSnapshot());
    }
}

Json HomePage::currentHotkeys(const Json& settings) const {
    // current_hotkeys: what the Hotkeys page holds, else the defaults with the saved ones over
    // them.
    if (auto* page = dynamic_cast<HotkeysPage*>(dialog_.pageContent(destinations::hotkeys));
        page != nullptr && !page->currentHotkeys().empty()) {
        return page->currentHotkeys();
    }
    Json hotkeys = default_hotkeys();
    if (settings.is_object()) {
        const auto saved = settings.find(keys::hotkeys.key());
        if (saved != settings.end() && saved->is_object()) {
            for (const auto& [action, value] : saved->items()) {
                hotkeys[action] = value;
            }
        }
    }
    return hotkeys;
}

void HomePage::refreshShortcut() {
    const std::shared_ptr<const Json> snapshot = dialog_.settingsSnapshot();
    const Json& settings = *snapshot;
    const Json hotkeys = currentHotkeys(settings);
    std::string hotkey;
    if (const auto found = hotkeys.find("record_toggle");
        found != hotkeys.end() && found->is_string()) {
        hotkey = found->get<std::string>();
    }
    if (!shortcut_->isCapturing()) {
        shortcut_->setHotkey(q(hotkey));
    }
    QString description;
    if (keys::recording_trigger_mode.read(settings) != "push_hold") {
        description = QStringLiteral("Press to start or stop dictation.");
    } else if (keys::recording_hands_free_latch.read(settings)) {
        description = QStringLiteral("Hold to record, or double-tap to keep recording hands-free.");
    } else {
        description = QStringLiteral("Hold to record; release to stop and transcribe.");
    }
    const std::string note = hotkeys::mouse_shortcut_note(hotkey);
    shortcutDescription_->setText(note.empty() ? description
                                               : description + QLatin1Char(' ') + q(note));
}

HomeSummary HomePage::composeSummary(const Json& settings) const {
    HomeSummary summary;
    const HomeSources* sources = dialog_.context().find<HomeSources>();
    std::vector<HomeWhereItem> items;
    const bool remote =
        sources != nullptr && sources->voiceIsRemote && sources->voiceIsRemote(settings);
    const bool cloud =
        sources != nullptr && sources->voiceIsCloud && sources->voiceIsCloud(settings);
    if (remote && !cloud) {
        // A paired computer: audio leaves this one, but not for a cloud.
        const QString detail =
            sources->voiceDetail ? q(sources->voiceDetail(settings)).toLower() : QString();
        items.push_back({HomeRows::kWhereDictationVoice,
                         QStringLiteral("Dictation voice (%1)").arg(detail), true});
    } else {
        items.push_back(
            {HomeRows::kWhereDictationVoice, QStringLiteral("Dictation voice"), remote});
    }
    // AI cleanup and the text read near the cursor go where the cleanup provider does: text
    // reaches a remote provider only while cleanup is on.
    const bool cleanupOn = text::resolve_level(settings) != text::CleanupLevel::None;
    const bool cleanupRemote =
        providerIsRemote(llm::resolve_transcript_cleanup_provider(settings), settings);
    items.push_back({HomeRows::kWhereCleanup,
                     QStringLiteral("AI cleanup (%1)")
                         .arg(cleanupOn ? QStringLiteral("on") : QStringLiteral("off")),
                     cleanupRemote});
    const bool textReadable = sources != nullptr && sources->textReadingSupported
                                  ? sources->textReadingSupported()
                                  : focus::text_reading_supported();
    if (!text::styles_page::reads_text_near_cursor(settings, textReadable)) {
        items.push_back(
            {HomeRows::kWhereTextNearCursor, QStringLiteral("Text near the cursor (off)"), false});
    } else if (cleanupRemote) {
        items.push_back({HomeRows::kWhereTextNearCursor,
                         QStringLiteral("Text near the cursor (with AI cleanup)"), true});
    } else {
        items.push_back(
            {HomeRows::kWhereTextNearCursor, QStringLiteral("Text near the cursor"), false});
    }
    if (const HomeRows* rows = dialog_.context().find<HomeRows>()) {
        for (const HomeRows::WhereSource& source : rows->whereSources()) {
            guarded("Couldn't read a page's where-strip items", [&] {
                std::vector<HomeWhereItem> more = source(settings);
                items.insert(items.end(), more.begin(), more.end());
            });
        }
    }
    // Learned rules and the recordings never leave this computer.
    items.push_back({HomeRows::kWhereLearnedRules, QStringLiteral("Learned rules"), false});
    items.push_back({HomeRows::kWhereRecordings, QStringLiteral("Recordings"), false});
    std::ranges::stable_sort(items, {}, &HomeWhereItem::order);
    for (const HomeWhereItem& item : items) {
        (item.leaves ? summary.cloudItems : summary.localItems).append(item.text);
    }

    std::optional<models::StorageSummary> storage;
    if (sources != nullptr && sources->storage) {
        guarded("Couldn't read the speech model storage for the overview",
                [&] { storage = sources->storage(); });
    }
    summary.storageChecking = !storage.has_value();
    if (storage) {
        summary.storageDownloaded = storage->downloaded;
        summary.storageTotal = storage->total;
        summary.storageBytes = storage->bytes;
        for (const auto& [label, bytes] : storage->by_backend) {
            summary.storageByBackend.emplace_back(q(label), bytes);
        }
    }
    if (sources != nullptr && sources->components) {
        try {
            for (const models::ComponentInfo& info : sources->components()) {
                summary.components.emplace_back(q(info.display_name), info.is_usable());
            }
        } catch (const std::exception& error) {
            kLog.warning("Couldn't list components for the overview; showing none: {}",
                         error.what());
            summary.components.clear();
        }
    }

    QString apiKeys;
    if (const SettingsPageSpec* spec =
            dialog_.context().settingsPages().page(destinations::api_keys);
        spec != nullptr && spec->railValue) {
        guarded("Couldn't read the API keys rail value",
                [&] { apiKeys = spec->railValue(settings); });
    }
    QString runtime;
    if (sources != nullptr && sources->runtimeSummary) {
        guarded("Couldn't read the runtime summary",
                [&] { runtime = q(sources->runtimeSummary(settings)); });
    }
    summary.footer =
        QStringLiteral("API keys: %1  ·  Runtime: %2  ·  Missing models: %3")
            .arg(apiKeys, runtime, hfPolicyLabel(keys::hf_access_policy.read(settings)));
    return summary;
}

void HomePage::refreshHome() {
    refresh();
    updateSummary(composeSummary(*dialog_.settingsSnapshot()));
}

void HomePage::updateSummary(const HomeSummary& summary) {
    const QString separator = QStringLiteral(" · ");
    localList_->setText(summary.localItems.isEmpty() ? QStringLiteral("Nothing")
                                                     : summary.localItems.join(separator));
    cloudList_->setText(summary.cloudItems.isEmpty() ? QStringLiteral("Nothing")
                                                     : summary.cloudItems.join(separator));
    if (summary.storageChecking) {
        storageValue_->setText(QStringLiteral("Checking…"));
        storageDetail_->setText(QStringLiteral("Scanning downloaded models"));
        storageBar_->setSegments({});
        storageLegend_->setText(QString());
    } else {
        storageValue_->setText(summary.storageBytes != 0
                                   ? QStringLiteral("%1 of speech models")
                                         .arg(q(format_size_bytes(summary.storageBytes)))
                                   : QStringLiteral("No speech models downloaded yet"));
        storageDetail_->setText(QStringLiteral("%1 of %2 downloaded")
                                    .arg(summary.storageDownloaded)
                                    .arg(summary.storageTotal));
        std::vector<std::pair<QString, std::int64_t>> ordered = summary.storageByBackend;
        std::ranges::stable_sort(ordered,
                                 [](const auto& a, const auto& b) { return a.second > b.second; });
        std::vector<std::int64_t> sizes;
        QStringList parts;
        constexpr std::size_t count = std::size(StorageBar::kBackendTokens);
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            const auto& [label, size] = ordered[index];
            sizes.push_back(size);
            const QString color = ui::tokenColor(StorageBar::kBackendTokens[index % count]).name();
            parts.append(QStringLiteral("<span style=\"color:%1\">■</span>&nbsp;%2 %3")
                             .arg(color, label, q(format_size_bytes(size))));
        }
        storageBar_->setSegments(sizes);
        storageLegend_->setText(parts.join(QStringLiteral("&nbsp;&nbsp;&nbsp;")));
        storageLegend_->setVisible(!parts.isEmpty());
    }
    int installed = 0;
    const QString ok = ui::tokenColor("success").name();
    const QString off = ui::tokenColor("slate-text-4").name();
    QStringList dots;
    for (const auto& [name, usable] : summary.components) {
        installed += usable ? 1 : 0;
        dots.append(QStringLiteral("<span style=\"color:%1\">●</span>&nbsp;%2")
                        .arg(usable ? ok : off, name));
    }
    const QString heading = summary.components.empty()
                                ? QStringLiteral("<b>Components</b> · none available")
                                : QStringLiteral("<b>Components · %1 of %2 installed</b>")
                                      .arg(installed)
                                      .arg(static_cast<int>(summary.components.size()));
    components_->setText(dots.isEmpty() ? heading
                                        : heading + QStringLiteral("<br>") +
                                              dots.join(QStringLiteral("&nbsp;&nbsp; ")));
    footer_->setText(summary.footer);
}

void HomePage::cancelCapture() {
    if (shortcut_ != nullptr) {
        shortcut_->cancelCapture();
    }
}

QWidget* HomePage::namedControl(std::string_view name) {
    return section(name);
}

// --- Registration ---

void registerHomePage(AppContext& context) {
    if (context.find<HomeRows>() == nullptr) {
        context.provide<HomeRows>(std::make_shared<HomeRows>());
    }
    if (context.find<HomeSources>() == nullptr) {
        context.provide<HomeSources>(std::make_shared<HomeSources>());
    }
    if (context.settingsPages().page(destinations::overview) != nullptr) {
        return;
    }
    SettingsPageSpec spec;
    spec.destination = destinations::overview;
    spec.title = QStringLiteral("Home");
    spec.subtitle = QStringLiteral(
        "Your everyday settings, and where your words go. Everything else is in the sidebar.");
    spec.create = [&context](QWidget* parent) -> QWidget* {
        auto* page = qobject_cast<SettingsPage*>(parent);
        if (page == nullptr || page->dialog() == nullptr) {
            throw std::logic_error("Home is built inside the Settings window.");
        }
        SettingsDialog& dialog = *page->dialog();
        HomePage::Options options;
        if (AppPages* pages = context.find<AppPages>()) {
            options.trayAvailable = pages->resolvedHooks().trayAvailable();
        } else {
            options.trayAvailable = QSystemTrayIcon::isSystemTrayAvailable();
        }
        options.nativeWayland = is_native_wayland_session();
        auto* home = new HomePage(dialog, options, page);
        // The palette is part of the window Home lands in, as OpenWhisper builds it with the
        // dialog.
        if (dialog.findChild<SearchPalette*>() == nullptr) {
            installSearchPalette(dialog);
        }
        return home;
    };
    context.settingsPages().addPage(std::move(spec));
}

} // namespace fv
