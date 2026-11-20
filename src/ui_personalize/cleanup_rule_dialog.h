#pragma once

#include <optional>

#include <QDialog>
#include <QString>

class QTextEdit;

namespace fv {

// Confirms or edits a learned cleanup rule (ui_qt/dialogs/cleanup_rule_dialog.py, object name
// "cleanupRuleDialog"). With an `original` it is the review of a new rule: the polished text is
// offered beside the words the user gave, and each is editable before saving. Without one it
// edits a rule already in the library.
// Affinity: GUI thread.
class CleanupRuleDialog : public QDialog {
    Q_OBJECT
public:
    // rule: the text to confirm or edit (the polished text when confirming). original: the raw
    // instruction the rule was polished from; none when editing an existing rule. When it is
    // set and the polish succeeded, the user can choose the polished rule (recommended) or
    // their own words. notice: a warning line (AI polish unavailable); a notice takes the
    // choice away, as no polished text exists. dictated: the original came, at least partly,
    // from dictation, so it is quoted as said and transcribed rather than typed.
    explicit CleanupRuleDialog(QString rule, std::optional<QString> original = std::nullopt,
                               QString notice = {}, bool dictated = false,
                               QWidget* parent = nullptr);

    // The rule as it stands, trimmed (rule_text).
    QString ruleText() const;
    // Whether the polished rule and the user's words are both offered (_offer_choice).
    bool offersChoice() const { return offerChoice_; }
    QTextEdit* ruleEdit() const { return ruleEdit_; }

private:
    void setupUi(const QString& rule, const std::optional<QString>& original,
                 const QString& notice);
    void acceptAsTyped();
    void acceptPolished();

    QString original_;
    QString polished_;
    bool dictated_;
    bool offerChoice_ = false;
    QTextEdit* ruleEdit_ = nullptr;
};

} // namespace fv
