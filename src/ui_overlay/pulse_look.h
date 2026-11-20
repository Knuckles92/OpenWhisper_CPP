#pragma once

#include "ui_overlay/visual.h"
#include "uimath/look_sims.h"

namespace fv {

// Pulse: a clean orb, a solid core and one crisp ring that follow the voice, with round
// ripples when a word starts. Finish: the ring narrows to a comet arc that spins round the core
// (uimath's PulseSim). Affinity: GUI thread.
class PulseLook final : public Visual {
public:
    using Visual::Visual;

    bool particles() const override { return false; }
    void reset() override { sim_.reset(); }
    void advance(double dt) override;
    void beginWork() override;
    void advanceWork(double dt, double rate) override { sim_.advance_work(dt, rate); }
    void draw(QPainter& painter, double grow, double width) override;
    void drawWork(QPainter& painter, double morph, double clock, double width) override;

    const PulseSim& sim() const { return sim_; }

private:
    PulseSim sim_;
};

} // namespace fv
