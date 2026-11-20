#pragma once

#include <QImage>
#include <QRectF>
#include <QWidget>

// A click-through screenshot card that flies into the walkthrough column
// (ui_qt/widgets/screenshot_flight.py). Affinity: GUI thread.
namespace fv {

// How long a flight lasts, in seconds.
inline constexpr double kFlightS = 0.68;

// Where the card is at progress (0-1) of a flight from source to target, in global
// coordinates: a cubic ease out along a line lifted 64 px at its middle, its size easing from
// the source's to the target's.
QRectF flightCardRect(double progress, const QRectF& source, const QRectF& target);

// One short flight, driven by the panel's frame timer. Only a thumbnail is retained. The window
// takes neither input nor focus, and resizes itself to the source and target (with room for
// the arc, tilt and shadow) on every frame, so a flight crosses monitors; the panel lands it
// before another capture can see it.
class ScreenshotFlight : public QWidget {
    Q_OBJECT

public:
    explicit ScreenshotFlight(QWidget* panel);

    // The shot in flight (its number as kept), or 0 once landed.
    int number() const { return number_; }
    double progress() const { return progress_; }
    QRectF source() const { return source_; }
    QRectF target() const { return target_; }
    const QImage& image() const { return image_; }

    void start(int number, const QImage& image, const QRectF& source, const QRectF& target);
    void land();
    void advance(double progress, const QRectF& target);
    QRectF cardRect() const;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    int number_ = 0;
    QImage image_;
    QRectF source_;
    QRectF target_;
    double progress_ = 0.0;
};

} // namespace fv
