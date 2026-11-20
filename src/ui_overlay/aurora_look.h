#pragma once

#include "ui_overlay/visual.h"
#include "uimath/look_sims.h"

namespace fv {

// Aurora: three flowing waves whose height and pace follow the voice right now. Finish: the
// three waves twist into one braid that spins (uimath's AuroraSim). Affinity: GUI thread.
class AuroraLook final : public Visual {
public:
    using Visual::Visual;

    bool particles() const override { return false; }
    std::string_view small() const override { return kSmallSquash; }
    void reset() override { sim_.reset(); }
    void advance(double dt) override;
    void advanceWork(double dt, double rate) override { sim_.advance_work(dt, rate); }
    void draw(QPainter& painter, double grow, double width) override;
    void drawWork(QPainter& painter, double morph, double clock, double width) override;

    const AuroraSim& sim() const { return sim_; }

private:
    AuroraSim sim_;
};

} // namespace fv
