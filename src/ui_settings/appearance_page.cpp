#include "ui_settings/appearance_page.h"

#include <algorithm>
#include <exception>
#include <utility>

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "appcore/mic_priority.h"
#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "base/desktop_session.h"
#include "recorder/recorder.h"
#include "settings/enums.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_overlay/recording_look_preview.h"
#include "ui_settings/app_pages.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"
#include "ui_theme/icons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"

namespace fv {
namespace {

using enums::EnumId;

QString q(std::string_view text) {
    return qt::toQString(text);
}

// RecordingOverlayLook.ALL and StreamingOverlayLayout.ALL, in order.
std::vector<QString> looks() {
    std::vector<QString> all;
    for (const std::string_view look : keys::choices::recording_overlay_look) {
        all.push_back(q(look));
    }
    return all;
}

std::vector<QString> layouts() {
    std::vector<QString> all;
    for (const std::string_view layout : keys::choices::streaming_overlay_layout) {
        all.push_back(q(layout));
    }
    return all;
}

// The text an enum's LABELS or DETAILS give a value.
QString entryText(const std::vector<enums::Entry>& entries, const QString& value) {
    for (const auto& entry : entries) {
        if (entry.value.is_string() && q(entry.value.get<std::string>()) == value) {
            return q(entry.text);
        }
    }
    return {};
}

std::vector<std::pair<QString, QString>> barItems(EnumId id, const std::vector<QString>& values) {
    std::vector<std::pair<QString, QString>> items;
    for (const QString& value : values) {
        items.emplace_back(entryText(enums::labels(id), value),
                           entryText(enums::details(id), value));
    }
    return items;
}

int indexOf(const std::vector<QString>& values, const QString& value) {
    const auto found = std::ranges::find(values, value);
    return found == values.end() ? 0 : static_cast<int>(found - values.begin());
}

// resolve_ui_theme: the saved theme; with none saved, Omarchy's own on an Omarchy desktop.
QString savedTheme(const Json& settings) {
    if (!(settings.is_object() && settings.contains(keys::ui_theme.key())) && use_omarchy_ui()) {
        return QStringLiteral("omarchy");
    }
    return q(keys::ui_theme.read(settings));
}

// _select_combo_data: the item carrying value, else the first.
void selectData(QComboBox* combo, const QVariant& value) {
    combo->setCurrentIndex(std::max(0, combo->findData(value)));
}

} // namespace

AppearancePage::AppearancePage(SettingsPage& page, SettingsDialog& dialog,
                               SettingsPagesSignals& signals,
                               std::function<std::unique_ptr<Recorder>(const Json&)> recorder,
                               const Clock* clock)
    : QWidget(&page), dialog_(dialog), signals_(signals) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    themeCombo_ = new ElidingComboBox;
    themeCombo_->setObjectName(QStringLiteral("settingsUiThemeCombo"));
    themeCombo_->setMinimumHeight(40);
    themeCombo_->setMinimumWidth(140);
    for (const std::string_view theme : keys::choices::ui_theme) {
        themeCombo_->addItem(entryText(enums::labels(EnumId::UiTheme), q(theme)), q(theme));
    }
    selectData(themeCombo_, q(keys::ui_theme.read(Json::object())));
    connect(themeCombo_, &QComboBox::currentIndexChanged, this, [this] { onThemeChanged(); });
    themeTile_ = new FieldTile(QStringLiteral("Theme"),
                               QStringLiteral("Dark, light, or match your operating system."),
                               themeCombo_, ui::designIcon(QStringLiteral("palette-purple.svg")));

    fontScaleCombo_ = new ElidingComboBox;
    fontScaleCombo_->setObjectName(QStringLiteral("settingsUiFontScaleCombo"));
    fontScaleCombo_->setMinimumHeight(40);
    fontScaleCombo_->setMinimumWidth(140);
    for (const std::int64_t percent : keys::int_choices::ui_font_scale) {
        QString text;
        for (const auto& entry : enums::labels(EnumId::UiFontScale)) {
            if (entry.value == Json(percent)) {
                text = q(entry.text);
            }
        }
        fontScaleCombo_->addItem(text, static_cast<int>(percent));
    }
    selectData(fontScaleCombo_, static_cast<int>(keys::ui_font_scale.read(Json::object())));
    connect(fontScaleCombo_, &QComboBox::currentIndexChanged, this,
            [this] { onFontScaleChanged(); });
    fontScaleTile_ =
        new FieldTile(QStringLiteral("Font size"),
                      QStringLiteral("Text size in windows and dialogs. The live preview has its "
                                     "own size below."),
                      fontScaleCombo_, ui::designIcon(QStringLiteral("typography-blue.svg")));
    page.addTileGroup(layout, QStringLiteral("Theme and text"), {themeTile_, fontScaleTile_});

    lookBar_ = new SegmentedBar(barItems(EnumId::RecordingOverlayLook, looks()));
    lookBar_->setAccessibleName(QStringLiteral("Recording overlay look"));
    // activated, not currentIndexChanged: loading the saved look must not save it back.
    connect(lookBar_, &SegmentedBar::activated, this,
            [this](int index) { onLookActivated(index); });
    std::function<std::unique_ptr<Recorder>()> factory;
    if (recorder) {
        factory = [this, recorder] {
            return recorder(*dialog_.settingsSnapshot());
        };
    }
    preview_ = new RecordingLookPreview(
        factory, [this] { return q(appcore::mic::preferred_name(*dialog_.settingsSnapshot())); },
        nullptr, &dialog_.context().settings(), clock);
    lookTile_ = new FieldTile(
        QStringLiteral("Recording overlay"),
        QStringLiteral("How the overlay near your pointer looks while you speak. Every look but "
                       "Classic moves with your voice; Classic is the original particles."),
        lookBar_, ui::designIcon(QStringLiteral("palette-purple.svg")));
    // The label switches sit beside the preview they change, in a narrow column, so each
    // caption stays next to its switch.
    parts_ = new QWidget;
    parts_->setObjectName(QStringLiteral("recordingLookParts"));
    auto* partsLayout = new QVBoxLayout(parts_);
    partsLayout->setContentsMargins(0, 6, 0, 0);
    partsLayout->setSpacing(6);
    auto* partsTitle = new QLabel(QStringLiteral("Show on the overlay"));
    partsTitle->setObjectName(QStringLiteral("settingsTileFieldLabel"));
    partsLayout->addWidget(partsTitle);
    dotSwitch_ =
        partSwitch(partsLayout, QStringLiteral("Live dot"), keys::recording_overlay_dot.key());
    textSwitch_ = partSwitch(partsLayout, QStringLiteral(u"“Recording” label"),
                             keys::recording_overlay_text.key());
    clockSwitch_ = partSwitch(partsLayout, QStringLiteral("Elapsed time"),
                              keys::recording_overlay_clock.key());
    partsLayout->addStretch(1);
    auto* lookRow = new QHBoxLayout;
    lookRow->setContentsMargins(0, 4, 0, 0);
    lookRow->setSpacing(24);
    lookRow->addWidget(preview_, 1);
    lookRow->addWidget(parts_, 0);
    lookTile_->addBodyLayout(lookRow);

    fontSizeSpin_ = new NoWheelSpinBox;
    fontSizeSpin_->setAccessibleName(QStringLiteral("Live preview text size"));
    fontSizeSpin_->setMinimum(10);
    fontSizeSpin_->setMaximum(48);
    fontSizeSpin_->setSuffix(QStringLiteral(" pt"));
    fontSizeSpin_->setValue(
        static_cast<int>(keys::streaming_overlay_font_size.read(Json::object())));
    fontSizeSpin_->setMinimumHeight(40);
    fontSizeSpin_->setMinimumWidth(120);
    fontSizeSpin_->setKeyboardTracking(false);
    connect(fontSizeSpin_, &QSpinBox::valueChanged, this, [this] { onFontSizeChanged(); });
    // Left enabled while the preview is off: the switch is on another page, and a size picked
    // ahead of time is kept.
    fontSizeTile_ =
        new FieldTile(QStringLiteral("Live preview text size"),
                      QStringLiteral("The words that appear near your pointer as you speak, when "
                                     "Real-time transcription preview is on under Recording."),
                      fontSizeSpin_, ui::designIcon(QStringLiteral("typography-blue.svg")), true);
    layoutBar_ = new SegmentedBar(barItems(EnumId::StreamingOverlayLayout, layouts()), true);
    layoutBar_->setAccessibleName(QStringLiteral("Live preview layout"));
    connect(layoutBar_, &SegmentedBar::activated, this,
            [this](int index) { onLayoutActivated(index); });
    layoutTile_ =
        new FieldTile(QStringLiteral("Live preview layout"),
                      QStringLiteral("Where the look goes once your words appear: shrunk into a "
                                     "header above them, or full size under them."),
                      layoutBar_, ui::designIcon(QStringLiteral("palette-purple.svg")));
    page.addTileGroup(layout, QStringLiteral("Overlay"), {lookTile_, layoutTile_, fontSizeTile_},
                      1);
}

SettingsSwitch* AppearancePage::partSwitch(QVBoxLayout* layout, const QString& text,
                                           std::string_view key) {
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(16);
    auto* label = new QLabel(text);
    row->addWidget(label, 1);
    auto* button = new SettingsSwitch;
    button->setAccessibleName(QStringLiteral("Show the %1").arg(text));
    button->setChecked(true);
    connect(button, &QAbstractButton::toggled, this,
            [this, key](bool on) { onPartToggled(key, on); });
    row->addWidget(button, 0, Qt::AlignVCenter);
    layout->addLayout(row);
    return button;
}

QString AppearancePage::savedLook() const {
    return q(keys::recording_overlay_look.read(*dialog_.settingsSnapshot()));
}

void AppearancePage::onPartToggled(std::string_view key, bool on) {
    showOverlayParts();
    if (!dialog_.persist(key, on)) {
        return;
    }
    Q_EMIT signals_.recordingLookChanged(savedLook());
}

void AppearancePage::showOverlayParts(const QString& look) {
    preview_->setLabelParts(dotSwitch_->isChecked(), textSwitch_->isChecked(),
                            clockSwitch_->isChecked());
    // `look` is the one just clicked: the bar's current index changes only after its activated
    // signal.
    const std::vector<QString> all = looks();
    const QString shown = look.isEmpty()
                              ? all[static_cast<std::size_t>(std::max(0, lookBar_->currentIndex()))]
                              : look;
    parts_->setEnabled(shown != QStringLiteral("legacy"));
}

void AppearancePage::onLookActivated(int index) {
    const std::vector<QString> all = looks();
    if (index < 0 || index >= static_cast<int>(all.size())) {
        return;
    }
    const QString look = all[static_cast<std::size_t>(index)];
    preview_->setLook(look);
    showOverlayParts(look);
    if (!dialog_.persist(keys::recording_overlay_look.key(), qt::toUtf8(look))) {
        return;
    }
    Q_EMIT signals_.recordingLookChanged(look);
}

void AppearancePage::onLayoutActivated(int index) {
    const std::vector<QString> all = layouts();
    if (index < 0 || index >= static_cast<int>(all.size())) {
        return;
    }
    const QString chosen = all[static_cast<std::size_t>(index)];
    if (!dialog_.persist(keys::streaming_overlay_layout.key(), qt::toUtf8(chosen))) {
        return;
    }
    Q_EMIT signals_.streamingLayoutChanged(chosen);
}

void AppearancePage::onFontSizeChanged() {
    const int size = fontSizeSpin_->value();
    if (!dialog_.persist(keys::streaming_overlay_font_size.key(), size)) {
        return;
    }
    Q_EMIT signals_.streamingFontChanged(size);
}

void AppearancePage::onThemeChanged() {
    const QString theme = themeCombo_->currentData().toString();
    if (std::ranges::find(keys::choices::ui_theme, qt::toUtf8(theme)) ==
        std::end(keys::choices::ui_theme)) {
        return;
    }
    if (!dialog_.persist(keys::ui_theme.key(), qt::toUtf8(theme))) {
        return;
    }
    Q_EMIT signals_.uiThemeChanged(theme);
}

void AppearancePage::onFontScaleChanged() {
    const QVariant percent = fontScaleCombo_->currentData();
    if (!percent.isValid()) {
        return;
    }
    if (!dialog_.persist(keys::ui_font_scale.key(), percent.toInt())) {
        return;
    }
    Q_EMIT signals_.uiFontScaleChanged(percent.toInt());
}

void AppearancePage::loadSettings(const Json& settings) {
    selectData(fontScaleCombo_, static_cast<int>(keys::ui_font_scale.read(settings)));
    selectData(themeCombo_, savedTheme(settings));
    fontSizeSpin_->setValue(static_cast<int>(keys::streaming_overlay_font_size.read(settings)));
    layoutBar_->setCurrentIndex(
        indexOf(layouts(), q(keys::streaming_overlay_layout.read(settings))));
    const QString look = q(keys::recording_overlay_look.read(settings));
    lookBar_->setCurrentIndex(indexOf(looks(), look));
    preview_->setLook(look);
    dotSwitch_->setChecked(keys::recording_overlay_dot.read(settings));
    textSwitch_->setChecked(keys::recording_overlay_text.read(settings));
    clockSwitch_->setChecked(keys::recording_overlay_clock.read(settings));
    showOverlayParts();
}

QWidget* AppearancePage::namedControl(std::string_view name) {
    const std::pair<std::string_view, QWidget*> controls[] = {
        {"ui_theme_combo", themeCombo_},
        {"ui_theme_tile", themeTile_},
        {"ui_font_scale_combo", fontScaleCombo_},
        {"ui_font_scale_tile", fontScaleTile_},
        {"streaming_layout_tile", layoutTile_},
        {"streaming_layout_bar", layoutBar_},
        {"streaming_font_size_tile", fontSizeTile_},
        {"streaming_font_size_spinbox", fontSizeSpin_},
        {"recording_look_bar", lookBar_},
        {"recording_look_tile", lookTile_},
        {"recording_look_preview", preview_},
        {"recording_parts", parts_},
        {"recording_dot_switch", dotSwitch_},
        {"recording_text_switch", textSwitch_},
        {"recording_clock_switch", clockSwitch_},
    };
    for (const auto& [control, widget] : controls) {
        if (control == name) {
            return widget;
        }
    }
    return nullptr;
}

} // namespace fv
