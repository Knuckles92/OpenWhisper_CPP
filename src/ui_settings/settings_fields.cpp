#include "ui_settings/settings_fields.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

#include "ui_widgets/wrapped_label.h"

namespace fv {

QWidget* settingsField(const QString& label, QWidget* widget) {
    auto* wrapper = new QWidget;
    wrapper->setObjectName(QStringLiteral("modelManagerFieldGroup"));
    auto* column = new QVBoxLayout(wrapper);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(5);
    auto* caption = new QLabel(label);
    caption->setObjectName(QStringLiteral("textModelFieldLabel"));
    column->addWidget(caption);
    column->addWidget(widget);
    return wrapper;
}

WrappedLabel* settingsCaption(const QString& text) {
    auto* label = new WrappedLabel(text);
    label->setObjectName(QStringLiteral("infoLabel"));
    return label;
}

QLabel* groupTitle(QVBoxLayout* layout, const QString& text) {
    auto* caption = new QLabel(text);
    caption->setObjectName(QStringLiteral("settingsTileGroupTitle"));
    layout->addWidget(caption);
    return caption;
}

} // namespace fv
