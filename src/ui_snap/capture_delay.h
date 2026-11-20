#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

#include <QList>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QWidget>

namespace fv {
class SettingsStore;
}

// A quiet delay chip and a nonactivating preset picker for automatic captures
// (ui_qt/widgets/capture_delay.py). The chip sits on the live toolbar and on the Walkthrough tab:
// it shows the delay after each click or mark before an automatic screenshot, or the countdown of
// one that is coming, with a × that cancels it. Clicking it opens the picker of the six presets.
// Both paint with the Snap chrome; the picker keeps itself out of screenshots and never takes the
// focus. Affinity: GUI thread.
namespace fv::ui_snap {

class DelayPicker;

// The presets, in seconds (WALKTHROUGH_CAPTURE_DELAYS).
std::span<const std::int64_t> captureDelays();

// What the chip says: "Snap in 2.4s" while remaining is above 0, else "Delay · 3s" or
// "Delay · Instant".
QString captureDelayText(int seconds, double remaining);
// A countdown's arc in 1/16 degrees, clockwise from 12 o'clock (negative): the share of total
// still left.
int captureDelayArc(double remaining, double total);
// The preset an arrow key moves to from seconds (Right and Up go up, Left and Down down, held at
// either end); seconds must be a preset (std::out_of_range otherwise, Python's ValueError).
int steppedCaptureDelay(int seconds, int key);

class CaptureDelay : public QWidget {
    Q_OBJECT

public:
    // With a parent the chip never takes the focus (the live toolbar); without one it does, so
    // its arrow keys work (the tab). settings is where walkthrough_capture_delay is read and
    // saved; nullptr reads the default and saves nothing.
    explicit CaptureDelay(QWidget* parent = nullptr, SettingsStore* settings = nullptr);
    ~CaptureDelay() override;

    int seconds() const { return seconds_; }
    double remaining() const { return remaining_; }
    double total() const { return total_; }

    void setSeconds(int seconds);
    // Seconds left of total before an automatic screenshot; 0 when none is coming.
    void setCountdown(double remaining, double total = 0.0);
    // The open preset picker, if any: kept out of screenshots like the toolbar.
    QList<QWidget*> popupWindows() const;
    void closePicker();
    DelayPicker* picker() const { return menu_; }

Q_SIGNALS:
    void changed(int seconds);
    void cancelled();

protected:
    bool event(QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void choose(int seconds);
    void updateAccessibleName();

    SettingsStore* settings_ = nullptr;
    int seconds_ = 0;
    double remaining_ = 0.0;
    double total_ = 0.0;
    bool pressed_ = false;
    QPointer<DelayPicker> menu_;
};

// The six presets under the chip, in a floating window that never takes the focus. It closes
// once the pointer has been away from it and from the chip's window for 0.6 s.
class DelayPicker : public QWidget {
    Q_OBJECT

public:
    explicit DelayPicker(CaptureDelay* chip);

    // Shows it under the chip (above when there is no room below), on the chip's screen.
    void open();
    // Preset index's button, in the picker's own pixels; at() is the preset under position or -1.
    static QRectF option(int index);
    static int at(QPointF position);
    int hovered() const { return hover_; }

Q_SIGNALS:
    void chosen(int seconds);

protected:
    void closeEvent(QCloseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void checkPointer();

    CaptureDelay* chip_;
    int hover_ = -1;
    int pressed_ = -1;
    std::optional<std::chrono::steady_clock::time_point> away_;
    QTimer watch_;
};

} // namespace fv::ui_snap
