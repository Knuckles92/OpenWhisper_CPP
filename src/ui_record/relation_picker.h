#pragma once

#include <QString>
#include <QWidget>

#include "dictation/batch.h"

class QLabel;
class QPushButton;

namespace fv {

class NoWheelComboBox;
class WrappedLabel;

// The "how these files relate" preset and its one-line explanation. Affinity: GUI thread.
class RelationPicker : public QWidget {
    Q_OBJECT
public:
    explicit RelationPicker(QWidget* parent = nullptr);

    // The selected preset (Separate when the combo holds none).
    dictation::BatchRelation relation() const;
    // Shows a preset as selected without announcing it. A value the combo does not hold is
    // ignored.
    void setRelation(dictation::BatchRelation relation);
    // The Custom preset's hint: "One combined transcript" or "One transcript per file", then
    // "  ·  " and the description with whitespace collapsed and cut to 87 characters plus an
    // ellipsis when it is over 90. Shows the Edit description button.
    void setCustomSummary(const QString& instructions, bool combine);
    void setLocked(bool locked);

    QLabel* label() const { return label_; }
    NoWheelComboBox* combo() const { return combo_; }
    QPushButton* editButton() const { return editButton_; }
    WrappedLabel* hint() const { return hint_; }

Q_SIGNALS:
    // The user chose a preset: its stored value ("separate", "sequential" or "custom").
    void relationChanged(const QString& value);
    // Edit description was pressed.
    void editRequested();

private:
    void applyHint(dictation::BatchRelation relation);
    void onIndexChanged();

    QLabel* label_;
    NoWheelComboBox* combo_;
    QPushButton* editButton_;
    WrappedLabel* hint_;
};

} // namespace fv
