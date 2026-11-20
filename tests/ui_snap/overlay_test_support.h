#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <tuple>
#include <vector>

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QImage>
#include <QMouseEvent>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QWidget>

#include "pyport/number.h"

// Driving the Snap overlay and toolbar with synthetic input, as tests/test_snap_overlay.py and
// tests/test_snap_session.py do: mouse events are sent straight to a widget at local positions,
// so nothing reaches the real pointer.
namespace fvtest {

// Records each emission of a signal: the arguments as a tuple.
template <class... Args> class Spy {
public:
    template <class Sender, class Signal> Spy(Sender* sender, Signal signal) {
        QObject::connect(sender, signal,
                         [this](const Args&... args) { calls.emplace_back(args...); });
    }
    std::size_t size() const { return calls.size(); }
    std::vector<std::tuple<Args...>> calls;
};

// The fixture's frozen screen: 640 x 400 logical pixels of #f4efe9 at a device-pixel ratio.
inline constexpr int kWidth = 640;
inline constexpr int kHeight = 400;

inline QImage frozenScreen(double dpr = 1.0) {
    QImage image(static_cast<int>(fv::py::round_half_even(kWidth * dpr)),
                 static_cast<int>(fv::py::round_half_even(kHeight * dpr)), QImage::Format_RGB32);
    image.fill(QColor(QStringLiteral("#f4efe9")));
    image.setDevicePixelRatio(dpr);
    return image;
}

inline void mouse(QWidget* widget, QEvent::Type kind, double x, double y,
                  Qt::MouseButtons buttons = Qt::LeftButton,
                  Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const QPointF point(x, y);
    const Qt::MouseButton button = kind == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(kind, point, widget->mapToGlobal(point), button, buttons, modifiers);
    QApplication::sendEvent(widget, &event);
}

// A press, four moves along the way and a release.
inline void drag(QWidget* widget, QPointF start, QPointF end,
                 Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    mouse(widget, QEvent::MouseButtonPress, start.x(), start.y(), Qt::LeftButton, modifiers);
    for (int step = 1; step < 5; ++step) {
        const double t = step / 4.0;
        mouse(widget, QEvent::MouseMove, start.x() + (end.x() - start.x()) * t,
              start.y() + (end.y() - start.y()) * t, Qt::LeftButton, modifiers);
    }
    mouse(widget, QEvent::MouseButtonRelease, end.x(), end.y(), Qt::NoButton, modifiers);
}

inline void click(QWidget* widget, QPointF at, QEvent::Type kind = QEvent::MouseButtonPress) {
    mouse(widget, kind, at.x(), at.y());
    mouse(widget, QEvent::MouseButtonRelease, at.x(), at.y(), Qt::NoButton);
}

inline QPointF center(const QRect& rect) {
    return QPointF(rect.center());
}

inline bool pumpUntil(const std::function<bool()>& predicate,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

inline void pump(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Deletes what deleteLater() queued, as Python's sendPostedEvents(None, DeferredDelete).
inline void flushDeletes() {
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

} // namespace fvtest
