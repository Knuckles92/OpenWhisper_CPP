#include "ui_theme/local_style.h"

#include <QApplication>
#include <QVariant>
#include <QWidget>

#include "ui_theme/font_scale.h"
#include "ui_theme/style_props.h"

namespace fv::ui {
namespace {

bool needsResolution(const QString& sheet) {
    return sheet.contains(QStringLiteral("font-size"), Qt::CaseInsensitive) || sheet.contains(u'@');
}

void applyResolved(QWidget* widget, const QString& source, double scale) {
    const QString resolved = resolveStylesheet(source, scale);
    widget->setProperty(prop::scaledStyleSheet, resolved);
    if (widget->styleSheet() != resolved) {
        widget->setStyleSheet(resolved);
    }
}

} // namespace

void setLocalStyleSheet(QWidget* widget, const QString& tokenSource) {
    if (widget == nullptr) {
        return;
    }
    if (tokenSource.isEmpty()) {
        widget->setProperty(prop::unscaledStyleSheet, QVariant());
        widget->setProperty(prop::scaledStyleSheet, QVariant());
        widget->setStyleSheet(QString());
        return;
    }
    widget->setProperty(prop::unscaledStyleSheet, tokenSource);
    applyResolved(widget, tokenSource, currentUiFontScale());
}

void resolveLocalStyleSheets(QApplication& app, double scale) {
    const QWidgetList widgets = app.allWidgets();
    for (QWidget* widget : widgets) {
        QString source = widget->property(prop::unscaledStyleSheet).toString();
        const QString sheet = widget->styleSheet();
        const QString lastResolved = widget->property(prop::scaledStyleSheet).toString();
        // A sheet with tokens or font sizes set directly since the last resolution is designed
        // source, as OpenWhisper's WidgetStyleFilter adopts it when it is set, even on a widget
        // that had a source before.
        const bool setDirectly = !sheet.isEmpty() && needsResolution(sheet) &&
                                 !(!lastResolved.isEmpty() && sheet == lastResolved);
        if (setDirectly) {
            source = sheet;
            widget->setProperty(prop::unscaledStyleSheet, source);
        } else if (source.isEmpty()) {
            continue;
        }
        applyResolved(widget, source, scale);
    }
}

} // namespace fv::ui
