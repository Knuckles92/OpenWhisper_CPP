#include "ui_record/relation_picker.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui_record/detail/code_points.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

using dictation::BatchRelation;

// RELATION_OPTIONS: the relation, the combo text, then the hint shown under it.
struct RelationOption {
    BatchRelation relation;
    const char16_t* text;
    const char16_t* hint;
};

constexpr RelationOption kRelationOptions[] = {
    {BatchRelation::Separate, u"Separate recordings",
     u"Each file is cleaned on its own and saved as its own transcript."},
    {BatchRelation::Sequential, u"Parts of one recording, in order",
     u"Stitched into one transcript; overlap at the seams is removed."},
    {BatchRelation::Custom, u"Custom…",
     u"Describe the files and how the cleanup should treat them."},
};

QString relationValue(BatchRelation relation) {
    return QString::fromUtf8(dictation::to_string(relation).data(),
                             static_cast<qsizetype>(dictation::to_string(relation).size()));
}

QString truncatedSummary(const QString& instructions) {
    QString text = instructions.simplified();
    bool complete = false;
    detail::prefixLength(text, 90, &complete);
    // Over 90 code points: the first 87, right-stripped, and an ellipsis.
    if (!complete) {
        const QString cut = text.left(detail::prefixLength(text, 87, nullptr));
        text = cut.trimmed() + QStringLiteral(u"…");
    }
    return text;
}

} // namespace

RelationPicker::RelationPicker(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("uploadRelationRow"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);

    label_ = new QLabel(QStringLiteral("How these files relate"));
    label_->setObjectName(QStringLiteral("uploadRelationLabel"));
    row->addWidget(label_);

    combo_ = new NoWheelComboBox;
    combo_->setObjectName(QStringLiteral("uploadRelationCombo"));
    for (const RelationOption& option : kRelationOptions) {
        combo_->addItem(QString::fromUtf16(option.text), relationValue(option.relation));
    }
    connect(combo_, &QComboBox::currentIndexChanged, this, [this](int) { onIndexChanged(); });
    row->addWidget(combo_, 1);

    editButton_ = new QPushButton(QStringLiteral("Edit description"));
    editButton_->setObjectName(QStringLiteral("uploadRelationEditButton"));
    editButton_->setFlat(true);
    editButton_->setCursor(Qt::PointingHandCursor);
    connect(editButton_, &QPushButton::clicked, this, &RelationPicker::editRequested);
    editButton_->hide();
    row->addWidget(editButton_);
    layout->addLayout(row);

    hint_ = new WrappedLabel;
    hint_->setObjectName(QStringLiteral("uploadRelationHint"));
    layout->addWidget(hint_);

    applyHint(relation());
}

BatchRelation RelationPicker::relation() const {
    const std::optional<BatchRelation> value =
        dictation::batch_relation_from(combo_->currentData().toString().toStdString());
    return value.value_or(BatchRelation::Separate);
}

void RelationPicker::setRelation(BatchRelation relation) {
    const int index = combo_->findData(relationValue(relation));
    if (index < 0) {
        return;
    }
    combo_->blockSignals(true);
    combo_->setCurrentIndex(index);
    combo_->blockSignals(false);
    applyHint(relation);
}

void RelationPicker::setCustomSummary(const QString& instructions, bool combine) {
    const QString shape = combine ? QStringLiteral("One combined transcript")
                                  : QStringLiteral("One transcript per file");
    const QString text = truncatedSummary(instructions);
    hint_->setText(text.isEmpty() ? shape : shape + QStringLiteral(u"  ·  ") + text);
    editButton_->show();
}

void RelationPicker::setLocked(bool locked) {
    combo_->setEnabled(!locked);
    editButton_->setEnabled(!locked);
}

void RelationPicker::applyHint(BatchRelation relation) {
    for (const RelationOption& option : kRelationOptions) {
        if (option.relation == relation) {
            hint_->setText(QString::fromUtf16(option.hint));
            break;
        }
    }
    editButton_->setVisible(relation == BatchRelation::Custom);
}

void RelationPicker::onIndexChanged() {
    const BatchRelation value = relation();
    applyHint(value);
    Q_EMIT relationChanged(relationValue(value));
}

} // namespace fv
