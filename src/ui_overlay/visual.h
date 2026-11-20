#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <QColor>
#include <QPointF>

#include "uimath/look_sims.h"
#include "uimath/overlay_look.h"

class QPainter;

namespace fv {

class ParticleStyle;
class VoiceRibbon;

// What a look reads from the overlay drawing it. Both outlive the look.
struct LookContext {
    const VoiceRibbon* ribbon = nullptr;
    const ParticleStyle* style = nullptr;
    // The overlay's fixed size before any growth (config.WAVEFORM_OVERLAY_WIDTH and _HEIGHT).
    int overlay_width = 0;
    int base_height = 0;
};

// One recording look: how the overlay draws a voice while recording and how the look finishes
// through Processing and Transcribing. Every look but Classic hears loudness through the
// overlay's VoiceRibbon. The width a look draws across is passed to each draw call: the
// overlay's width, or a narrower room when it draws small (Python mutated band_width for that).
// Affinity: GUI thread.
class Visual {
public:
    explicit Visual(const LookContext& context);
    virtual ~Visual() = default;
    Visual(const Visual&) = delete;
    Visual& operator=(const Visual&) = delete;

    // Whether the recording particles are drawn with this look.
    virtual bool particles() const { return true; }
    // Particles emitted a second at rest, or nullopt for the style's own rate.
    virtual std::optional<double> emissionRate() const { return std::nullopt; }
    // How it draws small: kSmallSquash, kSmallShrink or kSmallFit. A fit look draws at the
    // overlay's width whatever room it is given.
    virtual std::string_view small() const { return kSmallShrink; }

    // A new recording starts.
    virtual void reset() {}
    // Where particles launch at x, or empty for the flat line.
    virtual std::function<double(double)> emitter() const { return {}; }
    // One recording tick, after the particles and the ribbon moved.
    virtual void advance(double /*dt*/) {}
    virtual void draw(QPainter& painter, double grow, double width) = 0;

    // The recording ended: keep what the voice last looked like.
    virtual void beginWork();
    // One Processing or Transcribing tick; rate eases up for Transcribing.
    virtual void advanceWork(double /*dt*/, double /*rate*/) {}
    // morph is 0 at the end of the recording and 1 once fully working; clock is seconds of
    // work, run faster while transcribing.
    virtual void drawWork(QPainter& painter, double morph, double clock, double width) = 0;

protected:
    const VoiceRibbon& ribbon() const { return *context_.ribbon; }
    const LookContext& context() const { return context_; }
    // The hue the particles are emitted around right now.
    double hue() const;
    // The frozen voice at 0-1 across.
    double snapAt(double f) const;
    // The voice as beginWork kept it.
    const VoiceSnapshot& snapshot() const { return snapshot_; }

private:
    LookContext context_;
    VoiceSnapshot snapshot_;
};

// The particles' colour family at hue, for the current theme: QColor::fromHsv with alpha set
// to max(0, min(255, int(alpha))). bright=false is the softer dark-theme variant.
QColor hueColor(double hue, double alpha = 255.0, bool bright = true);

// Whether the current palette is the dark one: the looks pick their colours per paint, because
// the theme can change while the overlay is on screen.
bool darkTheme();
// A uimath colour as the QColor Python painted with: one of hue_color's comes back as
// QColor::fromHsv, keeping Qt's 16-bit channels; any other is taken as its 8-bit reading.
QColor toQColor(const Rgba& color);
QPointF toQPointF(const PointD& point);

// The visual for look (normalized first), or null for Classic (recording_looks.make_visual).
std::unique_ptr<Visual> makeVisual(std::string_view look, const LookContext& context);

} // namespace fv
