#pragma once

#include <vector>

#include <QFrame>
#include <QString>

#include "text/cleanup_profiles.h"

class QComboBox;
class QPushButton;

namespace fv {

class WrappedLabel;

// Quick Record's "Cleanup profile" card (quick_record_tab.py `profile_card`): the profile combo,
// the button that opens the profile manager and a line saying what the choice does. It shows
// the choices it is given and says what the person did; the tab keeps the selection, the hint
// text and the cleanup checkbox that follows it.
// Affinity: GUI thread.
class CleanupProfileCard : public QFrame {
    Q_OBJECT
public:
    // The combo's first item, whose data is "".
    static constexpr char kStandardName[] = "Standard dictation";

    explicit CleanupProfileCard(QWidget* parent = nullptr);

    // Lists "Standard dictation" and then each profile (data: its id), and selects `selectedId`
    // (the first item when none has it). Announces nothing.
    void setProfiles(const std::vector<text::CleanupProfile>& profiles, const QString& selectedId);
    // The id of the selected profile, "" for standard dictation.
    QString selectedId() const;

    QComboBox* combo() const { return combo_; }
    QPushButton* manageButton() const { return manageButton_; }
    WrappedLabel* hint() const { return hint_; }

Q_SIGNALS:
    // The person chose another entry of the combo.
    void profileChosen();
    // The Manage button was pressed.
    void manageRequested();

private:
    QComboBox* combo_ = nullptr;
    QPushButton* manageButton_ = nullptr;
    WrappedLabel* hint_ = nullptr;
};

} // namespace fv
