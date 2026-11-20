#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include <QString>
#include <QWidget>

#include "appkit/settings_page.h"
#include "base/json.h"

class QVBoxLayout;

namespace fv {

class Clock;
class ElidingComboBox;
class FieldTile;
class NoWheelSpinBox;
class RecordingLookPreview;
class Recorder;
class SegmentedBar;
class SettingsDialog;
class SettingsPage;
class SettingsPagesSignals;
class SettingsSwitch;

// Settings → App → Appearance (settings_dialog.py _build_appearance_page): the theme and font
// size combos, the recording overlay's look bar with its live preview and label switches, and
// the live preview's layout bar and text size. The bars save on `activated` (a person's click),
// never on an index change, so loading never saves; the label switches are off for Classic,
// which has no live label.
// Affinity: GUI thread.
class AppearancePage : public QWidget, public SettingsPageModule {
    Q_OBJECT
public:
    // recorder: the preview's throwaway recorder on the saved microphone order
    // (_recording_look_recorder); empty hides "Try with my microphone". clock: what the preview
    // animates on (null: the system clock); it must outlive the page.
    AppearancePage(SettingsPage& page, SettingsDialog& dialog, SettingsPagesSignals& signals,
                   std::function<std::unique_ptr<Recorder>(const Json& settings)> recorder,
                   const Clock* clock = nullptr);

    ElidingComboBox* themeCombo() const { return themeCombo_; }
    FieldTile* themeTile() const { return themeTile_; }
    ElidingComboBox* fontScaleCombo() const { return fontScaleCombo_; }
    FieldTile* fontScaleTile() const { return fontScaleTile_; }
    SegmentedBar* lookBar() const { return lookBar_; }
    FieldTile* lookTile() const { return lookTile_; }
    RecordingLookPreview* lookPreview() const { return preview_; }
    QWidget* parts() const { return parts_; }
    SettingsSwitch* dotSwitch() const { return dotSwitch_; }
    SettingsSwitch* textSwitch() const { return textSwitch_; }
    SettingsSwitch* clockSwitch() const { return clockSwitch_; }
    NoWheelSpinBox* fontSizeSpin() const { return fontSizeSpin_; }
    FieldTile* fontSizeTile() const { return fontSizeTile_; }
    SegmentedBar* layoutBar() const { return layoutBar_; }
    FieldTile* layoutTile() const { return layoutTile_; }

    // SettingsPageModule.
    void loadSettings(const Json& settings) override;
    QWidget* namedControl(std::string_view name) override;

private:
    SettingsSwitch* partSwitch(QVBoxLayout* layout, const QString& text, std::string_view key);
    void onPartToggled(std::string_view key, bool on);
    void showOverlayParts(const QString& look = {});
    void onLookActivated(int index);
    void onLayoutActivated(int index);
    void onFontSizeChanged();
    void onThemeChanged();
    void onFontScaleChanged();
    QString savedLook() const;

    SettingsDialog& dialog_;
    SettingsPagesSignals& signals_;
    ElidingComboBox* themeCombo_ = nullptr;
    FieldTile* themeTile_ = nullptr;
    ElidingComboBox* fontScaleCombo_ = nullptr;
    FieldTile* fontScaleTile_ = nullptr;
    SegmentedBar* lookBar_ = nullptr;
    FieldTile* lookTile_ = nullptr;
    RecordingLookPreview* preview_ = nullptr;
    QWidget* parts_ = nullptr;
    SettingsSwitch* dotSwitch_ = nullptr;
    SettingsSwitch* textSwitch_ = nullptr;
    SettingsSwitch* clockSwitch_ = nullptr;
    NoWheelSpinBox* fontSizeSpin_ = nullptr;
    FieldTile* fontSizeTile_ = nullptr;
    SegmentedBar* layoutBar_ = nullptr;
    FieldTile* layoutTile_ = nullptr;
};

} // namespace fv
