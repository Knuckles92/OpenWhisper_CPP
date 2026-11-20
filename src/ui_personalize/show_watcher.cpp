#include "ui_personalize/show_watcher.h"

#include <exception>
#include <utility>

#include <QEvent>
#include <QWidget>

#include "base/log.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.widgets.command_settings"};

} // namespace

ShowWatcher::ShowWatcher(QWidget* widget, std::function<void()> callback)
    : QObject(widget), callback_(std::move(callback)) {
    widget->installEventFilter(this);
}

bool ShowWatcher::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Show && callback_) {
        try {
            callback_();
        } catch (const std::exception& error) {
            kLog.error_with(error, "A page could not re-read its state on show");
        } catch (...) {
            kLog.error("A page could not re-read its state on show");
        }
    }
    return QObject::eventFilter(watched, event);
}

} // namespace fv
