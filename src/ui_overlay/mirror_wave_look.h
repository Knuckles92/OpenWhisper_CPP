#pragma once

#include "ui_overlay/visual.h"

namespace fv {

// Mirror wave: the voice's outline as a glowing shape, mirrored; the particles rise off its top.
// Finish: the outline gathers into a short glowing capsule that pulses (uimath's
// mirror_wave_frame and mirror_wave_finish). Affinity: GUI thread.
class MirrorWaveLook final : public Visual {
public:
    using Visual::Visual;

    std::string_view small() const override { return kSmallSquash; }
    std::function<double(double)> emitter() const override;
    void draw(QPainter& painter, double grow, double width) override;
    void drawWork(QPainter& painter, double morph, double clock, double width) override;
};

} // namespace fv
