#pragma once

#include <functional>
#include <string>
#include <vector>

#include <QFrame>
#include <QString>
#include <QWidget>

#include "base/json.h"

class QLabel;
class QMenu;
class QToolButton;

namespace fv {

class Button;
class FlowLayout;
class SettingsStore;
class WrappedLabel;

// One language of "Languages I dictate in": its name and a button that removes it. Object name
// dictationLanguageChip, 30 px tall. The properties available (the engine can use it; false
// draws the chip dashed and muted) and active (the language the next dictation uses) are what
// the theme draws. Affinity: GUI thread.
class LanguageChip : public QFrame {
    Q_OBJECT
public:
    // unavailableReason becomes the tooltip of a chip the engine cannot use.
    LanguageChip(const QString& code, bool available, bool active, const QString& unavailableReason,
                 QWidget* parent = nullptr);

    const QString& code() const { return code_; }
    QLabel* label() const { return label_; }
    QToolButton* removeButton() const { return removeButton_; }

Q_SIGNALS:
    void removed(const QString& code);

private:
    QString code_;
    QLabel* label_;
    QToolButton* removeButton_;
};

// The languages someone dictates in: removable chips and an Add menu.
//
// Saves dictation_languages (keeping the active language valid) as soon as a chip is added or
// removed, and never writes the engine's own language (local_asr_language). Languages the
// current engine cannot use stay saved and show muted, so switching engines back restores them.
// An engine that takes one language shows why instead of chips. changed() follows a person's
// edit, never a refresh. Affinity: GUI thread.
class DictationLanguagesField : public QWidget {
    Q_OBJECT
public:
    explicit DictationLanguagesField(SettingsStore& settings, QWidget* parent = nullptr);

    // Shows the choices for backend (a selected_model value), which may not be saved yet; an
    // empty id follows the saved engine.
    void setBackend(const QString& backend);
    // Rebuilds the chips and the caption from the saved settings.
    void refresh();

    // A person's edits, each saved at once. A failed save says so in the caption.
    void add(const QString& code);
    void remove(const QString& code);
    // Fills the Add menu with the languages the engine still accepts and pops it up under the
    // Add button.
    void openAddMenu();

    const std::vector<LanguageChip*>& chips() const { return chips_; }
    QWidget* chipArea() const { return chipArea_; }
    Button* addButton() const { return addButton_; }
    QMenu* addMenu() const { return addMenu_; }
    WrappedLabel* captionLabel() const { return caption_; }

    // Which language the next dictation uses, and what adding more does: chosen is the saved
    // list, choices what the engine accepts of it, current the language in effect ("auto" for
    // detection), engineName the engine's display name.
    static QString captionText(const std::vector<std::string>& chosen,
                               const std::vector<std::string>& choices, const std::string& current,
                               const QString& engineName);

Q_SIGNALS:
    void changed();

private:
    Json savedSettings() const;
    void save(const std::function<void(Json&)>& change);

    SettingsStore& settings_;
    QString backend_;
    std::vector<std::string> remaining_;
    std::vector<LanguageChip*> chips_;
    QMenu* addMenu_;
    QWidget* chipArea_;
    FlowLayout* flow_;
    Button* addButton_;
    WrappedLabel* caption_;
};

} // namespace fv
