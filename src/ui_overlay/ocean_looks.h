#pragma once

#include "ui_overlay/visual.h"
#include "uimath/fizz.h"
#include "uimath/ocean_school.h"

class QLinearGradient;
class QPainterPath;

namespace fv {

// The ocean looks and Fizz (ui_qt/overlays/ocean_looks.py), painted over uimath's SchoolSim and
// FizzSim. The ocean looks are a school of Classic's balls under horizontal layers stacked like
// swells; their motion is worked out at the overlay's width, so they draw small by fitting the
// whole overlay's drawing into the room. Affinity: GUI thread.

// The school and its swell layers, which each ocean look paints in its own way.
class SchoolLook : public Visual {
public:
    SchoolLook(const LookContext& context, SchoolKind kind);

    bool particles() const override { return false; }
    std::string_view small() const override { return kSmallFit; }
    void reset() override { sim_.reset(); }
    void advance(double dt) override;
    void beginWork() override;
    void advanceWork(double dt, double rate) override { sim_.advance_work(dt, rate); }
    // The school draws at the overlay's width, whatever width it is given.
    void draw(QPainter& painter, double grow, double width) final;
    // The look itself, still swimming: the school keeps moving while the work morphs in.
    void drawWork(QPainter& painter, double morph, double clock, double width) override;

    const SchoolSim& sim() const { return sim_; }

protected:
    // The look as it draws while recording, at the overlay's width.
    virtual void drawSchool(QPainter& painter, double grow) = 0;

    // A layer's polyline joined with quadratic curves through the midpoints.
    QPainterPath layerPath(int layer) const;
    QColor ocean(int layer, double alpha, double light = 0.0) const;
    // A layer's colour fading out at both ends.
    QLinearGradient faded(int layer, double alpha, double light = 0.0) const;
    QColor fishColor(const Fish& fish, double alpha = 255.0) const;
    // Each layer as a soft wide line under a bright one.
    void drawLines(QPainter& painter, double grow, double strength = 1.0) const;
    // Filled swell bands; light (0-1, the voice) brightens the water.
    void drawBands(QPainter& painter, double grow, double light = 0.0) const;
    // The fish as Classic's balls: speaking lights them up, each halo growing and brightening.
    void drawBalls(QPainter& painter, double grow) const;

private:
    SchoolSim sim_;
};

// Bait ball: the school mills in a tight spinning ball that swells and rises with the voice; it
// finishes by pulling into the centre and spinning faster under a dome of swell.
class BaitBallLook final : public SchoolLook {
public:
    explicit BaitBallLook(const LookContext& context) : SchoolLook(context, SchoolKind::BaitBall) {}

protected:
    void drawSchool(QPainter& painter, double grow) override;
};

// Real fish: little teardrop fish with wagging tails under glowing bands; they finish by
// wheeling into a slow circle while light shimmers across the water.
class RealFishLook final : public SchoolLook {
public:
    explicit RealFishLook(const LookContext& context) : SchoolLook(context, SchoolKind::RealFish) {}

    void drawWork(QPainter& painter, double morph, double clock, double width) override;

protected:
    void drawSchool(QPainter& painter, double grow) override;

private:
    void drawFish(QPainter& painter, double grow) const;
};

// Deep-sea glow: bioluminescent fish leaving light trails under faint swell lines; they finish
// by spiralling into a glowing whirl that sends out sonar pings.
class DeepGlowLook final : public SchoolLook {
public:
    explicit DeepGlowLook(const LookContext& context) : SchoolLook(context, SchoolKind::DeepGlow) {}

    void drawWork(QPainter& painter, double morph, double clock, double width) override;

protected:
    void drawSchool(QPainter& painter, double grow) override;

private:
    void drawGlow(QPainter& painter, double grow) const;
};

// Fizz: glasses that fill as bubbles race up and reach the surface; the voice sets how many
// launch and how fast, and the level drains in pauses. Finish: the levels roll from glass to
// glass like a filling wave while the bubbles keep fizzing.
class FizzLook final : public Visual {
public:
    explicit FizzLook(const LookContext& context);

    bool particles() const override { return false; }
    std::string_view small() const override { return kSmallFit; }
    void reset() override { sim_.reset(); }
    void advance(double dt) override;
    void beginWork() override;
    void advanceWork(double dt, double rate) override { sim_.advance_work(dt, rate); }
    void draw(QPainter& painter, double grow, double width) override;
    // Drawing the finish sets the glasses' levels, as in Python.
    void drawWork(QPainter& painter, double morph, double clock, double width) override;

    const FizzSim& sim() const { return sim_; }

private:
    void drawGlasses(QPainter& painter, const FizzSim::Levels& levels, double grow,
                     double foamSpeed) const;
    void drawBubbles(QPainter& painter, double grow) const;

    FizzSim sim_;
};

} // namespace fv
