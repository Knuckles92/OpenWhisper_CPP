#pragma once

#include <functional>
#include <optional>

#include <QString>

#include "base/json.h"
#include "ui_personalize/personalize_page.h"

class QPlainTextEdit;
class QTimer;

namespace fv {

class AppContext;
class Button;
class CleanupModelSection;
class CleanupSettings;
class FieldTile;
class HomePage;
class SegmentedBar;
class SettingTile;
class SettingsSwitch;
class WrappedLabel;

// Settings > Dictation > AI cleanup (settings_dialog.py _build_cleanup_page and its handlers):
// the cleanup switch, the level bar with its note, the cleanup model (CleanupModelSection), and
// the optional custom prompt with its editor and Remove buttons.
//
// The bar saves only clicks (activated): loading the saved level never saves it back or asks
// about a custom prompt. While a custom prompt is saved the bar shows Custom, and choosing a
// level asks before removing it; a declined switch puts Custom back on the next turn of the event
// loop, after the bar has selected the clicked segment. The custom prompt is saved when the
// editor loses focus and when the page's drafts are saved (switching pages, closing Settings);
// empty text or a copy of a built-in prompt removes the key instead of saving preset text. With
// cleanup off the level, prompt and Learned rules tiles are disabled; the cleanup model stays
// editable so it can be set up first.
// Affinity: GUI thread.
class CleanupPage : public PersonalizePage {
    Q_OBJECT
public:
    CleanupPage(PersonalizeHost& host, SettingsPage& container, CleanupSettings& cleanup);
    ~CleanupPage() override;

    // The rail's value: the level ("Off", "Light", "Medium", "High", "Custom"), then " · " and
    // the model while cleanup is on and a model is set.
    static QString railValue(const Json& settings);

    // Whether the switch is on.
    bool cleanupEnabled() const;
    // _update_cleanup_prompt_ui's part on this page: the tiles follow the switch, then the level.
    void updateCleanupPromptUi();
    // _update_cleanup_level_ui: the saved level, or Custom while a custom prompt replaces it.
    void updateCleanupLevelUi();
    // _forget_custom_prompt.
    void forgetCustomPrompt();
    // _persist_cleanup_prompt.
    void persistCleanupPrompt();
    // _open_cleanup_prompt_editor and _reset_cleanup_prompt.
    void openCleanupPromptEditor();
    void resetCleanupPrompt();

    SettingTile* cleanupTile() const { return cleanupTile_; }
    SettingsSwitch* cleanupCheck() const;
    SegmentedBar* levelBar() const { return levelBar_; }
    FieldTile* levelTile() const { return levelTile_; }
    WrappedLabel* levelNote() const { return levelNote_; }
    CleanupModelSection* modelSection() const { return model_; }
    QPlainTextEdit* promptEdit() const { return promptEdit_; }
    FieldTile* promptTile() const { return promptTile_; }
    Button* promptEditButton() const { return promptEditButton_; }
    Button* promptResetButton() const { return promptResetButton_; }

    // The prompt editor's seam: the edited text, or nullopt when the editor was cancelled.
    // CleanupPromptDialog by default.
    std::function<std::optional<QString>(const QString& prompt, QWidget* parent)> editPrompt;

    // SettingsPageModule.
    bool saveDrafts(bool ask) override;
    void onShown() override;

protected:
    void load(const Json& settings) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void onCleanupEnabledChanged(bool checked);
    void onLevelActivated(int index);
    void updatePromptButtons();

    CleanupSettings& cleanup_;
    SettingTile* cleanupTile_ = nullptr;
    SegmentedBar* levelBar_ = nullptr;
    FieldTile* levelTile_ = nullptr;
    WrappedLabel* levelNote_ = nullptr;
    CleanupModelSection* model_ = nullptr;
    QPlainTextEdit* promptEdit_ = nullptr;
    FieldTile* promptTile_ = nullptr;
    Button* promptEditButton_ = nullptr;
    Button* promptResetButton_ = nullptr;
    QTimer* levelResync_ = nullptr;
    QString savedPrompt_;
};

// Registers AI cleanup in the context's Settings pages: title, subtitle, its search fields and
// controls, and the rail's level and model.
// Affinity: UI actor.
void registerCleanupPage(AppContext& context);

// Home's Dictation rows of AI cleanup (settings_home.py): "Clean up my dictation" (the page's
// switch), "Cleanup level" (a combo that chooses through CleanupSettings without building the
// page, listing Custom only while a custom prompt is saved) and the status line under them,
// which says where transcript text goes. Whether a cloud provider's key is found is read off
// the GUI thread; until it answers the line says nothing about the key.
void addCleanupHomeRows(HomePage& home);

} // namespace fv
