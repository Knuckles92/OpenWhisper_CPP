#pragma once

#include <functional>
#include <string>

#include <QString>

#include "base/json.h"
#include "text/dictionary.h"
#include "text/personalize_copy.h"
#include "ui_personalize/personalize_page.h"

class QLabel;
class QLineEdit;

namespace fv {

class AppContext;
class Button;
class CleanupGates;
class DictionaryLibrary;
class HomePage;
class InfoTile;
class SettingTile;
class SettingsDialog;
class SettingsSwitch;
class WrappedLabel;

// What the app's current transcription backend says of itself (the backend provider's answer):
// a backend that is remote and available answers for a paired host's engine whether it takes
// hints; anything else leaves the page going by the selected engine alone.
struct BackendInfo {
    bool remote = false;
    bool available = false;
    // recognition_context's SUPPORT_MODEL or SUPPORT_AFTER.
    std::string support;
    std::string name;
    std::string hostName;
};

// Settings > Personalize > Dictionary (ui_qt/dialogs/settings_dictionary.py _DictionaryPage):
// words dictation should spell your way. A word is saved the moment it is added, through the
// dictionary's own mutators (no Save step); the composer turns into "Edit a word" for a word
// being edited, which ends when that word is saved, cancelled, removed or gone on reload. The
// "AI cleanup is off" gate shows only while cleanup's level is none and the engine cannot take
// the words. The learning switch shows only with "Read text near the cursor" on; otherwise its
// gate's "Apps && styles…" button opens Apps & styles. It re-reads the settings on every show,
// because text reading is switched on another page. Terms are user content: never log them.
// Affinity: GUI thread.
class DictionaryPage : public PersonalizePage {
    Q_OBJECT
public:
    DictionaryPage(PersonalizeHost& host, SettingsPage& container, CleanupGates& gates);
    ~DictionaryPage() override;

    // set_backend_provider: lets the page read the live backend; `provider` runs on the GUI
    // thread. Without one (an empty function) the page goes by the selected engine alone.
    static void setBackendProvider(std::function<BackendInfo()> provider);
    // _engine_support for a settings snapshot and the live backend; the API engine is named
    // after its selected speech endpoint.
    static text::EngineInfo engineInfo(const Json& settings);
    // The Home Dictionary row's line (home_detail) for a settings snapshot.
    static QString homeDetail(const Json& settings);
    // open_composer: shows Dictionary in `dialog` with the word field revealed and focused.
    static void openComposer(SettingsDialog& dialog);

    // The actions behind the page's controls.
    void submit();
    void beginEdit(const QString& termId);
    void cancelEdit();
    void star(const QString& termId, bool starred);
    void remove(const QString& termId);
    void keepNew();
    void moveRules();
    void setSteering(bool checked);
    void setLearning(bool checked);
    void openTextReading();
    // The composer's message line; "" hides it.
    void say(const QString& text, bool error = false);

    InfoTile* cleanupGate() const { return cleanupGate_; }
    InfoTile* composer() const { return composer_; }
    QLineEdit* termEdit() const { return termEdit_; }
    QLineEdit* heardEdit() const { return heardEdit_; }
    Button* addButton() const { return addButton_; }
    Button* cancelButton() const { return cancelButton_; }
    WrappedLabel* message() const { return message_; }
    InfoTile* newTile() const { return newTile_; }
    Button* keepButton() const { return keepButton_; }
    InfoTile* rulesTile() const { return rulesTile_; }
    Button* moveButton() const { return moveButton_; }
    InfoTile* libraryTile() const { return libraryTile_; }
    QLabel* count() const { return count_; }
    DictionaryLibrary* library() const { return library_; }
    SettingTile* steerTile() const { return steerTile_; }
    SettingsSwitch* steerSwitch() const;
    SettingTile* learnTile() const { return learnTile_; }
    SettingsSwitch* learnSwitch() const;
    InfoTile* learnGateTile() const { return learnGateTile_; }
    Button* learnTurnOnButton() const { return learnTurnOn_; }

    // SettingsPageModule: refresh re-reads the saved state.
    void refreshPage() override;

protected:
    void load(const Json& settings) override;

private:
    // show_settings: the saved state, with the loading flag held.
    void showSettings(const Json& settings);
    void reload();
    void endEdit();
    // Runs a dictionary mutator. False once the user has been told why it failed: the refusal's
    // own sentence, else `failure`.
    bool mutate(const std::function<void(Json&)>& change, const QString& failure);

    CleanupGates& gates_;
    std::string editing_;
    Json settings_ = Json::object();

    InfoTile* cleanupGate_ = nullptr;
    InfoTile* composer_ = nullptr;
    QLineEdit* termEdit_ = nullptr;
    QLineEdit* heardEdit_ = nullptr;
    Button* cancelButton_ = nullptr;
    Button* addButton_ = nullptr;
    WrappedLabel* message_ = nullptr;
    InfoTile* newTile_ = nullptr;
    Button* keepButton_ = nullptr;
    InfoTile* rulesTile_ = nullptr;
    Button* moveButton_ = nullptr;
    InfoTile* libraryTile_ = nullptr;
    QLabel* count_ = nullptr;
    DictionaryLibrary* library_ = nullptr;
    SettingTile* steerTile_ = nullptr;
    SettingTile* learnTile_ = nullptr;
    InfoTile* learnGateTile_ = nullptr;
    Button* learnTurnOn_ = nullptr;
};

// Registers Dictionary in the context's Settings pages: title, subtitle, search fields, controls
// and the rail's word count. Its cleanup gate comes from the context's CleanupGates.
// Affinity: UI actor.
void registerDictionaryPage(AppContext& context);

// Home's Personalize row for the dictionary (home_rows): the count and what fixes words now,
// and "Add word", which opens the composer.
// Affinity: GUI thread.
void addDictionaryHomeRows(HomePage& home);

} // namespace fv
