#pragma once

#include <functional>

#include <QObject>

class QWidget;

namespace fv {

// Calls `callback` whenever `widget` is shown again (command_settings.py ShowWatcher): pages
// re-read their state on every show, because other pages change settings while they are hidden.
// A callback that throws is logged and swallowed, so nothing leaves into Qt's event loop. It
// lives as a child of the widget.
// Affinity: GUI thread.
class ShowWatcher : public QObject {
    Q_OBJECT
public:
    ShowWatcher(QWidget* widget, std::function<void()> callback);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    std::function<void()> callback_;
};

} // namespace fv
