#include "ui_widgets/local_model_picker.h"

#include <algorithm>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "models/speech_catalog.h"
#include "ui_widgets/no_wheel.h"

namespace fv {

LocalModelPicker::LocalModelPicker(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("localModelPicker"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);

    modelCombo_ = new ElidingComboBox;
    modelCombo_->setObjectName(QStringLiteral("localModelPickerCombo"));
    modelCombo_->setMinimumHeight(40);
    connect(modelCombo_, &QComboBox::currentIndexChanged, this, [this] { onComboChanged(); });
    row->addWidget(modelCombo_, 1);

    manageButton_ = new QPushButton(QStringLiteral("Manage downloads"));
    manageButton_->setObjectName(QStringLiteral("localModelPickerManage"));
    manageButton_->setFlat(true);
    manageButton_->setCursor(Qt::PointingHandCursor);
    manageButton_->setToolTip(QStringLiteral("Open Downloads to add or delete speech models"));
    connect(manageButton_, &QPushButton::clicked, this, &LocalModelPicker::manageRequested);
    row->addWidget(manageButton_);
    layout->addLayout(row);

    captionLabel_ = new QLabel;
    captionLabel_->setObjectName(QStringLiteral("localModelPickerCaption"));
    captionLabel_->setWordWrap(true);
    layout->addWidget(captionLabel_);
}

std::vector<LocalModelPicker::Model> LocalModelPicker::catalogModels(bool meetingOnly) {
    std::vector<Model> found;
    for (const models::SpeechModel& model : models::models()) {
        if (!meetingOnly || model.meeting) {
            found.push_back({qt::toQString(model.key), qt::toQString(model.label)});
        }
    }
    if (meetingOnly) {
        std::ranges::stable_sort(found, [](const Model& a, const Model& b) {
            return a.label.toCaseFolded() < b.label.toCaseFolded();
        });
    }
    return found;
}

QString LocalModelPicker::currentModel() const {
    const QVariant key = modelCombo_->currentData();
    if (key.metaType().id() == QMetaType::QString && !key.toString().isEmpty()) {
        return key.toString();
    }
    const QString text = modelCombo_->currentText().trimmed();
    return text.isEmpty() ? selected_ : text;
}

void LocalModelPicker::setModels(const std::vector<Model>& models, const QString& selected) {
    const auto listed = [&models](const QString& key) {
        return std::ranges::any_of(models, [&key](const Model& model) { return model.key == key; });
    };
    selected_ = selected;
    if (!listed(selected_) && !models.empty()) {
        selected_ = models.front().key;
    }
    modelCombo_->blockSignals(true);
    modelCombo_->clear();
    for (const Model& model : models) {
        modelCombo_->addItem(model.label, model.key);
        modelCombo_->setItemData(modelCombo_->count() - 1, model.key, Qt::ToolTipRole);
    }
    modelCombo_->setCurrentIndex(std::max(0, modelCombo_->findData(selected_)));
    modelCombo_->blockSignals(false);
    setCaption(QString());
}

void LocalModelPicker::setCurrent(const QString& key) {
    const int index = modelCombo_->findData(key);
    if (index < 0) {
        return;
    }
    selected_ = key;
    modelCombo_->blockSignals(true);
    modelCombo_->setCurrentIndex(index);
    modelCombo_->blockSignals(false);
}

void LocalModelPicker::setCaption(const QString& text) {
    captionLabel_->setText(text);
    captionLabel_->setVisible(!text.isEmpty());
}

void LocalModelPicker::onComboChanged() {
    const QString model = currentModel();
    if (model.isEmpty() || model == selected_) {
        return;
    }
    selected_ = model;
    Q_EMIT modelChanged(model);
}

} // namespace fv
