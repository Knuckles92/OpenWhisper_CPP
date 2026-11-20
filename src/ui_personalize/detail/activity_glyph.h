#pragma once

#include <QWidget>

#include "ui_personalize/detail/elapsed_timer.h"
#include "ui_personalize/rule_activity.h"

namespace fv::detail {

// A pulsing live dot while listening; the engine status dot's orbit while working
// (_ActivityGlyph). Nothing is drawn while idle.
// Affinity: GUI thread.
class ActivityGlyph : public QWidget {
    Q_OBJECT
public:
    static constexpr int kSize = 18;
    static constexpr int kPulseMs = 1400;
    // The engine status dot's orbit, so work in flight looks the same everywhere.
    static constexpr double kOrbitMs = 1200.0;

    explicit ActivityGlyph(QWidget* parent, const Clock* clock);

    // Restarts the animation when the state changes.
    void setState(RuleActivityState state);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    RuleActivityState state_ = RuleActivityState::Idle;
    ElapsedTimer clock_;
};

} // namespace fv::detail
