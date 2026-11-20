#include "ui_theme/style_props.h"

#include <QStyle>
#include <QWidget>

namespace fv::ui {

void repolish(QWidget* widget) {
    if (widget == nullptr) {
        return;
    }
    if (QStyle* style = widget->style()) {
        style->unpolish(widget);
        style->polish(widget);
    }
    widget->update();
}

void setStyleProperty(QWidget* widget, const char* prop, const QVariant& value) {
    if (widget == nullptr || widget->property(prop) == value) {
        return;
    }
    widget->setProperty(prop, value);
    repolish(widget);
}

} // namespace fv::ui
