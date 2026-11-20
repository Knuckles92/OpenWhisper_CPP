#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QObject>
#include <QString>
#include <QStringList>

#include "appkit/qt_bridge.h"
#include "audio/backend.h"
#include "base/json.h"
#include "base/subscription.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QTimer;
class QWidget;

namespace fv {

class Button;
struct CleanupServices;
class EmptyState;
class InfoTile;
class ItemGlow;
class PersonalizePage;
class Recorder;
class RuleActivityStrip;
class RuleTranscriber;

// The control names of the Learned rules section (settings_cleanup_rules.py CONTROLS): the
// Settings window exposes them for deep links, search reveals and tests.
const std::vector<std::string>& cleanupRulesControls();

// What the review dialog is opened with (CleanupRuleDialog's arguments).
struct RuleReview {
    QString rule;
    std::optional<QString> original;
    QString notice;
    bool dictated = false;
};

// Settings > Personalize > Learned rules, below its "AI cleanup is off" gate
// (settings_cleanup_rules.py CleanupRulesSection): the composer that turns a typed or dictated
// instruction into a rule, and the library of saved rules.
//
// A new rule is polished by the AI cleanup model off the GUI thread and reviewed before it is
// saved; a casefold duplicate is a notice ("That rule already exists.") and so is the 51st rule
// ("Rule limit reached (50)."), and neither starts any work. When the polish fails the words are
// kept as given and the review says so. A dictated rule records from the saved microphone order
// for at most 60 s; Stop transcribes it off the GUI thread, Esc throws the take away without
// closing Settings, and closing Settings throws it away too. The words land in the input first
// (after what is typed), so a cancelled review keeps them. Polish, transcription and the
// microphone level come back through the app's GUI executor with request tokens, so a result
// for a closed window or an older request is dropped. Every change to the library is saved at
// once; text, rules and prompts never reach a log.
// Affinity: GUI thread.
class CleanupRulesSection : public QObject {
    Q_OBJECT
public:
    enum class DictationState { Idle, Recording, Transcribing };

    // What the section asks of the page and the window around it.
    struct Hooks {
        // The settings as saved now (polish reads the provider, model and thinking level).
        std::function<Json()> loadSettings;
        // The snapshot the window reads (the microphone order).
        std::function<Json()> settingsSnapshot;
        // Whether AI cleanup is on.
        std::function<bool()> cleanupEnabled;
        // Saves the library as listed.
        std::function<void(const std::vector<std::string>& rules)> persistRules;
        // The rule transcriber, or null while dictation is unavailable.
        std::function<RuleTranscriber*()> transcriber;
        // Where the dictation engine runs, for the activity strip.
        std::function<QString()> transcribeDetail;
        // Whether the Settings window is showing.
        std::function<bool()> isVisible;
        // Gives Esc to the dictation while it records ({} gives it back).
        std::function<void(std::function<void()>)> setEscapeOverride;
    };

    CleanupRulesSection(CleanupServices& services, Hooks hooks, QObject* parent = nullptr);
    ~CleanupRulesSection() override;

    // Adds the composer and library tiles to `page`, each in a tile group of its own.
    void build(PersonalizePage& page);
    // Lists the saved rules.
    void load(const Json& settings);

    DictationState dictationState() const { return dictationState_; }
    bool polishing() const { return polishing_; }
    const std::shared_ptr<Recorder>& recorder() const { return recorder_; }
    // Where the take is saved while it is transcribed (ruleDictationPath() unless set); tests
    // move it into a folder of their own.
    const std::filesystem::path& dictationPath() const { return dictationPath_; }
    void setDictationPath(std::filesystem::path path) { dictationPath_ = std::move(path); }

    // The rules as listed.
    std::vector<std::string> stagedRules() const;
    void updateControls();
    void addRule();
    void polishRule(const QString& raw, bool dictated = false);
    void editRule();
    void deleteRule();
    void toggleDictation();
    void cancelDictation();
    void stopDictation();
    // Settings closed: a take still recording is discarded and the recorder released.
    void releaseRecorder();

    InfoTile* composerTile() const { return composerTile_; }
    QLineEdit* ruleInput() const { return ruleInput_; }
    Button* micButton() const { return micButton_; }
    Button* addButton() const { return addButton_; }
    RuleActivityStrip* activity() const { return activity_; }
    QLabel* status() const { return status_; }
    InfoTile* libraryTile() const { return libraryTile_; }
    QLabel* count() const { return count_; }
    QListWidget* rulesList() const { return rulesList_; }
    EmptyState* empty() const { return empty_; }
    Button* editButton() const { return editButton_; }
    Button* deleteButton() const { return deleteButton_; }
    ItemGlow* glow() const { return glow_; }
    // The widget a control name names; null for any other name.
    QWidget* control(std::string_view name) const;

    // The review dialog's seam: the rule to save, or nullopt when the review was cancelled.
    // CleanupRuleDialog by default.
    std::function<std::optional<QString>(const RuleReview& review, QWidget* parent)> reviewRule;

private:
    void saveRules();
    void setNotice(const QString& text);
    void setMicRecording(bool recording);
    QString polishDetail(const std::string& provider, const std::string& model,
                         const Json& settings) const;
    void onPolished(std::uint64_t token, const QString& raw, const QString& polished,
                    const QString& error);
    void onDictationLevel(std::uint64_t token, double level);
    void onDictationFinished(std::uint64_t token, const QString& text, const QString& error);
    QWidget* dialogParent() const;
    bool alreadyListed(const QString& rule) const;

    CleanupServices& services_;
    Hooks hooks_;
    bool polishing_ = false;
    // Whether the words being polished came (at least partly) from dictation, so the review can
    // say "said" instead of "typed".
    bool polishDictated_ = false;
    DictationState dictationState_ = DictationState::Idle;
    std::shared_ptr<Recorder> recorder_;
    std::vector<audio::DeviceKey> recorderDevice_;
    Subscription levelSubscription_;
    // The request each relayed result must match.
    qt::TokenGate levelGate_;
    qt::TokenGate polishGate_;
    qt::TokenGate dictationGate_;
    std::filesystem::path dictationPath_;
    QTimer* dictationTimer_ = nullptr;
    QWidget* page_ = nullptr;

    InfoTile* composerTile_ = nullptr;
    QLineEdit* ruleInput_ = nullptr;
    Button* micButton_ = nullptr;
    Button* addButton_ = nullptr;
    RuleActivityStrip* activity_ = nullptr;
    QLabel* status_ = nullptr;
    InfoTile* libraryTile_ = nullptr;
    QLabel* count_ = nullptr;
    QListWidget* rulesList_ = nullptr;
    EmptyState* empty_ = nullptr;
    Button* editButton_ = nullptr;
    Button* deleteButton_ = nullptr;
    ItemGlow* glow_ = nullptr;
};

} // namespace fv
