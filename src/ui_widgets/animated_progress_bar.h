#pragma once

#include <QWidget>

class QTimer;

namespace fv {

// Rounded progress bar with an eased fill and a looping indeterminate sweep (object name
// "animatedProgressBar"). QProgressBar under the app stylesheet draws a flat rectangle that
// snaps between values; an update download is the one place the app asks the user to wait on
// it, so the fill eases toward each reported value and a highlight travels across it, and a
// gradient pill sweeps the track while the work is indeterminate. It animates only while shown.
// Affinity: GUI thread.
class AnimatedProgressBar : public QWidget {
    Q_OBJECT
public:
    static constexpr int kTickMs = 16;

    explicit AnimatedProgressBar(QWidget* parent = nullptr, int barHeight = 8);

    // Whether the bar is sweeping instead of showing a value.
    bool isIndeterminate() const { return indeterminate_; }
    // Latest requested fill, 0.0 to 1.0, ignoring the easing in flight.
    double fraction() const { return target_; }

    // Sweeps the track for work whose size is not known yet.
    void setIndeterminate();
    // Fills to fraction (clamped to 0.0 to 1.0), easing toward it unless animate is false.
    void setFraction(double fraction, bool animate = true);
    // Returns to an empty determinate bar.
    void reset();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void tick();
    void paintSweep(QPainter& painter, const QRectF& rect) const;
    void paintFill(QPainter& painter, const QRectF& rect, double radius) const;

    bool indeterminate_ = true;
    double target_ = 0.0;
    double display_ = 0.0;
    double phase_ = 0.0;
    QTimer* timer_;
};

} // namespace fv
