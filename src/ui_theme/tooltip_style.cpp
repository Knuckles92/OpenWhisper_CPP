#include "ui_theme/tooltip_style.h"

#include <QApplication>
#include <QEvent>
#include <QStyleFactory>
#include <QWidget>

namespace fv::ui {

SnappyTooltipStyle::SnappyTooltipStyle(QStyle* base) : QProxyStyle(base) {}

int SnappyTooltipStyle::styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget,
                                  QStyleHintReturn* returnData) const {
    if (hint == QStyle::SH_ToolTip_WakeUpDelay) {
        return kWakeUpDelayMs;
    }
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

bool RoundedTooltipFilter::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Polish && watched->isWidgetType() &&
        watched->inherits("QTipLabel")) {
        auto* tip = static_cast<QWidget*>(watched);
        if (QGuiApplication::platformName().toLower().startsWith(QStringLiteral("wayland"))) {
            tip->setStyleSheet(QStringLiteral("QToolTip { border-radius: 0px; }"));
            return false;
        }
        tip->setAttribute(Qt::WA_TranslucentBackground, true);
        // The native drop shadow is drawn for the rectangular window, which would bring the
        // square corners back around the tooltip.
        tip->setWindowFlag(Qt::NoDropShadowWindowHint, true);
    }
    return false;
}

void installTooltipStyle(QApplication& app) {
    auto* filter = new RoundedTooltipFilter(&app);
    app.installEventFilter(filter);
    // The references are rendered with windows11 (Qt's default on Windows, which OpenWhisper
    // wraps as app.style()); where it does not exist, the platform's current style is wrapped.
    QStyle* base = QStyleFactory::create(QString::fromLatin1(kBaseStyle));
    if (base == nullptr) {
        base = QStyleFactory::create(QApplication::style()->name());
    }
    if (base != nullptr) {
        // QApplication takes ownership of the proxy, and the proxy of its base style.
        QApplication::setStyle(new SnappyTooltipStyle(base));
    }
}

} // namespace fv::ui
