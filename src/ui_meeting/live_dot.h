#pragma once

#include <QWidget>

class QTimer;

namespace fv {

// The recording light (meeting_mode_tab.py LiveDot). It breathes only while audio is being
// captured and the dot is visible: a 50 ms tick moves the phase 0.025, one breath every two
// seconds; otherwise the timer stops and the phase returns to 0. Affinity: GUI thread.
class LiveDot : public QWidget {
    Q_OBJECT
public:
    // Ticks between frames.
    static constexpr int kTickMs = 50;
    // Phase added per tick: 40 ticks make a breath.
    static constexpr double kPhaseStep = 0.025;

    explicit LiveDot(QWidget* parent = nullptr);

    void setLive(bool live);
    bool isLive() const { return live_; }
    // In [0, 1).
    double phase() const { return phase_; }
    QTimer* timer() const { return timer_; }

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void sync();
    void tick();

    bool live_ = false;
    double phase_ = 0.0;
    QTimer* timer_;
};

} // namespace fv
